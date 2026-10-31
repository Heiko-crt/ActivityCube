#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(void);
void uart_send_string(const char *str);
int uart_read_data(uint8_t *data, uint32_t length);

#endif
