#ifndef UART_COUNTER_H
#define UART_COUNTER_H

#include <stdint.h>

uint32_t uart_counter_increment(void);
uint32_t uart_counter_get(void);

#endif