#ifndef _BSP_ADC_H_
#define _BSP_ADC_H_


#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define BATTERY_UINT             ADC_UNIT_1
#define BATTERY_CHANNEL          ADC_CHANNEL_2
#define BATTERY_ATTEN            ADC_ATTEN_DB_11  // 11dB≈0~2.9V（ESP32-S3数据手册ADC最大可测电压）
#define BATTERY_SAMPLE_COUNT     256              // 爆发采样次数，覆盖多个 PWM 周期做数字平均

// PWM 软件校正系数：ADC 最大线性电压 2.9V，GPIO PWM 峰值 3.3V
// 无 RC 滤波时，爆发采样均值 = D × 2.9V，但真实 DC = D × 3.3V
// 补偿方式：out_mv = out_mv × VDD_MV / ADC_FULLSCALE_MV
#define ADC_FULLSCALE_MV         2900   // ESP32-S3 ADC 最大可测电压（数据手册）
#define VDD_MV                   2900   // GPIO 输出高电平标称值
#define PWM_CORRECTION_NUM       VDD_MV
#define PWM_CORRECTION_DEN       ADC_FULLSCALE_MV



void adc_init(void);
bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle);
void adc_deinit(adc_oneshot_unit_handle_t handle);
void adc_calibration_deinit(adc_cali_handle_t handle);
esp_err_t adc_read_voltage_mv(int *out_mv);
#endif