#ifndef  _TJC_USART_HMI_H_
#define  _TJC_USART_HMI_H_ 


#define STR_LENGTH 128

#include "bsp_uart.h"

void TJC_SendData(uint8_t *data, uint16_t len);

//打印到串口屏幕
void TJCPrintf(const char *cmd, ...);

#endif