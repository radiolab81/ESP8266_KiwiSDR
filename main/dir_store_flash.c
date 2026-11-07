/**
 * @file dir_store_flash.c
 * @brief Flash-Partition als kiwi_dir_store_t (Konzept A/B-Slots: kiwi_dir_store.h).
 *
 * Besonderheit Loeschen: Ein 4-KB-Sektor zu loeschen dauert ca. 30-50 ms, und
 * waehrenddessen steht der Zugriff auf den Flash (und damit Code im Flash!) still.
 * Der Audiopfad ist waehrenddessen pausiert; wir
 * loeschen trotzdem sektorweise und geben dazwischen die CPU frei. Ein einzelnes
 * Update loescht einmal 192 KB = 48 Sektoren (ca. 2-3 s; auf dem ESP8266 pausiert das Audio ohnehin, siehe app_dir.c); leichte Aussetzer
 * waehrend dieser Zeit sind moeglich, bleiben aber meist vom Puffer verdeckt.
 */
#include "dir_store_flash.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dir_flash";
static const esp_partition_t *s_part;
static size_t s_slot;

/* ---- Ausrichtung -------------------------------------------------------
 * Der SPI-Flash-Treiber des ESP8266 arbeitet in 32-Bit-Woertern: Flash-Adresse, Laenge UND
 * RAM-Puffer sollen durch 4 teilbar sein. Die Datenbank liest/schreibt aber auch
 * Einzelbytes und 120-Byte-Records an beliebigen Zeigern. Darum gibt es einen kleinen
 * ausgerichteten Zwischenpuffer ("Bounce-Buffer"); durchgereicht wird nur, was passt.
 * Beim Schreiben werden Randbytes mit 0xFF aufgefuellt: 0xFF auf frisch geloeschtem
 * Flash aendert keine Bits, die Nachbardaten bleiben also unberuehrt. */
#define BOUNCE 64
#define ALIGNED4(x) ((((uintptr_t)(x)) & 3u) == 0)

static int raw_read(size_t addr, void *buf, size_t len)
{
    if (ALIGNED4(addr) && ALIGNED4(buf) && ALIGNED4(len))
        return esp_partition_read(s_part, addr, buf, len) == ESP_OK ? 0 : -1;
    uint32_t tmp[BOUNCE / 4];
    uint8_t *d = buf;
    while (len) {
        size_t a = addr & ~(size_t)3, skip = addr - a;
        size_t take = BOUNCE - skip < len ? BOUNCE - skip : len;
        size_t rd = (skip + take + 3) & ~(size_t)3;
        if (esp_partition_read(s_part, a, tmp, rd) != ESP_OK) return -1;
        memcpy(d, (uint8_t *)tmp + skip, take);
        d += take; addr += take; len -= take;
    }
    return 0;
}

static int raw_write(size_t addr, const void *buf, size_t len)
{
    if (ALIGNED4(addr) && ALIGNED4(buf) && ALIGNED4(len))
        return esp_partition_write(s_part, addr, buf, len) == ESP_OK ? 0 : -1;
    uint32_t tmp[BOUNCE / 4];
    const uint8_t *s = buf;
    while (len) {
        size_t a = addr & ~(size_t)3, skip = addr - a;
        size_t take = BOUNCE - skip < len ? BOUNCE - skip : len;
        size_t wr = (skip + take + 3) & ~(size_t)3;
        memset(tmp, 0xFF, wr);
        memcpy((uint8_t *)tmp + skip, s, take);
        if (esp_partition_write(s_part, a, tmp, wr) != ESP_OK) return -1;
        s += take; addr += take; len -= take;
    }
    return 0;
}

static int fl_read(void *ctx, int slot, size_t off, void *buf, size_t len)
{
    (void)ctx;
    if (off + len > s_slot) return -1;
    return raw_read((size_t)slot * s_slot + off, buf, len);
}

static int fl_erase(void *ctx, int slot)
{
    (void)ctx;
    for (size_t o = 0; o < s_slot; o += 4096) {
        if (esp_partition_erase_range(s_part, (size_t)slot * s_slot + o, 4096) != ESP_OK) return -1;
        vTaskDelay(1);                       /* CPU freigeben: Audio-/WLAN-Tasks duerfen dazwischen laufen */
    }
    return 0;
}

static int fl_write(void *ctx, int slot, size_t off, const void *buf, size_t len)
{
    (void)ctx;
    if (off + len > s_slot) return -1;
    return raw_write((size_t)slot * s_slot + off, buf, len);
}

static kiwi_dir_store_t s_store = { 0, 4096, fl_read, fl_erase, fl_write, NULL };

const kiwi_dir_store_t *dir_store_flash_get(void)
{
    if (s_part) return &s_store;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "kiwidb");
    if (!s_part) {
        ESP_LOGE(TAG, "Partition 'kiwidb' fehlt - partitions.csv aktiv? (make menuconfig: Partition Table = Custom; sdkconfig ggf. loeschen)");
        return NULL;
    }
    s_slot = (s_part->size / KIWI_DIR_SLOTS) & ~(size_t)4095;
    s_store.slot_size = s_slot;
    ESP_LOGI(TAG, "Partition kiwidb: %u KB, 2 Slots zu %u KB", (unsigned)(s_part->size / 1024), (unsigned)(s_slot / 1024));
    return &s_store;
}
