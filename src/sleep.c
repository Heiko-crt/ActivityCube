#include "sleep.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "cube.h"
#include "led.h"
#include "mpu6050.h"
#include "orientation_history.h"

#ifndef SLEEP_BUTTON_HOLD_MS
#define SLEEP_BUTTON_HOLD_MS 3000
#endif
#ifndef INACTIVITY_SLEEP_ENABLED
#define INACTIVITY_SLEEP_ENABLED 0
#endif
#define SLEEP_BUTTON_POLL_MS 50
#if INACTIVITY_SLEEP_ENABLED
#define INACTIVITY_SLEEP_MS 30000
#endif

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

static void enter_deep_sleep(bool wake_on_motion)
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

    err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset wake sources before sleep: %s",
                 esp_err_to_name(err));
        resume_application_tasks();
        return;
    }

    if (wake_on_motion) {
#if CONFIG_IDF_TARGET_ESP32
        err = rtc_gpio_init(MPU6500_WOM_PIN);
        if (err == ESP_OK) {
            err = rtc_gpio_set_direction(MPU6500_WOM_PIN, RTC_GPIO_MODE_INPUT_ONLY);
        }
        if (err == ESP_OK) {
            err = rtc_gpio_pullup_dis(MPU6500_WOM_PIN);
        }
        if (err == ESP_OK) {
            err = rtc_gpio_pulldown_en(MPU6500_WOM_PIN);
        }
        if (err == ESP_OK) {
            err = esp_sleep_enable_ext1_wakeup(1ULL << MPU6500_WOM_PIN,
                                               ESP_EXT1_WAKEUP_ANY_HIGH);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure MPU6500 wake pin: %s",
                     esp_err_to_name(err));
            rtc_gpio_deinit(MPU6500_WOM_PIN);
            resume_application_tasks();
            return;
        }

        err = mpu6500_enter_wom_mode();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure MPU6500 wake-on-motion: %s",
                     esp_err_to_name(err));
            const esp_err_t recovery_err = mpu6050_init();
            if (recovery_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore motion sensor after wake-on-motion setup: %s",
                         esp_err_to_name(recovery_err));
            }
            rtc_gpio_deinit(MPU6500_WOM_PIN);
            resume_application_tasks();
            return;
        }

        const uint32_t wom_pin_level = rtc_gpio_get_level(MPU6500_WOM_PIN);
        if (wom_pin_level != 0) {
            ESP_LOGE(TAG, "MPU6500 INT pin GPIO%d is high before deep sleep",
                     MPU6500_WOM_PIN);
            rtc_gpio_deinit(MPU6500_WOM_PIN);
            const esp_err_t recovery_err = mpu6050_init();
            if (recovery_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore motion sensor after asserted INT pin: %s",
                         esp_err_to_name(recovery_err));
            }
            resume_application_tasks();
            return;
        }
#else
        ESP_LOGE(TAG, "MPU6500 wake-on-motion on GPIO32 requires the classic ESP32");
        resume_application_tasks();
        return;
#endif
    } else {
        err = mpu6050_enter_sleep_mode();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to disable motion sensing before button-only sleep: %s",
                     esp_err_to_name(err));
            const esp_err_t recovery_err = mpu6050_init();
            if (recovery_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore motion sensor after sleep setup error: %s",
                         esp_err_to_name(recovery_err));
            }
            resume_application_tasks();
            return;
        }
    }

    err = rtc_gpio_init(SLEEP_BUTTON_PIN);
    if (err == ESP_OK) {
        err = rtc_gpio_set_direction(SLEEP_BUTTON_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    }
    if (err == ESP_OK) {
        err = rtc_gpio_pullup_en(SLEEP_BUTTON_PIN);
    }
    if (err == ESP_OK) {
        err = rtc_gpio_pulldown_dis(SLEEP_BUTTON_PIN);
    }
    if (err == ESP_OK) {
        err = esp_sleep_enable_ext0_wakeup(SLEEP_BUTTON_PIN, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure sleep-button wake pin: %s",
                 esp_err_to_name(err));
        const esp_err_t recovery_err = mpu6050_init();
        if (recovery_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore motion sensor after wake setup error: %s",
                     esp_err_to_name(recovery_err));
        }
#if CONFIG_IDF_TARGET_ESP32
        rtc_gpio_deinit(MPU6500_WOM_PIN);
#endif
        rtc_gpio_deinit(SLEEP_BUTTON_PIN);
        resume_application_tasks();
        return;
    }

    if (wake_on_motion) {
        ESP_LOGI(TAG, "Entering deep sleep; press the button or move the cube to wake");
    } else {
        ESP_LOGI(TAG, "Entering button-only deep sleep");
    }
    esp_deep_sleep_start();
}

void sleep_button_task(void *arg)
{
    (void)arg;
    const TickType_t hold_time = pdMS_TO_TICKS(SLEEP_BUTTON_HOLD_MS);
    const TickType_t poll_interval = pdMS_TO_TICKS(SLEEP_BUTTON_POLL_MS);
#if INACTIVITY_SLEEP_ENABLED
    const TickType_t inactivity_timeout = pdMS_TO_TICKS(INACTIVITY_SLEEP_MS);
#endif
    bool pressed = false;
    TickType_t pressed_since = 0;
#if INACTIVITY_SLEEP_ENABLED
    TickType_t last_orientation_change = xTaskGetTickCount();
    uint8_t last_position = cube_get_position();
#endif

    while (1) {
        const TickType_t now = xTaskGetTickCount();
#if INACTIVITY_SLEEP_ENABLED
        const uint8_t position = cube_get_position();
        if (position != last_position) {
            last_position = position;
            last_orientation_change = now;
        }
#endif

        if (gpio_get_level(SLEEP_BUTTON_PIN) == 0) {
            if (!pressed) {
                pressed = true;
                pressed_since = now;
            } else if (now - pressed_since >= hold_time) {
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

#if INACTIVITY_SLEEP_ENABLED
                last_orientation_change = xTaskGetTickCount();
#endif
                enter_deep_sleep(false);
                pressed = false;
            }
        } else {
            pressed = false;
#if INACTIVITY_SLEEP_ENABLED
            if (now - last_orientation_change >= inactivity_timeout) {
                ESP_LOGI(TAG, "No orientation change for %u ms; entering deep sleep",
                         (unsigned)INACTIVITY_SLEEP_MS);
                last_orientation_change = now;
                enter_deep_sleep(true);
            }
#endif
        }
        vTaskDelay(poll_interval);
    }
}
