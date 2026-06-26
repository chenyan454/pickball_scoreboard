#ifndef _BSP_UART_H_
#define _BSP_UART_H_

#include "driver/gpio.h"
#include "driver/uart.h"


#define UART0_USE  UART_NUM_0 //打印调试
#define UART1_USE  UART_NUM_1 //陶晶驰串口屏通讯


#define UART0_USE_TX GPIO_NUM_43
#define UART0_USE_RX GPIO_NUM_44

#define UART1_USE_TX GPIO_NUM_11
#define UART1_USE_RX GPIO_NUM_12

#define UART_TX_BUF_SIZE 1024
#define UART_RX_BUF_SIZE 1024


void usart1_init(uint32_t baudrate);

#endif