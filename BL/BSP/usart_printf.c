#include "usart_printf.h"

int usart_printf(UART_HandleTypeDef *huart, const char *format, ...)
{
    va_list arg;
    uint8_t buffer[512] = {0};
    va_start(arg, format);
    int len = vsnprintf((char *)buffer, sizeof(buffer), format, arg);
    va_end(arg);
    if (len < 0)
    {
        return len;
    }

    if (len > (int)sizeof(buffer))
    {
        len = (int)sizeof(buffer);
    }

    HAL_UART_Transmit_IT(huart, buffer, (uint16_t)len);
    return len;
}
