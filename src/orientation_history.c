#include "orientation_history.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time_service.h"

static const char *TAG = "orientation_history";
static bool has_previous_position;
static uint8_t previous_position;
static SemaphoreHandle_t history_mutex;

esp_err_t orientation_history_init(void)
{
    history_mutex = xSemaphoreCreateMutex();
    if (history_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create history mutex");
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(ORIENTATION_HISTORY_FILE, "a+");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open history file");
        return ESP_FAIL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "Failed to seek history file");
        fclose(file);
        return ESP_FAIL;
    }
    const long file_size = ftell(file);
    if (file_size < 0) {
        ESP_LOGE(TAG, "Failed to determine history file size");
        fclose(file);
        return ESP_FAIL;
    }
    if (file_size == 0 &&
        fprintf(file, "Uptime,NTP time,Position\n") < 0) {
        ESP_LOGE(TAG, "Failed to write history file header");
        fclose(file);
        return ESP_FAIL;
    }

    rewind(file);
    char line[128];
    while (fgets(line, sizeof(line), file) != NULL) {
        char uptime[32];
        char ntp_time[32];
        int position;
        if (sscanf(line, "%31[^,],%31[^,],%d", uptime, ntp_time, &position) == 3 &&
            position >= 0 && position <= 6) {
            previous_position = (uint8_t)position;
            has_previous_position = true;
        } else if (sscanf(line, "%*[^,],%d", &position) == 1 &&
                   position >= 0 && position <= 6) {
            previous_position = (uint8_t)position;
            has_previous_position = true;
        }
    }

    if (ferror(file)) {
        ESP_LOGE(TAG, "Failed to read history file");
        fclose(file);
        return ESP_FAIL;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Failed to close history file");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t orientation_history_record_internal(uint8_t position, bool force)
{
    if (position > 6) {
        return ESP_ERR_INVALID_ARG;
    }
    if (history_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(history_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!force && has_previous_position && position == previous_position) {
        xSemaphoreGive(history_mutex);
        return ESP_OK;
    }

    char uptime[32];
    const uint64_t uptime_seconds = (uint64_t)(esp_timer_get_time() / 1000000);
    const uint64_t hours = uptime_seconds / 3600;
    const unsigned int minutes = (unsigned int)((uptime_seconds / 60) % 60);
    const unsigned int seconds = (unsigned int)(uptime_seconds % 60);
    const int uptime_length = snprintf(uptime, sizeof(uptime), "%llu:%02u:%02u",
                                      (unsigned long long)hours, minutes, seconds);
    if (uptime_length < 0 || (size_t)uptime_length >= sizeof(uptime)) {
        ESP_LOGE(TAG, "Failed to format uptime");
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    char ntp_time[32];
    if (time_is_synchronized) {
        const time_t now = time(NULL);
        struct tm local_time;
        if (localtime_r(&now, &local_time) == NULL ||
            strftime(ntp_time, sizeof(ntp_time), "%Y-%m-%d %H:%M:%S",
                     &local_time) == 0) {
            ESP_LOGE(TAG, "Failed to format synchronized timestamp");
            xSemaphoreGive(history_mutex);
            return ESP_FAIL;
        }
    } else {
        snprintf(ntp_time, sizeof(ntp_time), "undefined");
    }

    FILE *file = fopen(ORIENTATION_HISTORY_FILE, "a");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open history file for appending");
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    const int written = fprintf(file, "%s,%s,%u\n", uptime, ntp_time, position);
    const bool write_failed = written < 0 || fflush(file) != 0;
    const bool close_failed = fclose(file) != 0;
    if (write_failed || close_failed) {
        ESP_LOGE(TAG, "Failed to write history entry");
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    previous_position = position;
    has_previous_position = true;
    ESP_LOGI(TAG, "Recorded orientation %u at uptime %s, NTP time %s",
             position, uptime, ntp_time);
    xSemaphoreGive(history_mutex);
    return ESP_OK;
}

esp_err_t orientation_history_record(uint8_t position)
{
    return orientation_history_record_internal(position, false);
}

esp_err_t orientation_history_record_event(uint8_t position)
{
    return orientation_history_record_internal(position, true);
}

esp_err_t orientation_history_clear(void)
{
    if (history_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(history_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    FILE *file = fopen(ORIENTATION_HISTORY_FILE, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open history file for clearing");
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Failed to close cleared history file");
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Orientation history cleared");
    xSemaphoreGive(history_mutex);
    return ESP_OK;
}
