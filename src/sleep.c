#include "sleep.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "cube.h"
#include "led.h"
#include "orientation_history.h"

#ifndef SLEEP_BUTTON_HOLD_MS
#define SLEEP_BUTTON_HOLD_MS 3000
#endif
#define SLEEP_BUTTON_POLL_MS 50

static const char *TAG = "sleep";
static TaskHandle_t mpu6050_task_handle;
static TaskHandle_t cube_task_handle;
static TaskHandle_t uart_task_handle;
static TaskHandle_t cube_position_task_handle;

void sleep_button_set_application_tasks(TaskHandle_t mpu6050_task,
                                        TaskHandle_t cube_task,
                                        TaskHandle_t uart_task,
                                        TaskHandle_t cube_position_task)
{
    mpu6050_task_handle = mpu6050_task;
    cube_task_handle = cube_task;
    uart_task_handle = uart_task;
    cube_position_task_handle = cube_position_task;
}

static void resume_application_tasks(void)
{
    if (mpu6050_task_handle != NULL) {
        vTaskResume(mpu6050_task_handle);
    }
    if (cube_task_handle != NULL) {
        vTaskResume(cube_task_handle);
    }
    if (uart_task_handle != NULL) {
        vTaskResume(uart_task_handle);
    }
    if (cube_position_task_handle != NULL) {
        vTaskResume(cube_position_task_handle);
    }
}

static void enter_deep_sleep(void)
{
    if (mpu6050_task_handle != NULL) {
        vTaskSuspend(mpu6050_task_handle);
    }
    if (cube_task_handle != NULL) {
        vTaskSuspend(cube_task_handle);
    }
    if (uart_task_handle != NULL) {
        vTaskSuspend(uart_task_handle);
    }
    if (cube_position_task_handle != NULL) {
        vTaskSuspend(cube_position_task_handle);
    }

    esp_err_t err = ws2812_wait_idle();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed waiting for LED transmission before sleep: %s",
                 esp_err_to_name(err));
        resume_application_tasks();
        return;
    }

    cube_set_position(0);
    err = orientation_history_record_event(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to record undefined orientation before sleep: %s",
                 esp_err_to_name(err));
        resume_application_tasks();
        return;
    }

    err = ws2812_set_all(255, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to show sleep indication: %s", esp_err_to_name(err));
        resume_application_tasks();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    err = ws2812_set_all(0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off LEDs before sleep: %s", esp_err_to_name(err));
        resume_application_tasks();
        return;
    }

    ESP_ERROR_CHECK(rtc_gpio_init(SLEEP_BUTTON_PIN));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(SLEEP_BUTTON_PIN, RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(SLEEP_BUTTON_PIN));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(SLEEP_BUTTON_PIN));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(SLEEP_BUTTON_PIN, 0));

    ESP_LOGI(TAG, "Entering deep sleep; press the button to wake");
    esp_deep_sleep_start();
}

void sleep_button_task(void *arg)
{
    (void)arg;
    const TickType_t hold_time = pdMS_TO_TICKS(SLEEP_BUTTON_HOLD_MS);
    const TickType_t poll_interval = pdMS_TO_TICKS(SLEEP_BUTTON_POLL_MS);
    bool pressed = false;
    TickType_t pressed_since = 0;

    while (1) {
        if (gpio_get_level(SLEEP_BUTTON_PIN) == 0) {
            if (!pressed) {
                pressed = true;
                pressed_since = xTaskGetTickCount();
            } else if (xTaskGetTickCount() - pressed_since >= hold_time) {
                ESP_LOGI(TAG, "Sleep button held for %u ms", (unsigned)SLEEP_BUTTON_HOLD_MS);
                esp_err_t err = ws2812_set_all(255, 0, 0);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to show sleep indication: %s", esp_err_to_name(err));
                }
                if (mpu6050_task_handle != NULL) {
                    vTaskSuspend(mpu6050_task_handle);
                }
                if (cube_task_handle != NULL) {
                    vTaskSuspend(cube_task_handle);
                }
                if (uart_task_handle != NULL) {
                    vTaskSuspend(uart_task_handle);
                }
                if (cube_position_task_handle != NULL) {
                    vTaskSuspend(cube_position_task_handle);
                }
                while (gpio_get_level(SLEEP_BUTTON_PIN) == 0) {
                    vTaskDelay(poll_interval);
                }
                vTaskDelay(poll_interval);
                if (gpio_get_level(SLEEP_BUTTON_PIN) == 0) {
                    pressed = false;
                    continue;
                }

                enter_deep_sleep();
                pressed = false;
            }
        } else {
            pressed = false;
        }
        vTaskDelay(poll_interval);
    }
}
