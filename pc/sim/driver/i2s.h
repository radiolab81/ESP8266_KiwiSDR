#pragma once
/* Simulierter I2S-Treiber: i2s_write() "verbraucht" die Daten im I2S-Takt (mit einstellbarer Abweichung). */
/* Wie der echte SDK-Header: bindet bool/size_t/TickType_t NICHT selbst ein. Wer ihn benutzt, muss
 * <stdbool.h> und freertos/FreeRTOS.h vorher einbinden (sink_pdm.c tut das). */
typedef int esp_err_t;
#define ESP_OK 0
typedef enum { I2S_NUM_0 = 0 } i2s_port_t;
enum { I2S_MODE_MASTER = 1, I2S_MODE_TX = 2 };
typedef enum { I2S_BITS_PER_SAMPLE_16BIT = 16 } i2s_bits_per_sample_t;
typedef enum { I2S_CHANNEL_FMT_RIGHT_LEFT = 0 } i2s_channel_fmt_t;
typedef enum { I2S_COMM_FORMAT_I2S = 1 } i2s_comm_format_t;
typedef struct { int mode; int sample_rate; i2s_bits_per_sample_t bits_per_sample; i2s_channel_fmt_t channel_format;
                 i2s_comm_format_t communication_format; int dma_buf_count; int dma_buf_len; bool tx_desc_auto_clear; } i2s_config_t;
typedef struct { int bck_o_en, ws_o_en, bck_i_en, ws_i_en, data_out_en, data_in_en; } i2s_pin_config_t;
esp_err_t i2s_driver_install(i2s_port_t p, const i2s_config_t *c, int q, void *qh);
esp_err_t i2s_driver_uninstall(i2s_port_t p);
esp_err_t i2s_set_pin(i2s_port_t p, const i2s_pin_config_t *pins);
esp_err_t i2s_write(i2s_port_t p, const void *src, size_t size, size_t *written, TickType_t wait);
esp_err_t i2s_start(i2s_port_t p);
esp_err_t i2s_stop(i2s_port_t p);
