#include "cube.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orientation_history.h"
#include "uart.h"

#define PITCH_MIN_1 -15.0f
#define PITCH_MAX_1 15.0f
#define ROLL_MIN_1 75.0f
#define ROLL_MAX_1 105.0f

#define PITCH_MIN_2 -75.0f
#define PITCH_MAX_2 -105.0f
#define ROLL_MIN_2 -180.0f
#define ROLL_MAX_2 180.0f

#define PITCH_MIN_3 -15.0f
#define PITCH_MAX_3 15.0f
#define ROLL_MIN_3 -15.0f
#define ROLL_MAX_3 15.0f

#define PITCH_MIN_4 75.0f
#define PITCH_MAX_4 105.0f
#define ROLL_MIN_4 -180.0f
#define ROLL_MAX_4 180.0f

#define PITCH_MIN_5A -15.0f
#define PITCH_MAX_5A 15.0f
#define ROLL_MIN_5A 165.0f
#define ROLL_MAX_5A 180.0f

#define PITCH_MIN_5B -15.0f
#define PITCH_MAX_5B 15.0f
#define ROLL_MIN_5B -165.0f
#define ROLL_MAX_5B -180.0f

#define PITCH_MIN_6 -15.0f
#define PITCH_MAX_6 15.0f
#define ROLL_MIN_6 -75.0f
#define ROLL_MAX_6 -105.0f

static const char *TAG = "cube";
static volatile char cubeposition;

static bool is_between(float value, float a, float b)
{
    return (value >= (a < b ? a : b)) &&
           (value <= (a > b ? a : b));
}

void update_cube_position(const mpu6050_orientation_t *orientation)
{
    if (orientation == NULL) {
        ESP_LOGE(TAG, "Cannot update cube position without orientation data");
        return;
    }

    if (is_between(orientation->pitch, PITCH_MIN_1, PITCH_MAX_1) &&
        is_between(orientation->roll, ROLL_MIN_1, ROLL_MAX_1)) {
        cubeposition = 1;
    } else if (is_between(orientation->pitch, PITCH_MIN_2, PITCH_MAX_2) &&
               is_between(orientation->roll, ROLL_MIN_2, ROLL_MAX_2)) {
        cubeposition = 2;
    } else if (is_between(orientation->pitch, PITCH_MIN_3, PITCH_MAX_3) &&
               is_between(orientation->roll, ROLL_MIN_3, ROLL_MAX_3)) {
        cubeposition = 3;
    } else if (is_between(orientation->pitch, PITCH_MIN_4, PITCH_MAX_4) &&
               is_between(orientation->roll, ROLL_MIN_4, ROLL_MAX_4)) {
        cubeposition = 4;
    } else if (is_between(orientation->pitch, PITCH_MIN_5A, PITCH_MAX_5A) &&
               is_between(orientation->roll, ROLL_MIN_5A, ROLL_MAX_5A)) {
        cubeposition = 5;
    } else if (is_between(orientation->pitch, PITCH_MIN_5B, PITCH_MAX_5B) &&
               is_between(orientation->roll, ROLL_MIN_5B, ROLL_MAX_5B)) {
        cubeposition = 5;
    } else if (is_between(orientation->pitch, PITCH_MIN_6, PITCH_MAX_6) &&
               is_between(orientation->roll, ROLL_MIN_6, ROLL_MAX_6)) {
        cubeposition = 6;
    } else {
        cubeposition = 0;
    }

    const esp_err_t err = orientation_history_record((uint8_t)cubeposition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to record cube orientation: %s", esp_err_to_name(err));
    }
}

uint8_t cube_get_position(void)
{
    return (uint8_t)cubeposition;
}

void cube_set_position(uint8_t position)
{
    if (position > 6) {
        ESP_LOGE(TAG, "Invalid cube position: %d", position);
        return;
    }
    cubeposition = position;
}

void cube_position(void *arg)
{
    (void)arg;

    while (1) {
        char message[128];
        const int length = snprintf(message, sizeof(message),
                                    "Cube position - %d \r\n", (int)cube_get_position());
        if (length > 0 && (size_t)length < sizeof(message)) {
            uart_send_string(message);
        } else {
            ESP_LOGE(TAG, "Failed to format cube position UART output");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
