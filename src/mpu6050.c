#include "mpu6050.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#define MPU6050_I2C_PORT I2C_NUM_0
#define MPU6050_I2C_FREQUENCY_HZ 100000
#define MPU6050_I2C_TIMEOUT_MS 100
#define MPU6050_ADDRESS_AD0_LOW 0x68
#define MPU6050_ADDRESS_AD0_HIGH 0x69
#define MPU6050_WHO_AM_I_ID 0x68
#define MPU6500_WHO_AM_I_ID 0x70
#define MPU6050_REG_WHO_AM_I 0x75
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_PWR_MGMT_1 0x6B
#define MPU6050_REG_SMPLRT_DIV 0x19
#define MPU6050_REG_CONFIG 0x1A
#define MPU6050_REG_GYRO_CONFIG 0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_ACCEL_SCALE 16384.0f
#define MPU6050_GYRO_SCALE 131.0f
#define MPU6050_COMPLEMENTARY_ALPHA 0.98f

#if CONFIG_IDF_TARGET_ESP32C3
#define MPU6050_SDA_PIN GPIO_NUM_4
#define MPU6050_SCL_PIN GPIO_NUM_5
#else
#define MPU6050_SDA_PIN GPIO_NUM_21
#define MPU6050_SCL_PIN GPIO_NUM_22
#endif

static const char *TAG = "mpu6050";
static uint8_t device_address;
static bool i2c_bus_initialized;
static bool sensor_initialized;
static bool orientation_initialized;
static mpu6050_orientation_t current_orientation;
static int64_t last_sample_time_us;
static portMUX_TYPE orientation_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    ESP_LOGI(TAG, "I2C TX: START addr=0x%02X+W data=[0x%02X,0x%02X] STOP",
             device_address, data[0], data[1]);

    const esp_err_t err = i2c_master_write_to_device(
        MPU6050_I2C_PORT,
        device_address,
        data,
        sizeof(data),
        pdMS_TO_TICKS(MPU6050_I2C_TIMEOUT_MS));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C TX result: ACKed, %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "I2C TX result: %s (device may have NACKed or timed out)",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    ESP_LOGI(TAG,
             "I2C TX: START addr=0x%02X+W reg=0x%02X REPEATED-START addr=0x%02X+R len=%u",
             device_address, reg, device_address, (unsigned int)length);

    const esp_err_t err = i2c_master_write_read_device(
        MPU6050_I2C_PORT,
        device_address,
        &reg,
        sizeof(reg),
        data,
        length,
        pdMS_TO_TICKS(MPU6050_I2C_TIMEOUT_MS));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C RX: addr=0x%02X reg=0x%02X result=%s data:",
                 device_address, reg, esp_err_to_name(err));
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_INFO);
    } else {
        ESP_LOGE(TAG, "I2C RX: addr=0x%02X reg=0x%02X result=%s",
                 device_address, reg, esp_err_to_name(err));
    }
    return err;
}

static int16_t read_signed_word(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

esp_err_t mpu6050_init(void)
{
    if (sensor_initialized) {
        return ESP_OK;
    }

    if (!i2c_bus_initialized) {
        ESP_LOGI(TAG, "I2C pins are: SDA=%d, SCL=%d", MPU6050_SDA_PIN, MPU6050_SCL_PIN);
        const i2c_config_t config = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = MPU6050_SDA_PIN,
            .scl_io_num = MPU6050_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = MPU6050_I2C_FREQUENCY_HZ,
        };
        esp_err_t err = i2c_param_config(MPU6050_I2C_PORT, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure I2C: %s", esp_err_to_name(err));
            return err;
        }

        err = i2c_driver_install(MPU6050_I2C_PORT, config.mode, 0, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(err));
            return err;
        }
        i2c_bus_initialized = true;
    }

    const uint8_t addresses[] = {MPU6050_ADDRESS_AD0_LOW, MPU6050_ADDRESS_AD0_HIGH};
    uint8_t identity = 0;
    bool found = false;
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        device_address = addresses[i];
        const esp_err_t err = read_registers(MPU6050_REG_WHO_AM_I, &identity, sizeof(identity));
        if (err == ESP_OK) {
            if (identity == MPU6050_WHO_AM_I_ID || identity == MPU6500_WHO_AM_I_ID) {
                found = true;
                ESP_LOGI(TAG, "Detected %s (WHO_AM_I=0x%02X)",
                         identity == MPU6500_WHO_AM_I_ID ? "MPU-6500" : "MPU-6050",
                         identity);
                break;
            }

            ESP_LOGW(TAG, "Unexpected WHO_AM_I=0x%02X at I2C address 0x%02X",
                     identity, device_address);
        }
    }

    if (!found) {
        ESP_LOGE(TAG, "No supported MPU-6050/MPU-6500 found at I2C address 0x68 or 0x69");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = write_register(MPU6050_REG_PWR_MGMT_1, 0x00);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        err = write_register(MPU6050_REG_SMPLRT_DIV, 0x07);
    }
    if (err == ESP_OK) {
        err = write_register(MPU6050_REG_CONFIG, 0x03);
    }
    if (err == ESP_OK) {
        err = write_register(MPU6050_REG_GYRO_CONFIG, 0x00);
    }
    if (err == ESP_OK) {
        err = write_register(MPU6050_REG_ACCEL_CONFIG, 0x00);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MPU6050: %s", esp_err_to_name(err));
        return err;
    }

    sensor_initialized = true;
    ESP_LOGI(TAG, "Motion sensor ready at I2C address 0x%02x (SDA GPIO%d, SCL GPIO%d)",
             device_address, MPU6050_SDA_PIN, MPU6050_SCL_PIN);
    return ESP_OK;
}

esp_err_t mpu6050_read_orientation(mpu6050_orientation_t *orientation)
{
    if (orientation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[14];
    const esp_err_t err = read_registers(MPU6050_REG_ACCEL_XOUT_H, data, sizeof(data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read sensor data: %s", esp_err_to_name(err));
        return err;
    }

    const float accel_x = (float)read_signed_word(&data[0]) / MPU6050_ACCEL_SCALE;
    const float accel_y = (float)read_signed_word(&data[2]) / MPU6050_ACCEL_SCALE;
    const float accel_z = (float)read_signed_word(&data[4]) / MPU6050_ACCEL_SCALE;
    const float gyro_x = (float)read_signed_word(&data[8]) / MPU6050_GYRO_SCALE;
    const float gyro_y = (float)read_signed_word(&data[10]) / MPU6050_GYRO_SCALE;
    const float gyro_z = (float)read_signed_word(&data[12]) / MPU6050_GYRO_SCALE;

    const float radians_to_degrees = 57.2957795131f;
    const float accel_roll = atan2f(accel_y, accel_z) * radians_to_degrees;
    const float accel_pitch = atan2f(-accel_x, sqrtf(accel_y * accel_y + accel_z * accel_z))
                              * radians_to_degrees;
    const int64_t sample_time_us = esp_timer_get_time();

    portENTER_CRITICAL(&orientation_mux);
    if (!orientation_initialized) {
        current_orientation.roll = accel_roll;
        current_orientation.pitch = accel_pitch;
        current_orientation.yaw = 0.0f;
        orientation_initialized = true;
    } else {
        const float elapsed_seconds = (float)(sample_time_us - last_sample_time_us) / 1000000.0f;
        if (elapsed_seconds <= 0.0f || elapsed_seconds > 1.0f) {
            current_orientation.roll = accel_roll;
            current_orientation.pitch = accel_pitch;
        } else {
            current_orientation.roll = MPU6050_COMPLEMENTARY_ALPHA
                                       * (current_orientation.roll + gyro_x * elapsed_seconds)
                                       + (1.0f - MPU6050_COMPLEMENTARY_ALPHA) * accel_roll;
            current_orientation.pitch = MPU6050_COMPLEMENTARY_ALPHA
                                        * (current_orientation.pitch + gyro_y * elapsed_seconds)
                                        + (1.0f - MPU6050_COMPLEMENTARY_ALPHA) * accel_pitch;
            current_orientation.yaw += gyro_z * elapsed_seconds;
        }
    }
    last_sample_time_us = sample_time_us;

    *orientation = current_orientation;
    portEXIT_CRITICAL(&orientation_mux);
    return ESP_OK;
}

esp_err_t mpu6050_get_orientation(mpu6050_orientation_t *orientation)
{
    if (orientation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&orientation_mux);
    if (!orientation_initialized) {
        portEXIT_CRITICAL(&orientation_mux);
        return ESP_ERR_INVALID_STATE;
    }
    *orientation = current_orientation;
    portEXIT_CRITICAL(&orientation_mux);
    return ESP_OK;
}
