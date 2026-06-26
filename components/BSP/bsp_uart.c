#include "bsp_uart.h"



void usart1_init(uint32_t baudrate)
{
		
		uart_config_t usartInitStruct={0};//串口初始化结构体
		usartInitStruct.baud_rate = baudrate;
		usartInitStruct.data_bits = UART_DATA_8_BITS;
		usartInitStruct.flags.allow_pd = 0;
		usartInitStruct.flags.backup_before_sleep = 0;
		usartInitStruct.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
		usartInitStruct.parity = UART_PARITY_DISABLE;
		usartInitStruct.rx_flow_ctrl_thresh = 0;
		usartInitStruct.source_clk = UART_SCLK_APB;
		usartInitStruct.stop_bits = UART_STOP_BITS_1;

		uart_param_config(UART1_USE, &usartInitStruct);//初始化
		
		//绑定引脚
		uart_set_pin(UART1_USE,UART1_USE_TX,UART1_USE_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
		//安装驱动
		uart_driver_install(UART1_USE,UART_RX_BUF_SIZE,UART_TX_BUF_SIZE,128,NULL,ESP_INTR_FLAG_LEVEL2);
}