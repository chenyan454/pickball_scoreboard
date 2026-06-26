/**
 *  onenet.c
 *  ========
 *  Onenet 云平台 MQTT 通信层 — 实现
 *
 *  本模块基于通用 MQTT 模块（my_mqtt），完成：
 *    - 与 OneNET 平台建立连接（mqtt_connect）
 *    - 注册 OneNET 命令下发处理回调
 *    - 数据点上传（JSON / 二进制格式）
 *    - 命令响应（$crsp）
 *    - 物模型属性设置应答（thing/property/set_reply）
 *
 *  【架构】
 *    main.c / 应用层
 *       ↓
 *    onenet.c （本文件 — OneNET 平台特定逻辑）
 *       ↓
 *    my_mqtt.c （通用 MQTT 封装）
 *       ↓
 *    esp_mqtt  （ESP-IDF 内置客户端）
 *
 *  【移植对照】
 *    STM32                        →  ESP32
 *    ────────────────────────────     ───────────────────────────
 *    MQTT_PacketConnect()           mqtt_connect() → esp_mqtt_client_init()
 *    ESP8266_SendData()             mqtt_publish()
 *    ESP8266_GetIPD() → OneNet_RevPro()  mqtt_data_cb_t 回调
 *    MQTT_PacketCmdResp()           onenet_send_cmd_resp()
 *    MQTT_PacketSaveData()          OneNet_SendData()
 *    MQTT_PacketSaveBinData()       OneNet_SendBinData()
 */

/*--------------------------- 头文件 ---------------------------*/
#include "onenet.h"
#include "my_mqtt.h"

// ESP-IDF
#include "esp_log.h"

// cJSON（ESP-IDF 内置，组件名 json）
#include "cJSON.h"

// C 库
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "onenet";

/*--------------------------- OneNET 主题宏 ---------------------------*/

// Onenet 平台 MQTT 主题前缀
#define ONENET_TOPIC_DP_POST        "$sys/" PROID "/" DEVID "/dp/post/json"
#define ONENET_TOPIC_CMD_RESP       "$crsp"         // 命令回复前缀
#define ONENET_TOPIC_CMD_PREFIX     "$creq"         // 命令下发前缀
#define ONENET_TOPIC_DP             "$dp"           // 二进制数据上传主题
#define ONENET_TOPIC_PROPERTY_POST  "$sys/" PROID "/" DEVID "/thing/property/post"

// 物模型 property set/reply 主题
#define ONENET_TOPIC_PROPERTY_SET       "$sys/" PROID "/" DEVID "/thing/property/set"
#define ONENET_TOPIC_PROPERTY_SET_REPLY "$sys/" PROID "/" DEVID "/thing/property/set_reply"

// 辅助宏：数字转字符串
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/*--------------------------- onenet根证书 ---------------------------*/
// 嵌入的根证书起始、结束指针
extern const unsigned char certificate_pem_start[] asm("_binary_certificate_pem_start");
extern const unsigned char certificate_pem_end[] asm("_binary_certificate_pem_end");

/*--------------------------- 模块内部状态 ---------------------------*/

// 用户注册的应用层数据回调（由 main.c 等通过 OneNet_SetDataCallback 注册）
static onenet_data_cb_t app_data_callback = NULL;

// 用户注册的 property/set 回调（由 main.c 等通过 OneNet_SetPropertySetCallback 注册）
static onenet_property_set_cb_t property_set_callback = NULL;


/*========================================================================
 *                    命令响应处理
 *========================================================================*/

/**
 * @brief  构建命令回复 topic 并发布
 *
 *  与 STM32 版 MQTT_PacketCmdResp() 功能等价：
 *  向 $crsp/<cmdid> 回复原始命令内容
 *
 *  @param  cmdid   命令 UUID（来自 $creq/<cmdid>）
 *  @param  req     原始命令 JSON 字符串
 *  @param  req_len 命令长度
 */
static void onenet_send_cmd_resp(const char *cmdid, const char *req, int req_len)
{
    // 构建回复 topic: "$crsp/<cmdid>"
    int topic_len = strlen(ONENET_TOPIC_CMD_RESP) + 1 + strlen(cmdid) + 1;
    char *resp_topic = (char *)malloc(topic_len);
    if (resp_topic == NULL) {
        ESP_LOGE(TAG, "CmdResp: malloc failed");
        return;
    }

    snprintf(resp_topic, topic_len, "%s/%s", ONENET_TOPIC_CMD_RESP, cmdid);

    ESP_LOGI(TAG, "CmdResp: topic=%s, req=%.*s", resp_topic, req_len, req);

    mqtt_publish(resp_topic, req, req_len, 0, 0);

    free(resp_topic);
}


/*========================================================================
 *                    OneNET 内部数据回调
 *========================================================================*/

/**
 * @brief  OneNET 内部数据回调（注册到 MQTT 模块）
 *
 *   本函数等价于 STM32 版 OneNet_RevPro() 的核心逻辑：
 *    - 判断是否为命令下发（$creq 前缀）→ 解析 数据 并回复 $crsp
 *    - 判断是否为物模型属性设置（thing/property/set）→ 调用应用层回调
 *    - 普通消息 → 解析 params 中的 数据
 *    - 最后透传给应用层回调
 */
static void onenet_data_handler(const char *topic, int topic_len,
                                 const char *data, int data_len)
{
    //==========================================================
    //  判断是否为命令下发（topic 以 "$creq" 开头）
    //  对应 STM32 的 case MQTT_PKT_CMD
    //==========================================================
    if (strncmp(topic, ONENET_TOPIC_CMD_PREFIX,
                strlen(ONENET_TOPIC_CMD_PREFIX)) == 0)
    {
        // 提取 cmdid（跳过 "$creq/" 共 6 个字符）
        // STM32 中 cmdid 固定 36 字节（UUID 格式）
        const char *cmdid = topic + strlen(ONENET_TOPIC_CMD_PREFIX) + 1; // +1 跳过 '/'

        ESP_LOGI(TAG, "CMD received: cmdid=%s, req=%.*s",
                 cmdid, data_len, data);

        // ---- 解析命令 JSON
        cJSON *json = cJSON_ParseWithLength(data, data_len);
        if (json != NULL) {
            cJSON *params = cJSON_GetObjectItem(json, "params");
            if (params != NULL) {
                cJSON *A_score = cJSON_GetObjectItem(params, "A_score");
                cJSON *B_score = cJSON_GetObjectItem(params, "B_score");

                if (cJSON_IsNumber(A_score)) // 必须判断类型，防止字段是字符串/空
                {
                    ESP_LOGI(TAG, "CMD: A_score %d", A_score->valueint);
                }

                if (cJSON_IsNumber(B_score))
                {
                    ESP_LOGI(TAG, "CMD: B_score %d", B_score->valueint);
                }
            }
            cJSON_Delete(json);
        }

        // 回复命令（与 STM32 MQTT_PacketCmdResp + ESP8266_SendData 等价）
        onenet_send_cmd_resp(cmdid, data, data_len);
    }
    //==========================================================
    //  判断是否为物模型 property/set（属性设置下发）
    //  对应 OneNET Studio thing/property/set 主题
    //==========================================================
    else if (strncmp(topic, ONENET_TOPIC_PROPERTY_SET,
                     strlen(ONENET_TOPIC_PROPERTY_SET)) == 0)
    {
        ESP_LOGI(TAG, "PropertySet received: data=%.*s", data_len, data);

        // 解析 set 请求 JSON，提取 id 和 params
        cJSON *json = cJSON_ParseWithLength(data, data_len);
        if (json != NULL) {
            cJSON *id_item = cJSON_GetObjectItem(json, "id");
            const char *msg_id = cJSON_IsString(id_item)
                                 ? cJSON_GetStringValue(id_item) : NULL;

            cJSON *params = cJSON_GetObjectItem(json, "params");
            if (params != NULL) {
                cJSON *A_score = cJSON_GetObjectItem(params, "A_score");
                cJSON *B_score = cJSON_GetObjectItem(params, "B_score");

                if (cJSON_IsNumber(A_score)) {
                    ESP_LOGI(TAG, "PropertySet: A_score=%d", A_score->valueint);
                }
                if (cJSON_IsNumber(B_score)) {
                    ESP_LOGI(TAG, "PropertySet: B_score=%d", B_score->valueint);
                }
            }

            // 调用应用层 property/set 回调
            if (property_set_callback && msg_id) {
                property_set_callback(msg_id, data, data_len);
            }

            cJSON_Delete(json);
        }
    }
    //==========================================================
    //  普通 Publish 消息
    //  对应 STM32 的 case MQTT_PKT_PUBLISH
    //==========================================================
    else
    {
        // 尝试解析 JSON 中的 params（与 STM32 逻辑一致）
        cJSON *json = cJSON_ParseWithLength(data, data_len);
        if (json != NULL) {
            cJSON *params = cJSON_GetObjectItem(json, "params");
            if (params != NULL) {
               cJSON *A_score = cJSON_GetObjectItem(params, "A_score");
                cJSON *B_score = cJSON_GetObjectItem(params, "B_score");

                if (cJSON_IsNumber(A_score)) // 必须判断类型，防止字段是字符串/空
                {
                    ESP_LOGI(TAG, "PUBLISH: A_score %d", A_score->valueint);
                }

                if (cJSON_IsNumber(B_score))
                {
                    ESP_LOGI(TAG, "PUBLISH: B_score %d", B_score->valueint);
                }
            }
            cJSON_Delete(json);
        }
    }

    // 透传给应用层回调
    if (app_data_callback) {
        app_data_callback(topic, topic_len, data, data_len);
    }
}


/*========================================================================
 *                    公共 API 实现
 *========================================================================*/

/**
 * @brief  与 Onenet 平台建立 MQTT 连接
 */
int8_t OneNet_DevLink(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "OneNet_DevLink");
    ESP_LOGI(TAG, "  PROID: %s", PROID);
    ESP_LOGI(TAG, "  DEVID: %s", DEVID);
    ESP_LOGI(TAG, "========================================");

    // 构建 MQTT 连接配置
    mqtt_config_t cfg = {
        .uri           = URL,
        .username      = PROID,
        .password      = TOKEN,
        .client_id     = DEVID,
		.certificate   = (const char *)certificate_pem_start,
		.common_name   =COMMON_NAME,
        .keepalive     = 120,
        .disable_clean_session = false,
        .will_topic    = NULL,
        .will_message  = NULL,
        .will_qos      = 0,
        .will_retain   = 0,
        // 心跳保活：每 40 秒向属性上报 topic 发送空数据，维持链路活跃
        .hb_topic      = ONENET_TOPIC_PROPERTY_POST,
        .hb_message    = "{\"id\":\"1\",\"version\":\"1.0\",\"params\":{}}",
        .hb_interval_ms = 40000,
		
		//开启自动重连机制，重连的时间频率为800ms
		.disable_auto_reconnect=false,
		.reconnect_timeout_ms=800,
    };

    // 先注册 OneNET 内部回调，避免连接后数据到达时回调尚未就绪
    mqtt_set_data_callback(onenet_data_handler);

    if (!mqtt_connect(&cfg)) {
        ESP_LOGE(TAG, "OneNet_DevLink: MQTT 连接失败");
        return false;
    }

    ESP_LOGI(TAG, "OneNet_DevLink: MQTT 连接成功");
    return true;
}

/**
 * @brief  获取底层 esp_mqtt 客户端句柄
 */
esp_mqtt_client_handle_t OneNet_GetClient(void)
{
    return mqtt_get_client();
}

/**
 * @brief  订阅 topic
 */
void OneNet_Subscribe(const char *topics[], unsigned char topic_cnt)
{
    if (!mqtt_is_connected()) {
        ESP_LOGE(TAG, "OneNet_Subscribe: MQTT not connected");
        return;
    }

    for (unsigned char i = 0; i < topic_cnt; i++) {
        ESP_LOGI(TAG, "Subscribe Topic: %s", topics[i]);
        int msg_id = mqtt_subscribe(topics[i], 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "WARN: Subscribe failed for %s", topics[i]);
        } else {
            ESP_LOGI(TAG, "Tips: Subscribe OK, msg_id=%d", msg_id);
        }
    }
}

/**
 * @brief  发布消息到指定 topic
 */
void OneNet_Publish(const char *topic, const char *msg)
{
    if (!mqtt_is_connected()) {
        ESP_LOGE(TAG, "OneNet_Publish: MQTT not connected");
        return;
    }

    ESP_LOGI(TAG, "Publish Topic: %s, Msg: %s", topic, msg);

    int msg_id = mqtt_publish(topic, msg, strlen(msg), 0, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "WARN: Publish failed");
    } else {
        ESP_LOGI(TAG, "Tips: Publish OK, msg_id=%d", msg_id);
    }
}

/**
 * @brief  上传数据点到 Onenet 平台
 */
int OneNet_SendData(const char *json_buf)
{
    if (!mqtt_is_connected()) {
        ESP_LOGE(TAG, "OneNet_SendData: MQTT not connected");
        return -1;
    }

    ESP_LOGI(TAG, "SendData: %s", json_buf);

    int msg_id = mqtt_publish(ONENET_TOPIC_DP_POST,
                               json_buf,
                               strlen(json_buf),
                               1,   // Qos1（与 STM32 MQTT_QOS_LEVEL1 一致）
                               0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "WARN: SendData failed");
    } else {
        ESP_LOGI(TAG, "Tips: SendData OK, msg_id=%d", msg_id);
    }

    return msg_id;
}

/**
 * @brief  上传二进制数据到 Onenet 平台
 */
int OneNet_SendBinData(const char *ds_name, const unsigned char *data, int len)
{
    if (!mqtt_is_connected()) {
        ESP_LOGE(TAG, "OneNet_SendBinData: MQTT not connected");
        return -1;
    }

    // 构建二进制头部 JSON: {"ds_id":"<name>"}
    char bin_head[64];
    snprintf(bin_head, sizeof(bin_head), "{\"ds_id\":\"%s\"}", ds_name);
    int bin_head_len = strlen(bin_head);

    // 计算总 payload 大小
    // type(1) + head_len(2) + head_json + file_len(4) + binary_data
    int payload_size = 1 + 2 + bin_head_len + 4 + len;
    unsigned char *payload = (unsigned char *)malloc(payload_size);
    if (payload == NULL) {
        ESP_LOGE(TAG, "SendBinData: malloc failed");
        return -1;
    }

    int pos = 0;
    payload[pos++] = 2;             // type = 2（二进制）

    payload[pos++] = (bin_head_len >> 8) & 0xFF;  // head_len MSB
    payload[pos++] = bin_head_len & 0xFF;         // head_len LSB

    memcpy(payload + pos, bin_head, bin_head_len);
    pos += bin_head_len;

    payload[pos++] = (len >> 24) & 0xFF;  // file_len
    payload[pos++] = (len >> 16) & 0xFF;
    payload[pos++] = (len >> 8) & 0xFF;
    payload[pos++] = len & 0xFF;

    memcpy(payload + pos, data, len);
    pos += len;

    ESP_LOGI(TAG, "SendBinData: ds_name=%s, total=%d bytes", ds_name, pos);

    int msg_id = mqtt_publish(ONENET_TOPIC_DP,
                               (const char *)payload,
                               pos,
                               1,   // Qos1
                               0);

    free(payload);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "WARN: SendBinData failed");
    } else {
        ESP_LOGI(TAG, "Tips: SendBinData OK, msg_id=%d", msg_id);
    }

    return msg_id;
}

/**
 * @brief  断开 MQTT 连接并释放资源
 */
void OneNet_Disconnect(void)
{
    mqtt_disconnect();
}

/**
 * @brief  注册 MQTT 数据事件回调
 */
void OneNet_SetDataCallback(onenet_data_cb_t cb)
{
    app_data_callback = cb;
}

/**
 * @brief  注册物模型属性设置回调
 *
 *  当收到云平台下发的 thing/property/set 消息时，
 *  OneNET 内部解析后会调用此回调，传入 msg_id 和原始数据。
 *  应用层在回调中处理属性值，然后调用 OneNet_ReplyPropertySet() 回复平台。
 */
void OneNet_SetPropertySetCallback(onenet_property_set_cb_t cb)
{
    property_set_callback = cb;
}

/**
 * @brief  向云平台发送 property/set_reply 应答
 *
 *  回复 topic: $sys/{PROID}/{DEVID}/thing/property/set_reply
 *  回复 payload 格式（params 为强制载体，用于云端数值对齐）:
 *    {
 *      "id":     "<msg_id>",               // 必须，与 set 请求中的 id 一致
 *      "code":   0,                       // 0=成功，其他值表示错误
 *      "msg":    "success",                 // 可选，结果描述
 *      "params": {"A_score":5,"B_score":3}  // 必须，设备端实际生效的数值
 *    }
 *
 *  @param  msg_id       原始 property/set 请求中的消息 ID
 *  @param  code         结果码（0 表示成功）
 *  @param  msg          结果描述字符串（可选，传 NULL 则不填）
 *  @param  params_json  设备端生效的参数 JSON 对象字符串（如 "{\"A_score\":5,\"B_score\":3}"），
 *                       作为强制载体回传云端实现数值对齐，传 NULL 则省略
 *
 *  @return true-发送成功  false-连接断开或构建失败
 */
bool OneNet_ReplyPropertySet(const char *msg_id, int code, const char *msg,
                              const char *params_json)
{
    if (!mqtt_is_connected()) {
        ESP_LOGE(TAG, "ReplyPropertySet: MQTT not connected");
        return false;
    }

    if (msg_id == NULL) {
        ESP_LOGE(TAG, "ReplyPropertySet: msg_id is NULL");
        return false;
    }

    // 构建 reply JSON: {"id":"xxx","code":200,"msg":"success","params":{...}}
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "ReplyPropertySet: cJSON_CreateObject failed");
        return false;
    }

    cJSON_AddStringToObject(root, "id", msg_id);
    cJSON_AddNumberToObject(root, "code", code);
    if (msg != NULL) {
        cJSON_AddStringToObject(root, "msg", msg);
    }

    // params 强制载体：回传设备端实际生效的参数，云端据此做数值对齐
    if (params_json != NULL) {
        cJSON *params = cJSON_Parse(params_json);
        if (params != NULL) {
            cJSON_AddItemToObject(root, "params", params);
        } else {
            ESP_LOGW(TAG, "ReplyPropertySet: params_json parse failed, omit params");
        }
    }

    char *reply_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (reply_str == NULL) {
        ESP_LOGE(TAG, "ReplyPropertySet: cJSON_PrintUnformatted failed");
        return false;
    }

    ESP_LOGI(TAG, "ReplyPropertySet: topic=%s, payload=%s",
             ONENET_TOPIC_PROPERTY_SET_REPLY, reply_str);

    int msg_id_out = mqtt_publish(ONENET_TOPIC_PROPERTY_SET_REPLY,
                                   reply_str,
                                   strlen(reply_str),
                                   0,   // Qos0
                                   0);

    cJSON_free(reply_str);

    if (msg_id_out < 0) {
        ESP_LOGE(TAG, "WARN: ReplyPropertySet publish failed");
        return false;
    }

    ESP_LOGI(TAG, "Tips: ReplyPropertySet OK, msg_id=%d", msg_id_out);
    return true;
}