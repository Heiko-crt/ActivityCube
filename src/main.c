#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UART_NUM UART_NUM_0
#define TXD_PIN GPIO_NUM_1
#define RXD_PIN GPIO_NUM_3
#define UART_BAUD_RATE 115200
#define BUF_SIZE 256
#define WS2812B_PIN GPIO_NUM_2
#define WS2812B_COUNT 30
#define RMT_RESOLUTION_HZ 10000000

static uint8_t ws2812_data[WS2812B_COUNT * 3];
static const uint8_t led_colors[6][3] = {
    {255, 0, 0},      // red
    {0, 255, 0},      // green
    {255, 255, 0},    // yellow
    {255, 0, 255},    // purple
    {0, 0, 255},      // blue
    {255, 255, 255},  // white
};
static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t bytes_encoder = NULL;
static rmt_transmit_config_t tx_config = {
    .loop_count = 0,
    .flags.eot_level = 0,
};

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

static void ws2812_set_pixel(size_t index, uint8_t red, uint8_t green, uint8_t blue) {
    if (index >= WS2812B_COUNT) {
        return;
    }

    ws2812_data[index * 3 + 0] = green;
    ws2812_data[index * 3 + 1] = red;
    ws2812_data[index * 3 + 2] = blue;
}

static void ws2812_refresh(void) {
    if (tx_channel == NULL || bytes_encoder == NULL) {
        return;
    }

    esp_err_t err = rmt_transmit(tx_channel, bytes_encoder, ws2812_data, sizeof(ws2812_data), &tx_config);
    if (err != ESP_OK) {
        printf("WS2812B transmit failed: %s\n", esp_err_to_name(err));
        return;
    }

    rmt_tx_wait_all_done(tx_channel, -1);
}

static void ws2812_step_colors(uint8_t offset) {
    for (size_t i = 0; i < 6; ++i) {
        const uint8_t *color = led_colors[(i + offset) % 6];
        ws2812_set_pixel(i, color[0], color[1], color[2]);
    }

    ws2812_refresh();
}

void configure_ws2812_chain(void) {
    memset(ws2812_data, 0, sizeof(ws2812_data));

    rmt_tx_channel_config_t channel_config = {
        .gpio_num = WS2812B_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };

    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .duration0 = 4,
            .level0 = 1,
            .duration1 = 8,
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = 8,
            .level0 = 1,
            .duration1 = 4,
            .level1 = 0,
        },
        .flags = {
            .msb_first = 1,
        },
    };

    esp_err_t err = rmt_new_tx_channel(&channel_config, &tx_channel);
    if (err != ESP_OK) {
        printf("Failed to create WS2812B RMT channel: %s\n", esp_err_to_name(err));
        return;
    }

    err = rmt_new_bytes_encoder(&encoder_config, &bytes_encoder);
    if (err != ESP_OK) {
        printf("Failed to create WS2812B encoder: %s\n", esp_err_to_name(err));
        return;
    }

    err = rmt_enable(tx_channel);
    if (err != ESP_OK) {
        printf("Failed to enable WS2812B RMT channel: %s\n", esp_err_to_name(err));
        return;
    }

    ws2812_step_colors(0);
}

static void led_task(void *arg)
{
    uint8_t led_step = 0;
    const TickType_t led_step_delay = pdMS_TO_TICKS(100);

    (void)arg;

    while (1) {
        ws2812_step_colors(led_step);
        led_step = (led_step + 1) % 6;
        vTaskDelay(led_step_delay);
    }
}

static void uart_task(void *arg)
{
    uint8_t data[BUF_SIZE];
    int len = 0;
    uint32_t count = 0;

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
        snprintf(msg, sizeof(msg), "Message #%lu\r\n", ++count);
        uart_send_string(msg);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    uart_init();
    configure_ws2812_chain();

    xTaskCreate(&led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(&uart_task, "uart_task", 4096, NULL, 4, NULL);

    vTaskDelete(NULL);
}
