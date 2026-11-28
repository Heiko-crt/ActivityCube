#include "orientation_history.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

typedef struct {
    uint8_t position;
    uint64_t uptime_seconds;
    bool has_uptime;
    time_t ntp_time;
    bool has_ntp_time;
} history_event_t;

static bool parse_uptime(const char *text, uint64_t *seconds)
{
    unsigned long long hours;
    unsigned int minutes;
    unsigned int remainder;
    int consumed = 0;
    if (sscanf(text, "%llu:%u:%u%n", &hours, &minutes, &remainder,
               &consumed) != 3 ||
        text[consumed] != '\0' || minutes > 59 || remainder > 59 ||
        hours > (UINT64_MAX - 3599) / 3600) {
        return false;
    }

    *seconds = (uint64_t)hours * 3600 + minutes * 60 + remainder;
    return true;
}

static bool parse_ntp_time(const char *text, time_t *timestamp)
{
    struct tm parsed = {0};
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int consumed = 0;
    if (sscanf(text, "%4d-%2d-%2d %2d:%2d:%2d%n", &year, &month, &day,
               &hour, &minute, &second, &consumed) != 6 ||
        text[consumed] != '\0' || month < 1 || month > 12 || day < 1 ||
        day > 31 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_sec = second;
    parsed.tm_isdst = -1;
    const time_t result = mktime(&parsed);
    if (result == (time_t)-1) {
        return false;
    }

    struct tm verified;
    if (localtime_r(&result, &verified) == NULL ||
        verified.tm_year != year - 1900 || verified.tm_mon != month - 1 ||
        verified.tm_mday != day || verified.tm_hour != hour ||
        verified.tm_min != minute || verified.tm_sec != second) {
        return false;
    }

    *timestamp = result;
    return true;
}

static bool parse_history_event(char *line, history_event_t *event)
{
    char uptime[32];
    char ntp_time[32];
    int position;
    int consumed = 0;

    if (sscanf(line, "%31[^,],%31[^,],%d%n", uptime, ntp_time,
               &position, &consumed) == 3) {
        if (line[consumed] != '\n' && line[consumed] != '\r' &&
            line[consumed] != '\0') {
            return false;
        }
    } else {
        char legacy_timestamp[32];
        consumed = 0;
        if (sscanf(line, "%31[^,],%d%n", legacy_timestamp, &position,
                   &consumed) != 2 ||
            (line[consumed] != '\n' && line[consumed] != '\r' &&
             line[consumed] != '\0')) {
            return false;
        }
        uptime[0] = '\0';
        ntp_time[0] = '\0';
        if (strncmp(legacy_timestamp, "uptime ", 7) == 0) {
            snprintf(uptime, sizeof(uptime), "%s", legacy_timestamp + 7);
        } else {
            snprintf(ntp_time, sizeof(ntp_time), "%s", legacy_timestamp);
        }
    }

    if (position < 0 || position > 6) {
        return false;
    }

    event->position = (uint8_t)position;
    event->has_uptime = parse_uptime(uptime, &event->uptime_seconds);
    event->has_ntp_time = parse_ntp_time(ntp_time, &event->ntp_time);
    return true;
}

static bool get_interval_duration(const history_event_t *start,
                                  const history_event_t *end,
                                  uint64_t *duration)
{
    if (start->has_uptime && end->has_uptime &&
        end->uptime_seconds < start->uptime_seconds) {
        return false;
    }
    if (start->has_ntp_time && end->has_ntp_time &&
        end->ntp_time > start->ntp_time) {
        *duration = (uint64_t)(end->ntp_time - start->ntp_time);
        return true;
    }
    if (start->has_uptime && end->has_uptime &&
        end->uptime_seconds > start->uptime_seconds) {
        *duration = end->uptime_seconds - start->uptime_seconds;
        return true;
    }
    return false;
}

static void add_today_interval(orientation_history_statistics_t *statistics,
                               uint8_t position, time_t start, time_t end,
                               const struct tm *today)
{
    struct tm day_start = *today;
    day_start.tm_hour = 0;
    day_start.tm_min = 0;
    day_start.tm_sec = 0;
    day_start.tm_isdst = -1;
    const time_t today_start = mktime(&day_start);

    struct tm next_day = day_start;
    next_day.tm_mday += 1;
    next_day.tm_isdst = -1;
    const time_t today_end = mktime(&next_day);
    if (today_start == (time_t)-1 || today_end <= today_start ||
        start >= today_end || end <= today_start) {
        return;
    }

    const time_t overlap_start = start > today_start ? start : today_start;
    const time_t overlap_end = end < today_end ? end : today_end;
    if (overlap_end > overlap_start) {
        statistics->today_seconds[position] +=
            (uint64_t)(overlap_end - overlap_start);
    }
}

static void account_interval(orientation_history_statistics_t *statistics,
                             const history_event_t *start,
                             const history_event_t *end,
                             const struct tm *today)
{
    uint64_t duration;
    if (!get_interval_duration(start, end, &duration)) {
        return;
    }
    statistics->total_seconds[start->position] += duration;

    time_t start_time = 0;
    time_t end_time = 0;
    bool has_wall_times = false;
    if (start->has_ntp_time && end->has_ntp_time) {
        start_time = start->ntp_time;
        end_time = end->ntp_time;
        has_wall_times = true;
    } else if (start->has_uptime && end->has_uptime &&
               end->uptime_seconds > start->uptime_seconds) {
        if (end->has_ntp_time) {
            end_time = end->ntp_time;
            start_time = end_time - (time_t)duration;
            has_wall_times = true;
        } else if (start->has_ntp_time) {
            start_time = start->ntp_time;
            end_time = start_time + (time_t)duration;
            has_wall_times = true;
        }
    }
    if (has_wall_times && end_time > start_time) {
        add_today_interval(statistics, start->position, start_time, end_time,
                           today);
    }
}

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

esp_err_t orientation_history_get_statistics(
    orientation_history_statistics_t *statistics)
{
    if (statistics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (history_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(history_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    memset(statistics, 0, sizeof(*statistics));
    FILE *file = fopen(ORIENTATION_HISTORY_FILE, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open history file for statistics");
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    struct tm today;
    if (localtime_r(&now, &today) == NULL) {
        fclose(file);
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    history_event_t previous;
    bool has_previous = false;
    char line[128];
    while (fgets(line, sizeof(line), file) != NULL) {
        history_event_t current;
        if (!parse_history_event(line, &current)) {
            continue;
        }
        if (has_previous) {
            account_interval(statistics, &previous, &current, &today);
        }
        previous = current;
        has_previous = true;
    }

    const bool read_failed = ferror(file);
    const bool close_failed = fclose(file) != 0;
    esp_err_t result = ESP_OK;
    if (read_failed) {
        ESP_LOGE(TAG, "Failed to read history file for statistics");
        result = ESP_FAIL;
    } else if (close_failed) {
        ESP_LOGE(TAG, "Failed to close history file after statistics");
        result = ESP_FAIL;
    } else if (has_previous) {
        history_event_t current = {
            .position = previous.position,
            .uptime_seconds = (uint64_t)(esp_timer_get_time() / 1000000),
            .has_uptime = true,
            .ntp_time = now,
            .has_ntp_time = time_is_synchronized,
        };
        account_interval(statistics, &previous, &current, &today);
    }

    xSemaphoreGive(history_mutex);
    return result;
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
