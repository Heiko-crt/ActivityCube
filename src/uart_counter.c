#include "uart_counter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE counter_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t counter_value;

uint32_t uart_counter_increment(void)
{
    portENTER_CRITICAL(&counter_mux);
    const uint32_t value = ++counter_value;
    portEXIT_CRITICAL(&counter_mux);
    return value;
}

uint32_t uart_counter_get(void)
{
    portENTER_CRITICAL(&counter_mux);
    const uint32_t value = counter_value;
    portEXIT_CRITICAL(&counter_mux);
    return value;
}