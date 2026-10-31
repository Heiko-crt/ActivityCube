#ifndef LED_H
#define LED_H

#include <stdint.h>

#include "esp_err.h"

void configure_ws2812_chain(void);
void ws2812_step_colors(uint8_t offset);
void ws2812_standard_colors(void);
void ws2812_low_colors(void);
esp_err_t ws2812_wait_idle(void);
esp_err_t ws2812_set_all(uint8_t red, uint8_t green, uint8_t blue);

#endif
