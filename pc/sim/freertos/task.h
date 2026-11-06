#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *h);
void vTaskDelete(TaskHandle_t h);
void vTaskDelay(TickType_t ticks);
