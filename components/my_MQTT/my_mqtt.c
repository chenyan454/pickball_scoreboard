/**
 *  my_mqtt.c
 *  =========
 *  通用 MQTT 客户端封装层 — 实现
 *
 *  基于 ESP-IDF 内置 esp_mqtt 客户端，提供：
 *    - MQTT 连接/断开/发布/订阅 通用接口
 *    - MQTT 事件处理（透传数据到用户回调）
 *    - 可配置的心跳保活任务
 *
 *  注意：本模块不包含任何 OneNET 平台特定逻辑。
 *        所有收到的 MQTT 数据直接透传给用户回调，
 *        不解析具体协议内容。
 */

#include "my_mqtt.h"
#include "mqtt_client.h"            // ESP-IDF 内置 esp_mqtt 组件

// ESP-IDF
#include "esp_log.h"

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// cJSON
#include "cJSON.h"

// C 库
#include <string.h>
#include <stdlib.h>

static const char *TAG = "my_mqtt";

/*--------------------------- 模块内部状态 ---------------------------*/

// esp_mqtt 客户端句柄
static esp_mqtt_client_handle_t mqtt_client = NULL;

// 用户注册的数据回调
static mqtt_data_cb_t user_data_callback = NULL;

// 连接状态标志
static bool mqtt_connected = false;

// 最近一次 MQTT 错误类型（供外部诊断）
static int last_mqtt_error = 0;

// 心跳配置（从 mqtt_connect() 的 cfg 中复制而来）
static char *hb_topic_copy   = NULL;
static char *hb_message_copy = NULL;
static int   hb_interval_ms  = 40000;
static bool  heartbeat_task_created = false;


/*========================================================================
 *                    心跳保活任务
 *========================================================================*/

/**
 * @brief  长连接保活定时任务
 *
 *  定期向指定 topic 发布心跳消息，避免云端因空闲而断开连接。
 *  topic 和 message 由 mqtt_connect() 时通过 mqtt_config_t 配置。
 */
static void mqtt_heartbeat_task(void *arg)
{
    // 首次心跳延迟 10 秒，给连接建立留出缓冲
    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1) {
        if (mqtt_connected && mqtt_client != NULL) {
            esp_mqtt_client_publish(mqtt_client,
                                    hb_topic_copy,
                                    hb_message_copy,
                                    strlen(hb_message_copy),
                                    0, 0);
            ESP_LOGI(TAG, "Send heartbeat to %s", hb_topic_copy);
        }
        vTaskDelay(pdMS_TO_TICKS(hb_interval_ms));
    }
}


/*========================================================================
 *                    MQTT 事件处理器
 *========================================================================*/

/**
 * @brief  esp_mqtt 事件处理函数
 *
 *  处理 esp_mqtt 客户端的所有事件：
 *    - CONNECTED / DISCONNECTED：更新连接状态
 *    - SUBSCRIBED / PUBLISHED：日志记录
 *    - DATA：透传给用户回调（不解析具体协议内容）
 *    - ERROR：诊断日志
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    /*------- 连接事件 -------*/
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_connected = true;
        // 如果配置了心跳且尚未创建任务，则创建心跳任务
        if (hb_topic_copy != NULL && hb_message_copy != NULL) {
            if (!heartbeat_task_created) {
                xTaskCreate(mqtt_heartbeat_task, "mqtt_hb_task", 2048, NULL, 4, NULL);
                heartbeat_task_created = true;
                ESP_LOGI(TAG, "Heartbeat task created for long connection");
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED (connected=%d)", mqtt_connected);
        // 如果之前从未连接成功，说明是 CONNECT 被拒而非断线
        if (!mqtt_connected) {
            ESP_LOGW(TAG, "  -> 连接被拒绝：请检查用户名/密码/ClientID 是否正确");
        }
        mqtt_connected = false;
        break;

    /*------- 订阅/发布确认（esp_mqtt 自动处理，此处仅日志）-------*/
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    /*------- 数据到达：透传给用户回调 -------*/
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA: topic=%.*s, data_len=%d",
                 event->topic_len, event->topic, event->data_len);

        // 将数据原样透传给用户回调，由上层自行解析
        if (user_data_callback) {
            user_data_callback(event->topic, event->topic_len,
                               event->data, event->data_len);
        }
        break;

    /*------- 错误事件 -------*/
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle) {
            ESP_LOGE(TAG, "  error_type=%d, connect_return_code=%d",
                     event->error_handle->error_type,
                     event->error_handle->connect_return_code);
            last_mqtt_error = event->error_handle->error_type;
            switch (event->error_handle->error_type) {
            case MQTT_ERROR_TYPE_TCP_TRANSPORT:
                ESP_LOGE(TAG, "  -> TCP 传输错误（DNS/网络不通/端口被拒）");
                break;
            case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
                ESP_LOGE(TAG, "  -> 连接被拒绝");
                break;
            default:
                ESP_LOGE(TAG, "  -> 错误类型: %d", event->error_handle->error_type);
                break;
            }
        }
        break;

    default:
        break;
    }
}


/*========================================================================
 *                    公共 API 实现
 *========================================================================*/

/**
 * @brief  连接到 MQTT Broker
 */
bool mqtt_connect(const mqtt_config_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "mqtt_connect: config is NULL");
        return false;
    }

    // 如果已有客户端，先销毁
    if (mqtt_client != NULL) {
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
    }

    // ---- 释放旧的心跳配置 ----
    if (hb_topic_copy != NULL)   { free(hb_topic_copy);   hb_topic_copy   = NULL; }
    if (hb_message_copy != NULL) { free(hb_message_copy); hb_message_copy = NULL; }

    // ---- 保存心跳配置 ----
    if (cfg->hb_topic != NULL && cfg->hb_message != NULL) {
        hb_topic_copy   = strdup(cfg->hb_topic);
        hb_message_copy = strdup(cfg->hb_message);
        hb_interval_ms  = (cfg->hb_interval_ms > 0) ? cfg->hb_interval_ms : 40000;
    }
    heartbeat_task_created = false;

    // ---- 构造 MQTT 客户端配置 ----
    // 注：当前使用 TLS 连接（mqtts.heclouds.com:8883 使用国内CA证书，
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker={
			.address.uri = cfg->uri,
			.verification={
				.certificate=cfg->certificate,
				.common_name=cfg->common_name,
				
			}
		},
        .credentials = {
            .username = cfg->username,
            .authentication.password = cfg->password,
            .client_id = cfg->client_id,
        },
        .session = {
            .keepalive = cfg->keepalive,
            .disable_clean_session = cfg->disable_clean_session,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,

			.last_will = {
            .topic = cfg->will_topic,
            .msg = cfg->will_message,
            .qos = cfg->will_qos,
            .retain = cfg->will_retain,
          },
        },
		.network={
			.disable_auto_reconnect=cfg->disable_auto_reconnect,
			.reconnect_timeout_ms=cfg->reconnect_timeout_ms,

		}
        
    };

    ESP_LOGI(TAG, "MQTT connect: uri=%s", cfg->uri);

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "WARN: MQTT client init failed");
        return false;
    }

    // 注册事件处理
    esp_err_t err = esp_mqtt_client_register_event(mqtt_client,
                                                    ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler,
                                                    NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WARN: register event handler failed");
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        return false;
    }

    // 启动 MQTT 客户端
    err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WARN: MQTT client start failed");
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        return false;
    }

    // 等待连接建立（最大等待约 10 秒）
    int retry = 0;
    while (!mqtt_connected && retry < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }

    if (!mqtt_connected) {
        ESP_LOGE(TAG, "ERR: MQTT 连接失败");
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Tips: MQTT 连接成功");
    return true;
}

/**
 * @brief  断开 MQTT 连接并释放资源
 */
void mqtt_disconnect(void)
{
    if (mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
        ESP_LOGI(TAG, "Disconnect: client destroyed");
    }
}

/**
 * @brief  查询 MQTT 是否已连接
 */
bool mqtt_is_connected(void)
{
    return mqtt_connected && (mqtt_client != NULL);
}

/**
 * @brief  获取底层 esp_mqtt 客户端句柄
 */
esp_mqtt_client_handle_t mqtt_get_client(void)
{
    return mqtt_client;
}

/**
 * @brief  订阅 topic
 */
int mqtt_subscribe(const char *topic, int qos)
{
    if (mqtt_client == NULL || !mqtt_connected) {
        ESP_LOGE(TAG, "mqtt_subscribe: MQTT not connected");
        return -1;
    }

    ESP_LOGI(TAG, "Subscribe Topic: %s", topic);
    int msg_id = esp_mqtt_client_subscribe(mqtt_client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "WARN: Subscribe failed for %s", topic);
    } else {
        ESP_LOGI(TAG, "Tips: Subscribe OK, msg_id=%d", msg_id);
    }
    return msg_id;
}

/**
 * @brief  发布消息
 */
int mqtt_publish(const char *topic, const char *data, int len, int qos, int retain)
{
    if (mqtt_client == NULL || !mqtt_connected) {
        ESP_LOGE(TAG, "mqtt_publish: MQTT not connected");
        return -1;
    }

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, data, len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "WARN: Publish failed (topic=%s)", topic);
    } else {
        ESP_LOGI(TAG, "Tips: Publish OK, msg_id=%d, topic=%s", msg_id, topic);
    }
    return msg_id;
}

/**
 * @brief  注册 MQTT 数据事件回调
 */
void mqtt_set_data_callback(mqtt_data_cb_t cb)
{
    user_data_callback = cb;
}
