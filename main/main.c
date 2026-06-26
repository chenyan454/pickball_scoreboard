#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "bsp_uart.h"
#include "tjc_usart_hmi.h"
#include "my_wifi.h"
#include "cJSON.h"
#include "onenet.h"

//将所有信息打包发送给串口屏
void sendto_TJC();

const char devPubTopic[]="$sys/97zB3KY04H/woker1/thing/property/post";
const char *devSubTopic[]={"$sys/97zB3KY04H/woker1/thing/property/set"};
static const char *TAG = "main";


//标记当前球员球权
typedef enum{
	SERVE_PLAYER1=1,
	SERVE_PLAYER2=2
}serve_player_t;
	
typedef struct {
	int current_set;//当前局数
	char time[32];//比赛消耗时间
	char serve_team[2];//队伍球权
	serve_player_t serve_player;//球员球权
	char date[32];//比赛日期 2026-6-1

} Match;

typedef struct {
	char name[32];//队伍名称
	char player1_name[32];//队员1名字
	char player2_name[32];//队员2名字
	volatile int score;//队伍得分
	
} Play;

//比分面板
typedef struct {
	Match match;
	Play play_a;
	Play play_b;
} GameBoard;

//比赛信息初始化
static GameBoard PkInfo = { 
	.match  = {{0}}, 
	.play_a = {{0}}, 
	.play_b = {{0}}
 };

void init_all()
{
   //陶晶驰串口屏通讯
    usart1_init(115200);
}

/**
 * @brief  物模型属性设置回调
 *
 *  当云平台下发 thing/property/set 时，OneNET 模块解析后会调用此函数。
 *  应用层在此处理属性值（如更新显示、驱动硬件），
 *  然后调用 OneNet_ReplyPropertySet() 回复平台，实现云端数值对齐。
 */
static void on_property_set(const char *msg_id, const char *data, int data_len)
{
    ESP_LOGI(TAG, "PropertySet callback: msg_id=%s, data=%.*s",
             msg_id, data_len, data);

    // 解析 data 中的 params，更新 play_a.score / play_b.score
    cJSON *json = cJSON_ParseWithLength(data, data_len);
    if (json != NULL) {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        if (cJSON_IsObject(params)) {
            cJSON *a_score = cJSON_GetObjectItem(params, "A_score");
			cJSON *a_name = cJSON_GetObjectItem(params, "A_name");
			cJSON *a_player1_name = cJSON_GetObjectItem(params, "A_name1");
			cJSON *a_player2_name = cJSON_GetObjectItem(params, "A_name2");
            cJSON *b_score = cJSON_GetObjectItem(params, "B_score");
			cJSON *b_name = cJSON_GetObjectItem(params, "B_name");
			cJSON *b_player1_name = cJSON_GetObjectItem(params, "B_name1");
			cJSON *b_player2_name = cJSON_GetObjectItem(params, "B_name2");

			cJSON *time = cJSON_GetObjectItem(params, "Time");
			cJSON *date = cJSON_GetObjectItem(params, "date");
			cJSON *current_set = cJSON_GetObjectItem(params, "number_game");
			cJSON *serve_team = cJSON_GetObjectItem(params, "serve_team");
			cJSON *serve_player = cJSON_GetObjectItem(params, "serve_player");

            if (cJSON_IsNumber(a_score)) {
                PkInfo.play_a.score = a_score->valueint;
            }
			if (cJSON_IsString(a_name)) {
                snprintf(PkInfo.play_a.name, sizeof(PkInfo.play_a.name), "%s", a_name->valuestring);
            }
			if (cJSON_IsString(a_player1_name)) {
                snprintf(PkInfo.play_a.player1_name, sizeof(PkInfo.play_a.player1_name), "%s", a_player1_name->valuestring);
            }
			if (cJSON_IsString(a_player2_name)) {
                snprintf(PkInfo.play_a.player2_name, sizeof(PkInfo.play_a.player2_name), "%s", a_player2_name->valuestring);
            }
			
            if (cJSON_IsNumber(b_score)) {
                PkInfo.play_b.score = b_score->valueint;
            }
			if (cJSON_IsString(b_name)) {
                snprintf(PkInfo.play_b.name, sizeof(PkInfo.play_b.name), "%s", b_name->valuestring);
            }
			if (cJSON_IsString(b_player1_name)) {
                snprintf(PkInfo.play_b.player1_name, sizeof(PkInfo.play_a.player1_name), "%s", b_player1_name->valuestring);
            }
			if (cJSON_IsString(b_player2_name)) {
                snprintf(PkInfo.play_b.player2_name, sizeof(PkInfo.play_a.player2_name), "%s", b_player2_name->valuestring);
            }

			if (cJSON_IsString(time)) {
                snprintf(PkInfo.match.time, sizeof(PkInfo.match.time), "%s", time->valuestring);
            }
			if (cJSON_IsString(date)) {
                snprintf(PkInfo.match.date, sizeof(PkInfo.match.date), "%s", date->valuestring);
            }
			if (cJSON_IsString(serve_team)) {
                 snprintf(PkInfo.match.serve_team, sizeof(PkInfo.match.serve_team), "%s", serve_team->valuestring);
            }
			if (cJSON_IsNumber(serve_player)) {
                PkInfo.match.serve_player = serve_player->valueint;
            }
			if (cJSON_IsNumber(current_set)) {
                PkInfo.match.current_set = current_set->valueint;
            }
        }
        cJSON_Delete(json);
    }

    // 调试打印当前比分
    ESP_LOGI(TAG, "PropertySet done: play_a.name=%s,play_a.score=%d,play_a.player1_name=%s,play_a.player2_name=%s,play_b.name=%s,play_b.score=%d,play_b.player1_name=%s,play_b.player2_name=%s,set=%d,time=%s,date=%s,serve_team=%s,serve_player=%d", 
             PkInfo.play_a.name,PkInfo.play_a.score,PkInfo.play_a.player1_name,PkInfo.play_a.player2_name,
			 PkInfo.play_b.name,PkInfo.play_b.score,PkInfo.play_b.player1_name,PkInfo.play_b.player2_name,
			 PkInfo.match.current_set,PkInfo.match.time,PkInfo.match.date,
			 PkInfo.match.serve_team,PkInfo.match.serve_player
			);

	
    // 构建 params 载体（强制回传设备端实际值，云端据此做数值对齐）
    char params_buf[512];
    snprintf(params_buf, sizeof(params_buf),
             "{\"A_name\":%s,\"A_score\":%d,\"A_name1\":%s,\"A_name2\":%s,\"B_name\":%s,\"B_score\":%d,\"B_name1\":%s,\"B_name2\":%s,},\"Time\":%s,\"date\":%s,\"number_game\":%d,\"serve_team\":%s,\"serve_player\":%d}",
             PkInfo.play_a.name,PkInfo.play_a.score,PkInfo.play_a.player1_name,PkInfo.play_a.player2_name,
			 PkInfo.play_b.name,PkInfo.play_b.score,PkInfo.play_b.player1_name,PkInfo.play_b.player2_name,
			 PkInfo.match.time,PkInfo.match.date,PkInfo.match.current_set,
			 PkInfo.match.serve_team,PkInfo.match.serve_player
			);

    // 回复平台（携带 params 强制载体）
    OneNet_ReplyPropertySet(msg_id, 0, "success", params_buf);


	//将所有信息打包发送给陶晶驰串口屏
	sendto_TJC();
}

/**
 * @brief  MQTT 连接监控任务
 *
 *  定期检查连接状态，如果断开则自动重连。
 *  esp_mqtt_client 内部有自动重连机制，此任务作为兜底保障。
 */
static void mqtt_watchdog_task(void *arg)
{
    // 等待 WiFi 和 MQTT 首次连接完成
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (1) {
        esp_mqtt_client_handle_t client = OneNet_GetClient();
        if (client == NULL) {
            ESP_LOGW(TAG, "Watchdog: MQTT client is NULL, retrying OneNet_DevLink...");
            if (OneNet_DevLink()) {
                ESP_LOGI(TAG, "Watchdog: Reconnected OK");
                OneNet_Subscribe(devSubTopic, 1);
            } else {
                ESP_LOGE(TAG, "Watchdog: Reconnect failed, will retry");
            }
        }
        // 每 30 秒检查一次
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void sendto_TJC()
{
		TJCPrintf("n0.val=%d", PkInfo.play_a.score);
		TJCPrintf("n1.val=%d", PkInfo.play_b.score);

		TJCPrintf("t2.txt=\"%s\"", PkInfo.play_a.name);
		TJCPrintf("t3.txt=\"%s\"", PkInfo.play_b.name);

		TJCPrintf("t4.txt=\"%s\"", PkInfo.play_a.player1_name);
		TJCPrintf("t5.txt=\"%s\"", PkInfo.play_a.player2_name);
		TJCPrintf("t6.txt=\"%s\"", PkInfo.play_b.player1_name);
		TJCPrintf("t7.txt=\"%s\"", PkInfo.play_b.player2_name);

	    TJCPrintf("t10.txt=\"%s\"", PkInfo.match.time);

		TJCPrintf("click m0,1");//发送解析计时器数据指令

		TJCPrintf("t9.txt=\"%s\"", PkInfo.match.date);

	    TJCPrintf("va6.txt=\"%s\"", PkInfo.match.serve_team);
	    TJCPrintf("va7.val=%d", PkInfo.match.serve_player);

}

void app_main(void)
{
     init_all();

    // 初始化 NVS（WiFi 需要）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 设置日志级别
    esp_log_level_set("wifi station", CONFIG_LOG_MAXIMUM_LEVEL);
    esp_log_level_set("onenet", CONFIG_LOG_MAXIMUM_LEVEL);
	esp_log_level_set("my_mqtt", CONFIG_LOG_MAXIMUM_LEVEL);


    // ==================== WiFi 连接 ====================
    ESP_LOGI(TAG, "Connecting to WiFi...");
    while (!wifi_init_sta()) {
        ESP_LOGW(TAG, "WiFi connect failed, retry in 500ms");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi connected");

		
    // ==================== MQTT 连接 OneNET ====================
    ESP_LOGI(TAG, "Connecting to OneNET MQTT...");
    while (OneNet_DevLink() == false) {
        ESP_LOGE(TAG, "MQTT连接失败，500ms后重试");
        OneNet_Disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // 订阅平台下发 topic
    OneNet_Subscribe(devSubTopic, 1);
    // 注册属性设置回调（收到 property/set 后自动回复 set_reply 实现数值对齐）
    OneNet_SetPropertySetCallback(on_property_set);

    ESP_LOGI(TAG, "Onenet connected successfully!");

    // ==================== 启动看门狗任务 ====================
    //xTaskCreate(mqtt_watchdog_task, "mqtt_wdog", 3072, NULL, 3, NULL);
    // ESP_LOGI(TAG, "MQTT watchdog task started");

    // app_main 不退出，维持主任务存活
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGD(TAG, "main task alive");
    }
}
