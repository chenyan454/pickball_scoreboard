#ifndef _MY_WIFI_H_
#define _MY_WIFI_H_

#include <stdbool.h>

/*--------------------------- WiFi 配置 ---------------------------*/
#define DEFAULT_SCAN_LIST_SIZE      12
#define CONFIG_ESP_WIFI_SSID        "chen40"
#define CONFIG_ESP_WIFI_PASSWORD    "123456chen"

/*--------------------------- 事件标志位 ---------------------------*/
#define WIFI_CONNECTED_BIT          BIT0        // 连接成功标志位
#define WIFI_FAIL_BIT               BIT1        // 连接失败标志位

#define CONFIG_ESP_MAXIMUM_RETRY    5           // 最大重试次数

/*--------------------------- 接收状态标志 ---------------------------*/
#define REV_OK                      1           // 接收完成标志
#define REV_WAIT                    0           // 接收未完成标志

/*--------------------------- WiFi STA 模式 ---------------------------*/
bool wifi_init_sta(void);

/*========================================================================
 * 以下为 TCP 传输层接口（替代原 STM32 平台的 TCP AT 命令驱动）
 * 在 ESP32 平台上使用 lwip socket 直接与 Onenet MQTT 服务器通信
 *========================================================================*/


#endif
