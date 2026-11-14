#ifndef MPU6050_H
#define MPU6050_H

#include "esp_err.h"

typedef struct {
    float roll;
    float pitch;
    float yaw;
} mpu6050_orientation_t;

esp_err_t mpu6050_init(void);
esp_err_t mpu6500_enter_wom_mode(void);
esp_err_t mpu6050_read_orientation(mpu6050_orientation_t *orientation);
esp_err_t mpu6050_get_orientation(mpu6050_orientation_t *orientation);

#endif
