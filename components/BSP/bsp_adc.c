#include "bsp_adc.h"

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

const static char *TAG = "ADC";
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_chan2_handle = NULL;
bool do_calibration1_chan2 = NULL;

void adc_init(void)
{
	//-------------ADC1 Init---------------//
   if(adc1_handle)
   {
	   return;
   }
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = BATTERY_UINT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    //-------------ADC1 Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .atten = BATTERY_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BATTERY_CHANNEL, &config));
    

    //-------------ADC1 Calibration Init---------------//
   if(adc1_cali_chan2_handle)
   {
	   return;
   }
   else
   {
    do_calibration1_chan2=adc_calibration_init(BATTERY_UINT, BATTERY_CHANNEL, BATTERY_ATTEN,&adc1_cali_chan2_handle);
   }
}

//ADC校准方案
bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

    //曲线拟合
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }



    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success");
    } 
    else {
        ESP_LOGE(TAG, "Calibration Error");
    }

	
    return calibrated;
}

void adc_deinit(adc_oneshot_unit_handle_t handle)
{

 	
     ESP_ERROR_CHECK(adc_oneshot_del_unit(handle));
	 ESP_LOGI(TAG, "deregister single_channel ");
}

void adc_calibration_deinit(adc_cali_handle_t handle)
{

 	ESP_LOGI(TAG, "deregister %s calibration scheme", "Curve Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));
}

// 读取电压，任务里每3秒调用一次
// 采用爆发采样 + 数字平均替代硬件 RC 低通滤波：
// PWM 10kHz 方波 → ADC 快速连续采样 256 次 → 均值 ≈ DC 等效电压
esp_err_t adc_read_voltage_mv(int *out_mv)
{
    int64_t sum = 0;
    int sample_count = 0;

    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        int raw;
        esp_err_t ret = adc_oneshot_read(adc1_handle, BATTERY_CHANNEL, &raw);
        if (ret != ESP_OK) {
            continue;  // 单次失败跳过，不影响整体
        }
        sum += raw;
        sample_count++;
    }

    if (sample_count == 0) {
        *out_mv = 0;
        return ESP_FAIL;
    }

    int avg_raw = (int)(sum / sample_count);

    if (do_calibration1_chan2) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan2_handle, avg_raw, out_mv));
    } else {
        // 无校准时手动换算：avg_raw / 4095 * ADC_FULLSCALE_MV（ATTEN_DB_11 满量程 ~2.9V）
        *out_mv = (int)((int64_t)avg_raw * ADC_FULLSCALE_MV / 4095);
    }

    // PWM 软件校正（无硬件 RC 滤波时必须）：
    // ADC 在 PWM HIGH 期间（3.3V）超出线性范围（2.9V）而饱和，
    // 爆发采样均值 = D × 2.9V，但真实 DC 等效 = D × 3.3V
    // 补偿：out_mv = out_mv × VDD / ADC_FULLSCALE = out_mv × 3300 / 2900 ≈ out_mv × 1.138
    *out_mv = (int)((int64_t)*out_mv * PWM_CORRECTION_NUM / PWM_CORRECTION_DEN);
    return ESP_OK;
}
