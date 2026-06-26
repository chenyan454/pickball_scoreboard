#include "tjc_usart_hmi.h"



/********************************************************
函数名：  	TJC_SendData
作者：    	
日期：    	2024.08.18
功能：    	向串口打印数据,需要制定发送的字符串长度
输入参数：	
返回值： 		
修改记录：
**********************************************************/
void TJC_SendData(uint8_t *data, uint16_t len)
{
    // 一次性批量写入全部数据到串口发送缓冲区
    uart_write_bytes(UART1_USE, data, len);
    // 阻塞等待整段数据全部硬件发送完成，再退出函数
   uart_wait_tx_done(UART1_USE, pdMS_TO_TICKS(100));
}


/********************************************************
函数名：  	TJCPrintf
作者：    	wwd
日期：    	2022.10.08
功能：    	向串口打印数据,使用前请将USART_SCREEN_write这个函数改为你的单片机的串口发送单字节函数
输入参数：	0参考printf
返回值： 		打印到串口的数量
修改记录：
**********************************************************/

void TJCPrintf(const char *str, ...)
{
    uint8_t end_buf[3] = {0xFF, 0xFF, 0xFF};
    char buffer[STR_LENGTH + 1];
    va_list arg_ptr;

    va_start(arg_ptr, str);
    int len = vsnprintf(buffer, STR_LENGTH + 1, str, arg_ptr);
    va_end(arg_ptr);

    // 发送指令主体
    if (len > 0)
    {
        uart_write_bytes(UART1_USE, buffer, len);
        uart_wait_tx_done(UART1_USE, pdMS_TO_TICKS(100));
    }
    // 一次性发送3个结束符
    uart_write_bytes(UART1_USE, end_buf, sizeof(end_buf));
    uart_wait_tx_done(UART1_USE, pdMS_TO_TICKS(100));
}

