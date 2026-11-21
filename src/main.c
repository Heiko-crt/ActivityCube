/* (c) by Heiko Frey, 2026*/
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cube.h"
#include "led.h"
#include "littlefs_storage.h"
#include "mpu6050.h"
#include "orientation_history.h"
#include "sleep.h"
#include "time_service.h"
#include "uart_counter.h"
#include "uart.h"
#include "wifi_client.h"

#define UART_NUM UART_NUM_0
#define TXD_PIN GPIO_NUM_1
#define RXD_PIN GPIO_NUM_3
#define UART_BAUD_RATE 115200
#define BUF_SIZE 256

static const char *TAG = "main";
static mpu6050_orientation_t orientation;
static TaskHandle_t mpu6050_task_handle;
static TaskHandle_t cube_task_handle;
static TaskHandle_t uart_task_handle;
static TaskHandle_t cube_position_task_handle;

void uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
}

void uart_send_string(const char *str) {
    uart_write_bytes(UART_NUM, str, strlen(str));
}

int uart_read_data(uint8_t *data, uint32_t length) {
    return uart_read_bytes(UART_NUM, data, length, 100 / portTICK_PERIOD_MS);
}

static void cube_task(void *arg)
{
    uint8_t led_step = 0;
    const TickType_t led_step_delay = pdMS_TO_TICKS(100);
    static char cubeposition_old = 0;
    static TickType_t last_update_time = 0;

    (void)arg;

    while (1) {
        if (cube_get_position() == 0) {
            ws2812_step_colors(led_step);
            led_step = (led_step + 1) % 6;
        }
        if ((cube_get_position() != cubeposition_old) && (cube_get_position() != 0)) {
            last_update_time = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - last_update_time >= pdMS_TO_TICKS(5000)) {
            ws2812_low_colors();
        } else {
            ws2812_standard_colors();
        }
        cubeposition_old = (char)cube_get_position();
        vTaskDelay(led_step_delay);
    }
}

static void uart_task(void *arg)
{
    uint8_t data[BUF_SIZE];
    int len = 0;

    (void)arg;

    uart_send_string("========== ESP32-C3 UART Test =========="
"\r\n");
    uart_send_string("UART initialized at 115200 baud\r\n");

    while (1) {
        len = uart_read_data(data, BUF_SIZE - 1);
        if (len > 0) {
            data[len] = '\0';

            uart_send_string("Received: ");
            uart_write_bytes(UART_NUM, (const char *)data, len);
            uart_send_string("\r\n");
        }

        char msg[64];
        const uint32_t count = uart_counter_increment();
        snprintf(msg, sizeof(msg), "Message #%lu\r\n", (unsigned long)count);
        uart_send_string(msg);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void mpu6050_task(void *arg)
{
    (void)arg;

    while (1) {
        //mpu6050_orientation_t orientation;
        const esp_err_t err = mpu6050_read_orientation(&orientation);
        if (err == ESP_OK) {
            update_cube_position(&orientation);
            char message[96];
            const int length = snprintf(message, sizeof(message),
                                        "MPU6050 roll: %.2f, pitch: %.2f, yaw: %.2f degrees\r\n",
                                        orientation.roll, orientation.pitch, orientation.yaw);
            if (length > 0 && (size_t)length < sizeof(message)) {
                uart_send_string(message);
            } else {
                ESP_LOGE(TAG, "Failed to format MPU6050 UART output");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
#if CONFIG_IDF_TARGET_ESP32
    const esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Woke from sleep button on GPIO%d", SLEEP_BUTTON_PIN);
    } else if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Woke from MPU6500 motion interrupt on GPIO%d",
                 MPU6500_WOM_PIN);
    }
    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT0 ||
        wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_ERROR_CHECK(rtc_gpio_deinit(SLEEP_BUTTON_PIN));
    }
    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_ERROR_CHECK(rtc_gpio_deinit(MPU6500_WOM_PIN));
    }
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(littlefs_storage_init());
    ESP_ERROR_CHECK(orientation_history_init());
    ESP_ERROR_CHECK(time_service_init());

    const gpio_config_t sleep_button_config = {
        .pin_bit_mask = 1ULL << SLEEP_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sleep_button_config));

    ESP_ERROR_CHECK(wifi_client_init());
    uart_init();
    configure_ws2812_chain();

    err = mpu6050_init();
    if (err == ESP_OK) {
        if (xTaskCreate(&mpu6050_task, "mpu6050_task", 3072, NULL, 4,
                        &mpu6050_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create MPU6050 task");
        }
    } else {
        ESP_LOGE(TAG, "MPU6050 unavailable; continuing without orientation readings");
    }

    xTaskCreate(&cube_task, "cube_task", 2048, NULL, 5, &cube_task_handle);
    xTaskCreate(&uart_task, "uart_task", 4096, NULL, 4, &uart_task_handle);
    xTaskCreate(&cube_position, "cube_position", 2048, NULL, 3,
                &cube_position_task_handle);
    sleep_button_set_application_tasks(mpu6050_task_handle, cube_task_handle,
                                       uart_task_handle, cube_position_task_handle);
    if (xTaskCreate(&sleep_button_task, "sleep_button_task",
                    SLEEP_BUTTON_TASK_STACK_SIZE, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sleep button task");
    }

    vTaskDelete(NULL);
}
