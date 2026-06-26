/**
 *  onenet.h
 *  ========
 *  Onenet 云平台 MQTT 通信层 — 接口定义
 *
 *  本模块基于通用 MQTT 模块（my_mqtt），提供 OneNET 平台特定的：
 *    - 连接/订阅/发布操作
 *    - 数据点上传（JSON / 二进制）
 *    - 平台命令下发处理（$creq / $crsp）
 *    - 心跳保活（thing/property/post）
 *
 *  底层使用 ESP-IDF 内置 esp_mqtt 客户端。
 */

#ifndef _ONENET_H_
#define _ONENET_H_

#include <stdbool.h>
#include "mqtt_client.h"      // esp_mqtt_client_handle_t

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------- Onenet 平台信息 ---------------------------*/
#define PROID           "97zB3KY04H"
#define TOKEN           "version=2018-10-31&res=products%2F97zB3KY04H%2Fdevices%2Fwoker1&et=1844918626&method=md5&sign=HfjsSJIsUJRxHsFw%2FTdCuA%3D%3D"
#define DEVID           "woker1"
//#define URL             "mqtt://mqtts.heclouds.com:1883"
//尝试使用单向TLS连接云平台
#define URL             "mqtts://mqttstls.heclouds.com:8883"
/*--------------------------- Onenet 服务器信息 ---------------------------*/
//#define ONENET_HOST     "mqtts.heclouds.com"
//#define ONENET_PORT     1883
#define COMMON_NAME     "OneNET MQTTS"

/*--------------------------- MQTT 数据事件回调 ---------------------------*/
/**
 * @brief  MQTT 数据事件回调类型
 * @param  topic      消息主题
 * @param  topic_len  主题长度
 * @param  data       消息数据
 * @param  data_len   数据长度
 */
typedef void (*onenet_data_cb_t)(const char *topic, int topic_len,
                                  const char *data, int data_len);

/**
 * @brief  物模型属性设置回调类型
 *
 *  当收到云平台下发的 thing/property/set 消息时触发。
 *  应用层应在此回调中解析属性值、执行设备操作，
 *  然后调用 OneNet_ReplyPropertySet() 回复平台以实现数值对齐。
 *
 * @param  msg_id    消息 ID（来自 set 请求的 "id" 字段，回复时必须原样返回）
 * @param  data      原始 JSON 数据
 * @param  data_len  数据长度
 */
typedef void (*onenet_property_set_cb_t)(const char *msg_id,
                                          const char *data, int data_len);

/*--------------------------- Onenet 平台操作接口 ---------------------------*/

/**
 * @brief  注册 MQTT 数据事件回调
 *         收到平台下发的命令/数据时，OneNET 内部处理完后会调用此回调
 */
void OneNet_SetDataCallback(onenet_data_cb_t cb);

/**
 * @brief  与 Onenet 平台建立 MQTT 连接
 *         内部使用通用 MQTT 模块，自动处理 TCP/MQTT 协议
 *         调用前需确保 WiFi 已通过 wifi_init_sta() 完成连接
 * @return true-成功  false-失败
 */
int8_t OneNet_DevLink(void);

/**
 * @brief  获取底层 esp_mqtt 客户端句柄
 *         供看门狗等模块使用
 */
esp_mqtt_client_handle_t OneNet_GetClient(void);

/**
 * @brief  订阅 topic
 * @param  topics     订阅的 topic 数组
 * @param  topic_cnt  topic 个数
 */
void OneNet_Subscribe(const char *topics[], unsigned char topic_cnt);

/**
 * @brief  发布消息到指定 topic
 * @param  topic  发布的主题
 * @param  msg    消息内容（字符串）
 */
void OneNet_Publish(const char *topic, const char *msg);

/**
 * @brief  注册物模型属性设置回调
 *
 *  当收到云平台下发的 thing/property/set 消息时，
 *  OneNET 内部解析后会调用此回调，传入 msg_id 和原始数据。
 *  应用层在回调中处理属性值，然后调用 OneNet_ReplyPropertySet() 回复平台。
 *
 * @param  cb  回调函数指针（传 NULL 可取消注册）
 */
void OneNet_SetPropertySetCallback(onenet_property_set_cb_t cb);

/**
 * @brief  向云平台发送 property/set_reply 应答
 *
 *  回复 topic: $sys/{PROID}/{DEVID}/thing/property/set_reply
 *  回复 payload 格式（params 为强制载体，用于云端数值对齐）:
 *    {
 *      "id":     "<msg_id>",               // 必须，与 set 请求中的 id 一致
 *      "code":   200,                       // 200=成功，其他值表示错误
 *      "msg":    "success",                 // 可选，结果描述
 *      "params": {"A_score":5,"B_score":3}  // 必须，设备端实际生效的数值
 *    }
 *
 *  @param  msg_id       原始 property/set 请求中的消息 ID
 *  @param  code         结果码（200 表示成功）
 *  @param  msg          结果描述字符串（可选，传 NULL 则不填）
 *  @param  params_json  设备端生效的参数 JSON 对象字符串（如 "{\"A_score\":5,\"B_score\":3}"），
 *                       作为强制载体回传云端实现数值对齐，传 NULL 则省略
 *
 *  @return true-发送成功  false-连接断开或构建失败
 */
bool OneNet_ReplyPropertySet(const char *msg_id, int code, const char *msg,
                              const char *params_json);

/**
 * @brief  上传数据点到 Onenet 平台
 *
 *         自动构建 topic: $sys/{PROID}/{DEVID}/dp/post/json
 *
 * @param  json_buf  JSON 格式的数据点（如 "{\"temperature\":25.5}"）
 * @return msg_id（>=0 成功），失败返回 -1
 */
int OneNet_SendData(const char *json_buf);

/**
 * @brief  上传二进制数据到 Onenet 平台
 *
 *         使用 $dp topic 上传二进制数据，
 *         数据格式： [type:1B][head_len:2B][head_json][file_len:4B][binary_data...]
 *
 * @param  ds_name  数据流名称
 * @param  data     二进制数据指针
 * @param  len      数据长度
 * @return msg_id（>=0 成功），失败返回 -1
 */
int OneNet_SendBinData(const char *ds_name, const unsigned char *data, int len);

/**
 * @brief  断开 MQTT 连接并释放资源
 */
void OneNet_Disconnect(void);

#ifdef __cplusplus
}
#endif

#endif
