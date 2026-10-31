#ifndef ORIENTATION_HISTORY_H
#define ORIENTATION_HISTORY_H

#include <stdint.h>

#include "esp_err.h"

#define ORIENTATION_HISTORY_FILE "/littlefs/orientation_history.csv"

esp_err_t orientation_history_init(void);
esp_err_t orientation_history_record(uint8_t position);
esp_err_t orientation_history_record_event(uint8_t position);
esp_err_t orientation_history_clear(void);

#endif
