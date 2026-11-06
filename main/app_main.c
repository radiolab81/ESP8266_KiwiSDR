/**
 * @file app_main.c
 * @brief ESP8266-Anwendung: WLAN verbinden -> KiwiSDR-Audio empfangen -> PDM-Ausgang (GPIO3).
 *
 * ABLAUF (drei FreeRTOS-Tasks; Aufbau wie bei der ESP32-Version)
 *   kiwi_task    : verbindet, ruft in einer Schleife kiwi_client_poll() auf und
 *                  baut bei Fehlern mit wachsender Wartezeit neu auf.
 *   pdm_out      : (in sink_pdm.c) holt PCM aus dem Ringpuffer -> Delta-Sigma -> I2S.
 *   console      : (in net_console.c) nimmt Befehle per TCP an und stimmt um:
 *                    f 1008      Frequenz in kHz
 *                    m usb       Modus (am amn amw sam usb lsb cw nbfm ...)
 *                    s           Status      q  Verbindung beenden
 *
 * UNTERSCHIEDE ZUR ESP32-FASSUNG
 *   - Altes SDK-Muster fuer WLAN: tcpip_adapter_init() + esp_event_loop_init(handler)
 *     mit system_event_t (statt esp_netif + esp_event_handler_register).
 *   - Konsole ueber TCP, weil die serielle Eingabe (GPIO3) das Audiosignal fuehrt.
 *   - Audio ueber PDM/I2S statt ueber den 8-Bit-DAC.
 *
 * Hoeflichkeit gegenueber dem Kiwi: Pro ESP8266 immer nur EINE Verbindung, und
 * nach einem Fehler wird mindestens 5 s (dann 10, 20, 40, 60 s) gewartet,
 * damit der Server uns nicht als Dauerverbinder sperrt.
 */
#include <stdio.h>
#include <string.h>

#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "kiwi_client.h"
#include "net_console.h"
#include "sink_pdm.h"

static const char *TAG = "kiwi_app";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t g_wifi_events;
static kiwi_client_t      g_client;        /* ca. 3,5 KB, bewusst statisch (nicht Stack) */
static volatile int       g_quit_session;  /* "q": aktuelle Sitzung beenden */
static volatile int       g_active;        /* 1 = g_client ist verbunden und darf angesprochen werden */

#ifdef CONFIG_KIWI_ADPCM
#define KIWI_USE_ADPCM 1
#else
#define KIWI_USE_ADPCM 0
#endif

/* ===================================================================== */
/* WLAN (altes Ereignismodell: eine Callback-Funktion fuer alle Ereignisse)*/
/* ===================================================================== */

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    (void)ctx;
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WLAN getrennt, versuche erneut ...");
        esp_wifi_connect();
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void wifi_start(void)
{
    g_wifi_events = xEventGroupCreate();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strncpy((char *)wc.sta.ssid, CONFIG_KIWI_WIFI_SSID, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, CONFIG_KIWI_WIFI_PASSWORD, sizeof wc.sta.password - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wc));
    /* Kein Modem-Sleep: weniger Verzoegerungsschwankungen. Kostet Strom, ist hier aber
     * wichtig, damit der Audiopuffer nicht leerlaeuft. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ===================================================================== */
/* Audio-Senke konfigurieren                                             */
/* ===================================================================== */

static const audio_sink_t *make_sink(void)
{
    sink_pdm_cfg_t sc;
    memset(&sc, 0, sizeof sc);
    sc.gain = CONFIG_KIWI_PDM_GAIN;
    sc.buffer_ms = CONFIG_KIWI_PDM_BUFFER_MS;
#ifdef CONFIG_KIWI_PDM_OSR_32
    sc.osr_bits = 32;
#else
    sc.osr_bits = 64;
#endif
#ifdef CONFIG_KIWI_PDM_SWAP_WORDS
    sc.swap_words = 1;
#endif
#ifdef CONFIG_KIWI_PDM_BCK_WS_PINS
    sc.use_bck_ws_pins = 1;
#endif
    sc.dma_buf_count = CONFIG_KIWI_PDM_DMA_COUNT;
    sc.dma_buf_len = CONFIG_KIWI_PDM_DMA_LEN;
    return sink_pdm_get(&sc);
}

/* ===================================================================== */
/* KiwiSDR-Task                                                          */
/* ===================================================================== */

static void kiwi_task(void *arg)
{
    (void)arg;
    uint32_t freq_hz = 0;
    char mode[8] = CONFIG_KIWI_MODE;
    if (kiwi_parse_khz(CONFIG_KIWI_FREQ_KHZ, &freq_hz) != 0) {
        ESP_LOGE(TAG, "KIWI_FREQ_KHZ ungueltig: '%s'", CONFIG_KIWI_FREQ_KHZ);
        vTaskDelete(NULL);
    }

    uint32_t backoff_ms = 5000;

    for (;;) {
        /* Erst weitermachen, wenn das WLAN eine IP hat. */
        xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

#ifdef CONFIG_KIWI_PDM_SELFTEST
        /* Hoertest: 1-kHz-Ton fuer 5 s, EINMAL, nach dem WLAN-Start. BEOBACHTUNG am Testgeraet: Vor dem
         * WLAN-Start nahm der I2S-DMA keine Daten ab (Zeitueberschreitungen, 0 Bytes), in der Sitzung
         * nach dem WLAN-Start lief er einwandfrei. Die Ursache ist nicht geklaert, deshalb laeuft der
         * Test jetzt dort, wo auch der echte Betrieb laeuft. */
        static int selftest_done;
        if (!selftest_done) {
            selftest_done = 1;
            sink_pdm_selftest(make_sink(), 5);
        }
#endif

        kiwi_config_t cfg = {
            .host = CONFIG_KIWI_HOST,
            .port = CONFIG_KIWI_PORT,
            .password = CONFIG_KIWI_PASSWORD,
            .user_name = CONFIG_KIWI_USER_NAME,
            .freq_hz = freq_hz,
            .mode = mode,
            .compression = KIWI_USE_ADPCM,
            .sink = make_sink(),
        };

        g_quit_session = 0;
        uint32_t t_session = kiwi_time_ms();
        kiwi_err_t rc = kiwi_client_init(&g_client, &cfg);
        if (rc == KIWI_OK) {
            ESP_LOGI(TAG, "Verbinde %s:%d, %s kHz, Modus %s", CONFIG_KIWI_HOST,
                     CONFIG_KIWI_PORT, CONFIG_KIWI_FREQ_KHZ, mode);
            rc = kiwi_client_connect(&g_client);
        }
        if (rc == KIWI_OK) {
            backoff_ms = 5000;                      /* Verbindung steht -> Wartezeit zuruecksetzen */
            g_active = 1;                           /* ab hier darf die Konsole umstimmen */
            while (!g_quit_session && (rc = kiwi_client_poll(&g_client, 200)) == KIWI_OK) {
#ifdef CONFIG_KIWI_STATUS_LOG
                static uint32_t t_log;
                uint32_t now = kiwi_time_ms();
                if (now - t_log >= 10000u) {
                    char st[160];
                    uint32_t ov, un, np, nm, ic, it, ir, ib;
                    kiwi_client_command(&g_client, "s", st, sizeof st);
                    sink_pdm_stats(&ov, &un, &np, &nm);
                    sink_pdm_diag(&ic, &it, &ir, &ib);
                    ESP_LOGI(TAG, "%s | Ring ueber/unter %u/%u, Korrektur +%u/-%u | I2S Aufrufe %u, Zeitueb. %u, Neustarts %u, Bytes %u | Heap frei %u",
                             st, (unsigned)ov, (unsigned)un, (unsigned)np, (unsigned)nm,
                             (unsigned)ic, (unsigned)it, (unsigned)ir, (unsigned)ib,
                             (unsigned)esp_get_free_heap_size());
                    t_log = now;
                }
#endif
            }
            g_active = 0;
            /* Zuletzt gewaehlte Abstimmung fuer eine Neuverbindung merken. */
            freq_hz = g_client.cur_freq_hz;
            memcpy(mode, g_client.cur_mode, sizeof mode);
            if (g_quit_session) rc = KIWI_ERR_CLOSED;
        }
        ESP_LOGW(TAG, "Sitzung beendet: %s [Server-Meldung: %s] | Dauer %u ms, Frames %u, Keepalives %u, Pings %u, verworfene Nachrichten %u",
                 kiwi_strerror(rc), g_client.err_detail[0] ? g_client.err_detail : "-",
                 (unsigned)(kiwi_time_ms() - t_session), (unsigned)g_client.st.frames,
                 (unsigned)g_client.st.keepalives, (unsigned)g_client.st.ws_pings,
                 (unsigned)g_client.st.dropped_msgs);
        kiwi_client_close(&g_client);              /* IMMER sauber schliessen */

        ESP_LOGI(TAG, "Neuer Versuch in %u s", (unsigned)(backoff_ms / 1000));
        vTaskDelay(backoff_ms / portTICK_PERIOD_MS);
        backoff_ms = backoff_ms >= 60000 ? 60000 : backoff_ms * 2;
    }
}

/* ===================================================================== */
/* Konsole (TCP): Befehle an den Kiwi-Client                             */
/* ===================================================================== */

#ifdef CONFIG_KIWI_NET_CONSOLE
static int console_handler(const char *line, char *reply, size_t cap)
{
    if (!g_active) {
        snprintf(reply, cap, "nicht verbunden");
        return -1;
    }
    int r = kiwi_client_command(&g_client, line, reply, cap);
    if (r == 1) g_quit_session = 1;
    return r;
}
#endif

/* ===================================================================== */

void app_main(void)
{
    /* NVS wird vom WLAN-Treiber benoetigt. */
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    ESP_LOGI(TAG, "Freier Heap beim Start: %u Byte", (unsigned)esp_get_free_heap_size());
    wifi_start();
    xTaskCreate(kiwi_task, "kiwi_net", 4096, NULL, 5, NULL);
#ifdef CONFIG_KIWI_NET_CONSOLE
    net_console_start(CONFIG_KIWI_NET_CONSOLE_PORT, console_handler);
#endif
}
