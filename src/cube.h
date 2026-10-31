#ifndef CUBE_H
#define CUBE_H

#include <stdint.h>

#include "mpu6050.h"

void update_cube_position(const mpu6050_orientation_t *orientation);
uint8_t cube_get_position(void);
void cube_set_position(uint8_t position);
void cube_position(void *arg);

#endif
