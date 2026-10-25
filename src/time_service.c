#include "time_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_service";

time_t current_time;
bool time_is_synchronized;

static bool sntp_started;

static void time_update_task(void *arg)
{
    (void)arg;

    while (true) {
        time(&current_time);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void time_sync_notification(struct timeval *tv)
{
    (void)tv;
    time_is_synchronized = true;
    ESP_LOGI(TAG, "Time synchronized from NTP");
}

esp_err_t time_service_init(void)
{
    char timezone[16];
    const char *sign = TIMEZONE_SHIFT >= 0 ? "-" : "+";
    const int offset = TIMEZONE_SHIFT >= 0 ? TIMEZONE_SHIFT : -TIMEZONE_SHIFT;
    const int length = snprintf(timezone, sizeof(timezone), "UTC%s%d", sign, offset);
    if (length < 0 || (size_t)length >= sizeof(timezone)) {
        return ESP_FAIL;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        return ESP_FAIL;
    }
    tzset();

    if (xTaskCreate(time_update_task, "time_update", 2048, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t time_service_start_sntp(void)
{
    if (sntp_started) {
        return ESP_OK;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification);
    esp_sntp_init();
    sntp_started = true;

    return ESP_OK;
}
