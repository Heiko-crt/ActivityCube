#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#define TIMEZONE_SHIFT 1

extern time_t current_time;
extern bool time_is_synchronized;

esp_err_t time_service_init(void);
esp_err_t time_service_start_sntp(void);

#endif
