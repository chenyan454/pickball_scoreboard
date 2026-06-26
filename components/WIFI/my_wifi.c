/**
 *  my_wifi.c
 *  =========
 *  ESP32 WiFi STA 模式 + Onenet MQTT TCP 传输层
 *
 *  本文件整合了两个层次的功能：
 *   1. ESP32 原生 WiFi STA 连接（使用 esp_wifi 驱动）
 *   2. TCP socket 传输层（替代 STM32 平台的 TCP AT 命令驱动），
 *      提供 TCP_* 系列兼容接口，供 onenet.c 调用
 *
 *  移植自：STM32 温湿度采集系统 (TCP.c)
 *  移植说明：STM32 通过 AT 命令控制 TCP 模块，ESP32 内置 WiFi，
 *           因此 WiFi 部分使用 ESP-IDF 原生 API，TCP 数据收发使用 lwip socket
 */

/*--------------------------- 头文件 ---------------------------*/
#include "my_wifi.h"

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ESP-IDF WiFi
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_err.h"

// lwip socket (用于 Onenet TCP 连接)
#include "lwip/sockets.h"
#include "lwip/netdb.h"


// C 库
#include <string.h>
#include <stdio.h>

/*=================================================================
 *                    第一部分：WiFi STA 模式
 *=================================================================*/

static EventGroupHandle_t s_wifi_event_group;   // 事件组句柄
static const char *TAG = "wifi station";
static int s_retry_num = 0;                     // 重试次数

/**
 * @brief  WiFi 事件回调处理
 *
 *  处理 WiFi 启动、断连、获取 IP 等事件，
 *  通过事件组向主流程通知连接状态
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief  初始化 ESP32 WiFi STA 模式
 *
 *  初始化 LWIP、创建事件循环、配置 WiFi 并连接到指定 AP，
 *  阻塞等待连接成功或失败
 */
bool wifi_init_sta(void)
{
	bool temp=false;
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());                       // 初始化 LWIP
    ESP_ERROR_CHECK(esp_event_loop_create_default());        // 创建默认事件循环
    esp_netif_create_default_wifi_sta();                     // 创建默认 wifi station

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();     // 获取默认 wifi 配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));                    // 初始化 wifi

    esp_event_handler_instance_t instance_any_id;            // 获取所有 wifi 相关事件
    esp_event_handler_instance_t instance_got_ip;            // 获取 STA 拿到 IP 事件
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &instance_any_id
    ));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &instance_got_ip
    ));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));       // 设置为 STA 模式
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config)); // 设置 STA 配置
    ESP_ERROR_CHECK(esp_wifi_start());                       // 启动 wifi

    ESP_LOGI(TAG, "wifi_init_sta finished. SSID:%s password:%s",
             CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
				 temp=true;
				  // 强制关闭WiFi所有省电策略，杜绝休眠丢包
					esp_wifi_set_ps(WIFI_PS_NONE);
					
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
    return temp;
}
