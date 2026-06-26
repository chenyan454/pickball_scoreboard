/**
 *  my_mqtt.h
 *  =========
 *  通用 MQTT 客户端封装层 — 接口定义
 *
 *  本模块对 ESP-IDF 内置 esp_mqtt 客户端进行二次封装，提供：
 *    - 统一的连接配置结构体 mqtt_config_t
 *    - 连接/断开/发布/订阅等通用 API
 *    - 可配置的心跳保活机制
 *    - 数据事件回调透传
 *
 *  注意：本模块不包含任何 OneNET 平台特定逻辑。
 */

#ifndef _MY_MQTT_H_
#define _MY_MQTT_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------- MQTT 数据包结构体 ---------------------------*/
typedef struct Buffer
{
	uint8_t	*_data;		// 协议数据
	uint32_t _len;		// 写入的数据长度
	uint32_t _size;		// 缓存总大小
	uint8_t	_memFlag;	// 内存使用的方案：0-未分配 1-动态分配 2-固定内存
} MQTT_PACKET_STRUCTURE;

/*--------------------------- MQTT 消息类型枚举 ---------------------------*/
enum MqttPacketType
{
    MQTT_PKT_CONNECT = 1, /**< 连接请求数据包 */
    MQTT_PKT_CONNACK,     /**< 连接确认数据包 */
    MQTT_PKT_PUBLISH,     /**< 发布数据数据包 */
    MQTT_PKT_PUBACK,      /**< 发布确认数据包 */
    MQTT_PKT_PUBREC,      /**< 发布数据已接收数据包，Qos 2时，回复MQTT_PKT_PUBLISH */
    MQTT_PKT_PUBREL,      /**< 发布数据释放数据包， Qos 2时，回复MQTT_PKT_PUBREC */
    MQTT_PKT_PUBCOMP,     /**< 发布完成数据包， Qos 2时，回复MQTT_PKT_PUBREL */
    MQTT_PKT_SUBSCRIBE,   /**< 订阅数据包 */
    MQTT_PKT_SUBACK,      /**< 订阅确认数据包 */
    MQTT_PKT_UNSUBSCRIBE, /**< 取消订阅数据包 */
    MQTT_PKT_UNSUBACK,    /**< 取消订阅确认数据包 */
    MQTT_PKT_PINGREQ,     /**< ping 数据包 */
    MQTT_PKT_PINGRESP,    /**< ping 响应数据包 */
    MQTT_PKT_DISCONNECT,  /**< 断开连接数据包 */
    MQTT_PKT_CMD          /**< 命令下发数据包 */
};

/*--------------------------- MQTT QoS 等级 ---------------------------*/
enum MqttQosLevel
{
    MQTT_QOS_LEVEL0,  /**< 最多发送一次 */
    MQTT_QOS_LEVEL1,  /**< 最少发送一次  */
    MQTT_QOS_LEVEL2   /**< 只发送一次 */
};

/*--------------------------- MQTT 连接标志位，内部使用 ---------------------------*/
enum MqttConnectFlag
{
    MQTT_CONNECT_CLEAN_SESSION  = 0x02,
    MQTT_CONNECT_WILL_FLAG      = 0x04,
    MQTT_CONNECT_WILL_QOS0      = 0x00,
    MQTT_CONNECT_WILL_QOS1      = 0x08,
    MQTT_CONNECT_WILL_QOS2      = 0x10,
    MQTT_CONNECT_WILL_RETAIN    = 0x20,
    MQTT_CONNECT_PASSORD        = 0x40,
    MQTT_CONNECT_USER_NAME      = 0x80
};

/*--------------------------- 预定义 packet ID ---------------------------*/
#define MQTT_PUBLISH_ID			10
#define MQTT_SUBSCRIBE_ID		20
#define MQTT_UNSUBSCRIBE_ID		30

/*--------------------------- MQTT 数据回调类型 ---------------------------*/
/**
 * @brief  MQTT 数据事件回调类型
 * @param  topic      消息主题
 * @param  topic_len  主题长度
 * @param  data       消息数据
 * @param  data_len   数据长度
 */
typedef void (*mqtt_data_cb_t)(const char *topic, int topic_len,
                                const char *data, int data_len);

/*--------------------------- MQTT 连接配置 ---------------------------*/
typedef struct {
    const char *uri;              /**< Broker URI（如 "mqtt://host:1883"） */
    const char *username;         /**< MQTT 用户名 */
    const char *password;         /**< MQTT 密码 */
    const char *client_id;        /**< MQTT Client ID */
	const char *certificate;      /**< 服务器根证书 */
	const char *common_name;	  /**< 服务器数字证书里绑定的域名 */
    int keepalive;                /**< 保活时间（秒），默认 120 */
    bool disable_clean_session;           /**< 是否清除会话 */

    // 遗嘱消息（可选，设为 NULL 表示不使用遗嘱）
    const char *will_topic;       /**< 遗嘱消息 topic */
    const char *will_message;     /**< 遗嘱消息内容 */
    int will_qos;                 /**< 遗嘱消息 QoS */
    int will_retain;              /**< 遗嘱消息 retain 标志 */

    // 心跳保活（可选，hb_topic 为 NULL 表示不启用心跳）
    const char *hb_topic;         /**< 心跳 topic */
    const char *hb_message;       /**< 心跳消息内容 */
    int hb_interval_ms;           /**< 心跳间隔（毫秒），0 表示默认 40000ms */
	
	//重连机制
	bool disable_auto_reconnect; /**< 失能自动重连 */
	int reconnect_timeout_ms;    /**< 重连的时间频率 */
} mqtt_config_t;

/*--------------------------- MQTT 通用接口 ---------------------------*/

/**
 * @brief  连接到 MQTT Broker
 *
 *         阻塞等待连接建立（最多约 10 秒），成功返回 true。
 *         如果配置了 hb_topic，会自动启动心跳保活任务。
 *
 * @param  cfg  连接配置（全部字段必须有效，指针指向的内容会被复制）
 * @return true-成功  false-失败
 */
bool mqtt_connect(const mqtt_config_t *cfg);

/**
 * @brief  断开 MQTT 连接并释放所有资源
 */
void mqtt_disconnect(void);

/**
 * @brief  查询 MQTT 是否已连接
 * @return true-已连接  false-未连接
 */
bool mqtt_is_connected(void);

/**
 * @brief  获取底层 esp_mqtt 客户端句柄
 *
 *         供需要直接操作 esp_mqtt 客户端的模块使用（如 OneNET 看门狗）。
 *         注意：切勿在外部调用 esp_mqtt_client_destroy()，统一通过 mqtt_disconnect() 释放。
 *
 * @return 客户端句柄，未连接时返回 NULL
 */
esp_mqtt_client_handle_t mqtt_get_client(void);

/**
 * @brief  订阅 topic
 * @param  topic  主题字符串
 * @param  qos    QoS 等级（0/1/2）
 * @return msg_id（>=0 成功），失败返回 -1
 */
int mqtt_subscribe(const char *topic, int qos);

/**
 * @brief  发布消息
 * @param  topic   主题
 * @param  data    消息数据
 * @param  len     数据长度
 * @param  qos     QoS 等级
 * @param  retain  保留标志
 * @return msg_id（>=0 成功），失败返回 -1
 */
int mqtt_publish(const char *topic, const char *data, int len, int qos, int retain);

/**
 * @brief  注册 MQTT 数据事件回调
 *
 *         当收到平台下发的消息（MQTT_EVENT_DATA）时，
 *         MQTT 模块会透传调用此回调。
 *
 * @param  cb  回调函数指针，传 NULL 取消回调
 */
void mqtt_set_data_callback(mqtt_data_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif
