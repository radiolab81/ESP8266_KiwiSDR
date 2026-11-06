/**
 * @file net_console.c
 * @brief TCP-Textkonsole (siehe net_console.h). Nutzt die lwIP-Socket-Funktionen direkt.
 *
 * ABLAUF: socket -> bind -> listen -> (accept -> Zeilen lesen -> Handler -> Antwort) ...
 * Es wird immer nur EINE Verbindung bedient; weitere warten im Listen-Backlog.
 */
#include "net_console.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "net_console";

static net_console_handler_t s_handler;
static uint16_t s_port;

static void send_str(int fd, const char *s)
{
    lwip_send(fd, s, strlen(s), 0);
}

/** Bedient eine Verbindung bis sie endet. */
static void serve(int fd)
{
    char line[48];
    size_t n = 0;
    int iac_skip = 0;                         /* Telnet-Aushandlung (0xFF + 2 Bytes) ueberlesen */
    uint8_t buf[32];

    send_str(fd, "KiwiSDR-Client Konsole: f <kHz> | m <modus> | s | q\r\n> ");
    for (;;) {
        int r = lwip_recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return;                   /* Gegenseite hat geschlossen oder Fehler */

        for (int i = 0; i < r; i++) {
            uint8_t c = buf[i];
            if (iac_skip) { iac_skip--; continue; }
            if (c == 0xFF) { iac_skip = 2; continue; }

            if (c == '\n' || c == '\r') {
                if (n) {
                    char reply[200];
                    line[n] = '\0';
                    reply[0] = '\0';
                    s_handler(line, reply, sizeof reply);
                    send_str(fd, reply);
                    n = 0;
                }
                send_str(fd, "\r\n> ");
            } else if (c == 0x08 || c == 0x7F) {          /* Rueck-/Entfernen-Taste */
                if (n) n--;
            } else if (c >= 0x20 && n < sizeof line - 1) {
                line[n++] = (char)c;
            }
        }
    }
}

static void console_task(void *arg)
{
    (void)arg;
    int ls = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { ESP_LOGE(TAG, "socket() fehlgeschlagen"); vTaskDelete(NULL); return; }

    int one = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(s_port);
    if (lwip_bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || lwip_listen(ls, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen auf Port %u fehlgeschlagen", (unsigned)s_port);
        lwip_close(ls);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Konsole lauscht auf TCP-Port %u", (unsigned)s_port);

    for (;;) {
        int fd = lwip_accept(ls, NULL, NULL);
        if (fd < 0) { vTaskDelay(1000 / portTICK_PERIOD_MS); continue; }
        serve(fd);
        lwip_close(fd);
    }
}

int net_console_start(uint16_t port, net_console_handler_t handler)
{
    s_handler = handler;
    s_port = port;
    return xTaskCreate(console_task, "console", 3072, NULL, 3, NULL) == pdPASS ? 0 : -1;
}
