#pragma once
#include <stdio.h>
#ifdef SIM_VERBOSE
#define ESP_LOGI(t, ...) do { printf("[%s] ", t); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define ESP_LOGI(t, ...) do { } while (0)
#endif
#define ESP_LOGE(t, ...) do { printf("[%s] FEHLER: ", t); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGW(t, ...) do { } while (0)
