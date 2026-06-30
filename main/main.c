#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "bsp_adc.h"
#include "my_dac.h"
//时间戳同步
#include <time.h>
#include "esp_sntp.h"


//将所有信息打包发送给串口屏
void sendto_TJC();

const char devPubTopic[]="$sys/yourproidInfo/yourdevidInfo/thing/property/post";
const char *devSubTopic[]={"$sys/yourproidInfo/yourdevidInfo/thing/property/set"};
static const char *TAG = "main";


//标记当前球员球权
typedef enum{
	SERVE_PLAYER1=1,
	SERVE_PLAYER2=2
}serve_player_t;
	
typedef struct {
	int current_set;//当前局数
	char time[32];//比赛消耗时间
	char match_status[32];//比赛状态
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

// 比赛开始时间（Unix 时间戳，单位：秒），由 OneNet 下发 start_ts
static volatile time_t g_start_ts = 0;

// 周期任务：启动电压输出
static void dac_ramp_task(void *arg)
{
    // 初始化DAC模块，仅执行一次
    ESP_ERROR_CHECK(dac_sim_init());

    int32_t volt = DAC_VOLT_MIN_MV;
    int32_t step = 10; // 每次变化10mV
    const TickType_t delay_ms = pdMS_TO_TICKS(50);

    int counter = 0;
    while (1)
    {
        // 设置目标输出电压
        dac_sim_set_voltage(volt);
        int32_t now_volt = dac_sim_get_voltage();

        // 仅每 1 秒打印一次，避免串口日志洪水导致 monitor 超时
        if (++counter % 20 == 0) {
            ESP_LOGI(TAG, "GPIO7 Output Voltage: %4d mV (%.2f V)", (int)now_volt, now_volt / 1000.0f);
        }

        // 0V ↔ 2.9V往返渐变
        volt += step;
        if (volt >= DAC_VOLT_MAX_MV) step = -10;
        if (volt <= DAC_VOLT_MIN_MV) step = 10;

        vTaskDelay(delay_ms);
    }


}

// 周期任务：每3秒读取电压值（256次爆发采样取平均）并下发到串口屏
void adc_get_baterry_task(void *arg)
{
    // 任务启动前仅初始化一次ADC
    adc_init();
	//0-2900mv
    int volt_mv;
	int volt_full=2900;
	int pct=0;
    while(1)
    {
        adc_read_voltage_mv(&volt_mv);
		pct=(volt_mv*100)/volt_full;
		//发送电量信息
		TJCPrintf("n2.val=%d", pct);
        ESP_LOGI("ADC", "当前电压:%d mV", volt_mv);
		ESP_LOGI("ADC", "当前电量:%d%%", pct);
		
        vTaskDelay(pdMS_TO_TICKS(3000)); // 3秒采集一次
    }
}



// 周期任务：计算从 g_start_ts 到当前时间流逝并每秒下发到串口屏（格式 MM:SS）
// 仅在 match_status == "进行中" 时计时，否则显示 00:00
static void elapsed_timer_task(void *arg)
{
    char buf[16];
    int tick = 0;
    while (1) {
        time_t now = time(NULL);
        time_t elapsed = 0;
        bool is_playing = (strcmp(PkInfo.match.match_status, "进行中") == 0);

        if (!is_playing)
		{
            // 非比赛状态：不计算时间，保持 00:00
            elapsed = 0;
        } 
		else if (is_playing&&now > g_start_ts)
		{
			// 比赛状态：计算时间
            elapsed = now - g_start_ts;
        } else {
            // 时间戳获取错误或者系统时间戳有误
            ESP_LOGE(TAG, "get error time!please check again.");
        }

        int minutes = (int)(elapsed / 60);
        int seconds = (int)(elapsed % 60);

        snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);

        // 调试输出：每 10 秒打印一次（避免串口洪水）
        if (tick % 10 == 0) {
            ESP_LOGI(TAG, "Dispatching time -> %s (elapsed=%llds, playing=%d)",
                     buf, (long long)elapsed, is_playing);
        }

        // 更新本地显示字段并下发给陶晶驰串口屏
        snprintf(PkInfo.match.time, sizeof(PkInfo.match.time), "%s", buf);
        TJCPrintf("t10.txt=\"%s\"", PkInfo.match.time);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

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

			
			cJSON *date = cJSON_GetObjectItem(params, "date");
			cJSON *current_set = cJSON_GetObjectItem(params, "number_game");
			cJSON *serve_team = cJSON_GetObjectItem(params, "serve_team");
			cJSON *serve_player = cJSON_GetObjectItem(params, "serve_player");
            cJSON *start_ts_item = cJSON_GetObjectItem(params, "start_ts");
			cJSON *match_status = cJSON_GetObjectItem(params, "match_status");


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
                snprintf(PkInfo.play_b.player1_name, sizeof(PkInfo.play_b.player1_name), "%s", b_player1_name->valuestring);
            }
			if (cJSON_IsString(b_player2_name)) {
                snprintf(PkInfo.play_b.player2_name, sizeof(PkInfo.play_b.player2_name), "%s", b_player2_name->valuestring);
            }

			if (cJSON_IsString(date)) {
                snprintf(PkInfo.match.date, sizeof(PkInfo.match.date), "%s", date->valuestring);
            }
			if (cJSON_IsString(serve_team)) {
                 snprintf(PkInfo.match.serve_team, sizeof(PkInfo.match.serve_team), "%s", serve_team->valuestring);
            }
			if (cJSON_IsString(match_status)) {
                 snprintf(PkInfo.match.match_status, sizeof(PkInfo.match.match_status), "%s", match_status->valuestring);
            }
			if (cJSON_IsNumber(serve_player)) {
                PkInfo.match.serve_player = serve_player->valueint;
            }
			if (cJSON_IsNumber(current_set)) {
                PkInfo.match.current_set = current_set->valueint;
            }

            if (start_ts_item) {
                long long raw_ts = 0;
                if (cJSON_IsNumber(start_ts_item)) {
                    raw_ts = (long long)start_ts_item->valuedouble;
                } 

                if (raw_ts > 1000000000000LL) {
                    // 13 位毫秒时间戳，转换为秒
                    g_start_ts = (time_t)(raw_ts / 1000LL);
                } else {
                    g_start_ts = (time_t)raw_ts;
                }
                ESP_LOGI(TAG, "Received start_ts raw=%lld -> start_ts(s)=%lld", raw_ts, (long long)g_start_ts);
            }
        }
        cJSON_Delete(json);
    }

    // 调试打印当前比分
    ESP_LOGI(TAG, "PropertySet done:比赛状态：%s,队伍A名字：%s,队伍A得分：%d,队伍A队员1：%s,队伍A队员2：%s,队伍B名字：%s,队伍B得分：%d,队伍B队员1：%s,:队伍B队员2：%s,:当前局数：%d,比赛消耗时间：%s,比赛日期：%s,球权（队伍）：%s,球权（队员）：%d", 
             PkInfo.match.match_status,PkInfo.play_a.name,PkInfo.play_a.score,PkInfo.play_a.player1_name,PkInfo.play_a.player2_name,
			 PkInfo.play_b.name,PkInfo.play_b.score,PkInfo.play_b.player1_name,PkInfo.play_b.player2_name,
			 PkInfo.match.current_set,PkInfo.match.time,PkInfo.match.date,
			 PkInfo.match.serve_team,PkInfo.match.serve_player
			);

	
    // 构建 params 载体（强制回传设备端实际值，云端据此做数值对齐）
    char params_buf[512];
    snprintf(params_buf, sizeof(params_buf),
             "{\"A_name\":%s,\"A_score\":%d,\"A_name1\":%s,\"A_name2\":%s,\"B_name\":%s,\"B_score\":%d,\"B_name1\":%s,\"B_name2\":%s},\"Time\":%s,\"date\":%s,\"number_game\":%d,\"serve_team\":%s,\"serve_player\":%d}",
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
		TJCPrintf("n3.val=%d", PkInfo.match.current_set);

		TJCPrintf("t2.txt=\"%s\"", PkInfo.play_a.name);
		TJCPrintf("t3.txt=\"%s\"", PkInfo.play_b.name);
		TJCPrintf("t14.txt=\"%s\"", PkInfo.match.match_status);


		TJCPrintf("t4.txt=\"%s\"", PkInfo.play_a.player1_name);
		TJCPrintf("t5.txt=\"%s\"", PkInfo.play_a.player2_name);
		TJCPrintf("t6.txt=\"%s\"", PkInfo.play_b.player1_name);
		TJCPrintf("t7.txt=\"%s\"", PkInfo.play_b.player2_name);


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

    // 设置日志级别：抑制高频重复日志，避免串口超时
    esp_log_level_set("wifi station", CONFIG_LOG_MAXIMUM_LEVEL);
    esp_log_level_set("onenet", ESP_LOG_WARN);         // 仅输出错误，抑制 data handler 中大量 INFO
    esp_log_level_set("my_mqtt", ESP_LOG_WARN);        // 仅输出错误，抑制心跳/发布确认日志
    esp_log_level_set("DAC_MOD", ESP_LOG_WARN);        // DAC 电压渐变信息量过大，仅输出警告
    esp_log_level_set("ADC", ESP_LOG_INFO);            // ADC 保留 INFO 级别（3s 一次，合理）


    // ==================== WiFi 连接 ====================
    ESP_LOGI(TAG, "Connecting to WiFi...");
    while (!wifi_init_sta()) {
        ESP_LOGW(TAG, "WiFi connect failed, retry in 500ms");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi connected");

    // ==================== SNTP 时间同步 ====================
    ESP_LOGI(TAG, "Starting SNTP time sync...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();

    // 等待时间同步完成（最长等待 10 秒，或时间已变为合理值）
    int sntp_retry = 0;
    while (++sntp_retry < 100) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            break;
        }
        // 即使状态未更新，time(NULL) 可能已经拿到正确时间
        time_t now = time(NULL);
        if (now > 1700000000) {  // 2023-11-15 之后，视为已同步
            ESP_LOGI(TAG, "SNTP time already valid (%lld), breaking early", (long long)now);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "SNTP time sync completed");
    } else {
        time_t now = time(NULL);
        if (now > 1700000000) {
            ESP_LOGI(TAG, "SNTP sync: status not final but time is valid");
        } else {
            ESP_LOGW(TAG, "SNTP sync timeout, will rely on tick fallback");
        }
    }

    ESP_LOGI(TAG, "Current time after SNTP sync: %lld", (long long)time(NULL));

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

    // 启动计时器下发任务（每秒更新一次显示）
    xTaskCreate(elapsed_timer_task, "elapsed_timer", 3072, NULL, 5, NULL);
	//启动电压输出
	xTaskCreate(dac_ramp_task, "dac_ramp_task", 3072, NULL, 5, NULL);
    // 启动电量显示（每30s更新一次）
	xTaskCreate(adc_get_baterry_task, "adc_task", 3072, NULL, 5, NULL);

	
    // ==================== 启动看门狗任务 ====================
    //xTaskCreate(mqtt_watchdog_task, "mqtt_wdog", 3072, NULL, 3, NULL);
    // ESP_LOGI(TAG, "MQTT watchdog task started");

    // app_main 不退出，维持主任务存活
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGD(TAG, "main task alive");
    }
}
