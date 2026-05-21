#ifndef __USART_PRINTF_H
#define __USART_PRINTF_H

#include "stdio.h"
#include "main.h"
#include "stdarg.h"
#include "usart.h"

int usart_printf(UART_HandleTypeDef *huart, const char *format, ...);

#endif
