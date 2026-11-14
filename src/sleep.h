#ifndef SLEEP_H
#define SLEEP_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SLEEP_BUTTON_TASK_STACK_SIZE 8192
#define SLEEP_BUTTON_PIN GPIO_NUM_33
#if CONFIG_IDF_TARGET_ESP32
#define MPU6500_WOM_PIN GPIO_NUM_32
#endif

void sleep_button_set_application_tasks(TaskHandle_t mpu6050_task,
                                        TaskHandle_t cube_task,
                                        TaskHandle_t uart_task,
                                        TaskHandle_t cube_position_task);
void sleep_button_task(void *arg);

#endif
