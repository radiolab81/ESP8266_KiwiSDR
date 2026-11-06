#pragma once
/* Minimal-Ersatz fuer die Simulation am PC (pthreads statt FreeRTOS) */
#include <stdint.h>
#include <stddef.h>
typedef void *TaskHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
