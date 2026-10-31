#include "led.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define WS2812B_PIN GPIO_NUM_2
#define WS2812B_COUNT 30
#define RMT_RESOLUTION_HZ 10000000

static const char *TAG = "led";
static uint8_t ws2812_data[WS2812B_COUNT * 3];
static const uint8_t led_colors[6][3] = {
    {255, 0, 0},
    {0, 255, 0},
    {255, 255, 0},
    {255, 0, 255},
    {0, 0, 255},
    {255, 255, 255},
};
static const uint8_t led_colors_low[6][3] = {
    {31, 0, 0},
    {0, 31, 0},
    {31, 31, 0},
    {31, 0, 31},
    {0, 0, 31},
    {31, 31, 31},
};
static rmt_channel_handle_t tx_channel;
static rmt_encoder_handle_t bytes_encoder;
static const rmt_transmit_config_t tx_config = {
    .loop_count = 0,
    .flags.eot_level = 0,
};

static void ws2812_set_pixel(size_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (index >= WS2812B_COUNT) {
        return;
    }

    ws2812_data[index * 3 + 0] = green;
    ws2812_data[index * 3 + 1] = red;
    ws2812_data[index * 3 + 2] = blue;
}

static esp_err_t ws2812_refresh(void)
{
    if (tx_channel == NULL || bytes_encoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = rmt_transmit(tx_channel, bytes_encoder, ws2812_data,
                                 sizeof(ws2812_data), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812B transmit failed: %s", esp_err_to_name(err));
        return err;
    }

    return rmt_tx_wait_all_done(tx_channel, -1);
}

esp_err_t ws2812_wait_idle(void)
{
    if (tx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return rmt_tx_wait_all_done(tx_channel, -1);
}

void ws2812_step_colors(uint8_t offset)
{
    for (size_t i = 0; i < 6; ++i) {
        const uint8_t *color = led_colors[(i + offset) % 6];
        ws2812_set_pixel(i, color[0], color[1], color[2]);
    }
    (void)ws2812_refresh();
}

void ws2812_standard_colors(void)
{
    for (size_t i = 0; i < 6; ++i) {
        const uint8_t *color = led_colors[i];
        ws2812_set_pixel(i, color[0], color[1], color[2]);
    }
    (void)ws2812_refresh();
}

void ws2812_low_colors(void)
{
    for (size_t i = 0; i < 6; ++i) {
        const uint8_t *color = led_colors_low[i];
        ws2812_set_pixel(i, color[0], color[1], color[2]);
    }
    (void)ws2812_refresh();
}

esp_err_t ws2812_set_all(uint8_t red, uint8_t green, uint8_t blue)
{
    for (size_t i = 0; i < WS2812B_COUNT; ++i) {
        ws2812_set_pixel(i, red, green, blue);
    }
    return ws2812_refresh();
}

void configure_ws2812_chain(void)
{
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
        ESP_LOGE(TAG, "Failed to create WS2812B RMT channel: %s",
                 esp_err_to_name(err));
        return;
    }

    err = rmt_new_bytes_encoder(&encoder_config, &bytes_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create WS2812B encoder: %s",
                 esp_err_to_name(err));
        return;
    }

    err = rmt_enable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable WS2812B RMT channel: %s",
                 esp_err_to_name(err));
        return;
    }

    ws2812_step_colors(0);
}
