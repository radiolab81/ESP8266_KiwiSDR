/**
 * @file sink_pdm.c
 * @brief ESP8266-Audio-Senke ueber I2S-Bitstrom (Delta-Sigma) - Gegenstueck zu sink_dac.c
 *        der ESP32-Version.
 *
 * AUFBAU (gleich wie beim ESP32, nur andere Ausgabehardware)
 * ----------------------------------------------------------
 *   Kiwi-Task --sink_write()--> Ringpuffer (RAM) --pdm_task--> I2S-DMA --> GPIO3 --> RC --> Audio
 *
 * UNTERSCHIEDE ZUR ESP32-FASSUNG
 *   1. Ausgabe: Statt "8-Bit-Wert an den DAC" erzeugt der Task einen Dichtestrom
 *      (kiwi_pdm.c) und schreibt ihn mit i2s_write() in den I2S-Block.
 *   2. Ringpuffer: eigener Einzel-Erzeuger/Einzel-Verbraucher-Ring statt FreeRTOS-
 *      StreamBuffer. Der ESP8266 hat nur EINEN Kern, daher genuegt es, den Schreibindex
 *      erst NACH den Daten zu veroeffentlichen (kein Mutex, keine Sperre).
 *   3. Taktkorrektur: Der I2S-Takt des ESP8266 entsteht aus ganzzahligen Teilern und ist
 *      nie ganz exakt (bis ca. 0,5 % Abweichung). Ohne Gegenmassnahme liefe der Puffer
 *      alle paar Minuten leer oder ueber. Der Task misst darum den gemittelten
 *      Fuellstand und streckt/staucht den Datenstrom um je EIN Ausgabesample (siehe
 *      kiwi_pdm_pipe_run, Parameter "extra").
 *   4. Speicher: kleiner Puffer (Standard 300 ms), denn es sind nur rund 50 KB Heap frei.
 */
#include "sink_pdm.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Reihenfolge wichtig: Der SDK-Header driver/i2s.h bindet <stdbool.h> und die FreeRTOS-Typen
 * (TickType_t) NICHT selbst ein, setzt sie aber voraus. Deshalb muessen sie davor stehen. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "kiwi_pdm.h"

static const char *TAG = "sink_pdm";

#define CHUNK_IN       16       /* Eingangssamples je Schleifendurchlauf (~1,3 ms bei 12 kHz) */
#define MAX_WORDS_OUT  (CHUNK_IN * (4 + 1) * 4)   /* up<=4, +1 Korrektur, <=4 Woerter/Sample */
#define SILENCE_WORDS  64

/** Compiler-Barriere: der Compiler darf Speicherzugriffe nicht ueber diese Stelle schieben. */
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

typedef struct {
    sink_pdm_cfg_t cfg;
    int16_t       *ring;                 /* Ringpuffer (Groesse: Zweierpotenz)             */
    uint32_t       mask;                 /* Groesse - 1                                    */
    volatile uint32_t head;              /* Schreibzaehler (nur der Kiwi-Task veraendert)  */
    volatile uint32_t tail;              /* Lesezaehler    (nur der PDM-Task veraendert)   */
    uint32_t       target;               /* Soll-Fuellstand = Startschwelle (Samples)      */
    uint32_t       band;                 /* Toleranz um den Sollwert                       */
    volatile int   run, task_done, opened;
    kiwi_pdm_pipe_t pipe;
    volatile uint32_t overruns, underruns, nudge_plus, nudge_minus;

    /* I2S-Konfiguration gemerkt, damit sich der Treiber bei Stillstand neu aufsetzen laesst */
    i2s_config_t     ic;
    i2s_pin_config_t pins;
    /* Diagnose: Gibt der I2S-Treiber Puffer frei? (siehe i2s_put) */
    volatile uint32_t i2s_calls, i2s_timeouts, i2s_restarts, i2s_bytes;
    uint32_t         stall_run;
} pdm_sink_t;

static pdm_sink_t g;

/* ================================================================== */
/* Audio-Task: Ringpuffer -> Delta-Sigma -> I2S                       */
/* ================================================================== */

/**
 * Wiederanlauf, wenn der I2S-Treiber keine Puffer mehr freigibt.
 *
 * HINTERGRUND (aus dem SDK-Quelltext driver/i2s.c): i2s_write() holt sich den naechsten
 * freien DMA-Puffer aus einer Warteschlange, in die NUR die DMA-Interrupt-Routine
 * abgespielte Puffer zurueckstellt. Laeuft der DMA nicht oder kommt der Interrupt nicht an,
 * wartet i2s_write() mit portMAX_DELAY fuer immer - der Audio-Task steht, der Ringpuffer
 * laeuft voll (Symptom: "Ring ueber" waechst, "unter"/"Korrektur" bleiben 0). Ein
 * aehnliches Verhalten ist im SDK-Tracker beschrieben (Issue #981).
 * Darum: nie ohne Zeitlimit warten, Stillstand zaehlen und stufenweise neu starten.
 */
static void i2s_recover(void)
{
    uint32_t n = ++g.i2s_restarts;
    if (n <= 3 || (n % 20) == 0)
        ESP_LOGW(TAG, "I2S-DMA liefert keine freien Puffer (Zeitueberschreitungen %u) - Wiederanlauf Nr. %u",
                 (unsigned)g.i2s_timeouts, (unsigned)n);

    if (n % 3 == 0) {
        /* Stufe 2: Treiber komplett neu aufsetzen (Warteschlange, DMA-Ring, Interrupt) */
        i2s_driver_uninstall(I2S_NUM_0);
        if (i2s_driver_install(I2S_NUM_0, &g.ic, 0, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "i2s_driver_install im Wiederanlauf fehlgeschlagen");
            return;
        }
        i2s_set_pin(I2S_NUM_0, &g.pins);
    } else {
        /* Stufe 1: Takt und DMA anhalten und neu starten */
        i2s_stop(I2S_NUM_0);
        i2s_start(I2S_NUM_0);
    }
}

/** Schreibt @p n Woerter; wartet nie laenger als 200 ms am Stueck und meldet Stillstand. */
static void i2s_put(const uint16_t *words, size_t n)
{
    const uint8_t *p = (const uint8_t *)words;
    size_t left = n * sizeof(uint16_t);

    while (left && g.run) {
        size_t w = 0;
        /* Im Normalbetrieb blockiert dies kurz (Pacing auf den I2S-Takt). */
        esp_err_t e = i2s_write(I2S_NUM_0, p, left, &w, 200 / portTICK_PERIOD_MS);
        g.i2s_calls++;
        g.i2s_bytes += (uint32_t)w;
        p += w;
        left -= w;
        if (e != ESP_OK || w == 0) {                   /* Zeitueberschreitung oder Fehler */
            g.i2s_timeouts++;
            vTaskDelay(10 / portTICK_PERIOD_MS);       /* nie im Kreis rasen (kaeme sonst nichts anderes dran) */
            if (++g.stall_run >= 3) { g.stall_run = 0; i2s_recover(); }
        } else {
            g.stall_run = 0;
        }
    }
}

static void pdm_task(void *arg)
{
    (void)arg;
    /* static: Die Arbeitspuffer liegen im RAM (.bss) statt auf dem Stack. Der Task-Stack bleibt
     * dadurch klein (wichtig auf dem ESP8266, und die Stack-Einheit ist nicht bestaetigt). */
    static int16_t  in[CHUNK_IN];
    static uint16_t out[MAX_WORDS_OUT];
    static uint16_t silence[SILENCE_WORDS];
    for (int i = 0; i < SILENCE_WORDS; i++) silence[i] = 0xAAAA;  /* 1010... = Pulsdichte 50 % = Stille */

    int prebuffering = 1;
    /* Gleitender Mittelwert des Fuellstands in 1/16 Sample (Zeitkonstante ~ 512 Durchlaeufe
     * = ca. 1,4 s). Glaettet Schwankungen durch grosse Netzwerkpakete (z. B. ADPCM-Frames). */
    int32_t avg16 = (int32_t)g.target * 16;

    while (g.run) {
        uint32_t fill = g.head - g.tail;               /* vorzeichenlose Differenz: auch bei Ueberlauf korrekt */

        if (prebuffering) {
            if (fill < g.target) { i2s_put(silence, SILENCE_WORDS); continue; }
            prebuffering = 0;
            avg16 = (int32_t)g.target * 16;
        }
        if (fill == 0) {                               /* Unterlauf: Stille und neu vorpuffern */
            g.underruns++;
            prebuffering = 1;
            continue;
        }

        uint32_t n = fill < CHUNK_IN ? fill : CHUNK_IN;
        uint32_t t = g.tail;
        for (uint32_t i = 0; i < n; i++) in[i] = g.ring[(t + i) & g.mask];
        COMPILER_BARRIER();                            /* erst fertig lesen, dann Platz freigeben */
        g.tail = t + n;

        /* Taktkorrektur: nur eingreifen, wenn der GEMITTELTE Fuellstand ausserhalb des Bandes liegt. */
        avg16 += ((int32_t)(fill * 16u) - avg16) >> 9;
        int extra = 0;
        if (avg16 > (int32_t)(g.target + g.band) * 16)      { extra = -1; g.nudge_minus++; }  /* zu voll: schneller leeren */
        else if (avg16 < (int32_t)(g.target - g.band) * 16) { extra = +1; g.nudge_plus++;  }  /* zu leer: langsamer leeren */

        size_t nw = kiwi_pdm_pipe_run(&g.pipe, in, n, extra, out);

        if (g.cfg.swap_words) {                        /* je zwei Woerter tauschen (Hardware-Reihenfolge) */
            for (size_t i = 0; i + 1 < nw; i += 2) { uint16_t tmp = out[i]; out[i] = out[i + 1]; out[i + 1] = tmp; }
        }
        i2s_put(out, nw);
    }
    g.task_done = 1;
    vTaskDelete(NULL);
}

/* ================================================================== */
/* audio_sink_t-Schnittstelle                                         */
/* ================================================================== */

static int sink_open(void *ctx, uint32_t rate)
{
    (void)ctx;
    if (rate < 4000 || rate > 48000) return -1;
    if (g.opened) return -1;

    /* Ringpuffer: mindestens buffer_ms, aufgerundet auf eine Zweierpotenz (schnelle Maskierung). */
    uint32_t want = (uint32_t)((uint64_t)rate * (uint32_t)g.cfg.buffer_ms / 1000u);
    uint32_t cap = 256;
    while (cap < want) cap <<= 1;
    g.ring = (int16_t *)malloc(cap * sizeof(int16_t));
    if (!g.ring) { ESP_LOGE(TAG, "kein Speicher fuer den Ringpuffer (%u Byte)", (unsigned)(cap * 2)); return -1; }
    g.mask = cap - 1;
    g.head = g.tail = 0;
    g.target = want / 2;                  /* Start und Sollwert: halber Puffer */
    g.band = g.target / 3;
    g.overruns = g.underruns = g.nudge_plus = g.nudge_minus = 0;

    /* Ausgabepfad: 12 kHz -> 24 kHz (Faktor 2), je Ausgabesample osr_bits Bits. */
    unsigned up = kiwi_pdm_choose_up(rate);
    unsigned words = (unsigned)g.cfg.osr_bits / 16u;
    kiwi_pdm_pipe_init(&g.pipe, up, words, g.cfg.gain);

    /* I2S-Takt: 16 Bit x 2 Kanaele = 32 Bit je Frame. Bits/s = Ausgaberate x OSR.
     * Beispiel: 12 kHz x 2 = 24 kHz; x 64 Bit = 1,536 Mbit/s; / 32 = 48 000 Frames/s. */
    uint32_t i2s_rate = (rate * up * (uint32_t)g.cfg.osr_bits) / 32u;

    memset(&g.ic, 0, sizeof g.ic);        /* Konfiguration bleibt in g: wird beim Wiederanlauf gebraucht */
    g.ic.mode = I2S_MODE_MASTER | I2S_MODE_TX;
    g.ic.sample_rate = (int)i2s_rate;
    g.ic.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    g.ic.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    g.ic.communication_format = I2S_COMM_FORMAT_I2S;
    g.ic.dma_buf_count = g.cfg.dma_buf_count;
    g.ic.dma_buf_len = g.cfg.dma_buf_len;
    g.ic.tx_desc_auto_clear = false;        /* Stille fuettern wir selbst (50 % statt 0 -> kein Knacken) */
    if (i2s_driver_install(I2S_NUM_0, &g.ic, 0, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install fehlgeschlagen");
        free(g.ring); g.ring = NULL;
        return -1;
    }

    /* Pins: Die Datenleitung liegt beim ESP8266 fest auf GPIO3 (RX0). BCK (GPIO15) und WS (GPIO2)
     * brauchen wir nicht - ohne sie bleiben die beiden Pins frei. */
    memset(&g.pins, 0, sizeof g.pins);
    g.pins.bck_o_en = g.cfg.use_bck_ws_pins;
    g.pins.ws_o_en = g.cfg.use_bck_ws_pins;
    g.pins.data_out_en = 1;
    i2s_set_pin(I2S_NUM_0, &g.pins);

    g.i2s_calls = g.i2s_timeouts = g.i2s_restarts = g.i2s_bytes = 0;
    g.stall_run = 0;
    g.run = 1;
    g.task_done = 0;
    if (xTaskCreate(pdm_task, "pdm_out", 3072, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Audio-Task konnte nicht gestartet werden (zu wenig Speicher?)");
        i2s_driver_uninstall(I2S_NUM_0);
        free(g.ring); g.ring = NULL;
        return -1;
    }
    g.opened = 1;
    ESP_LOGI(TAG, "PDM: Quelle %u Hz x%u = %u Hz, %d Bit/Sample, I2S %u Hz, Ring %u Samples (%u Byte)",
             (unsigned)rate, up, (unsigned)(rate * up), g.cfg.osr_bits, (unsigned)i2s_rate,
             (unsigned)cap, (unsigned)(cap * 2));
    return 0;
}

static int sink_write(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    if (!g.opened) return -1;
    uint32_t cap = g.mask + 1u;
    uint32_t room = cap - (g.head - g.tail);
    if (n > room) { g.overruns++; return 0; }          /* nie blockieren: der Netz-Task muss weiterlaufen */
    uint32_t h = g.head;
    for (size_t i = 0; i < n; i++) g.ring[(h + (uint32_t)i) & g.mask] = pcm[i];
    COMPILER_BARRIER();                                /* Daten zuerst, dann den Index veroeffentlichen */
    g.head = h + (uint32_t)n;
    return 0;
}

static void sink_close(void *ctx)
{
    (void)ctx;
    if (!g.opened) return;
    g.run = 0;
    for (int i = 0; i < 100 && !g.task_done; i++) vTaskDelay(10 / portTICK_PERIOD_MS);
    i2s_driver_uninstall(I2S_NUM_0);
    free(g.ring);
    g.ring = NULL;
    g.opened = 0;
    ESP_LOGI(TAG, "PDM geschlossen (Ueberlaeufe %u, Unterlaeufe %u, Korrekturen +%u/-%u, I2S: %u Aufrufe, %u Zeitueberschr., %u Wiederanlaeufe)",
             (unsigned)g.overruns, (unsigned)g.underruns, (unsigned)g.nudge_plus, (unsigned)g.nudge_minus,
             (unsigned)g.i2s_calls, (unsigned)g.i2s_timeouts, (unsigned)g.i2s_restarts);
}

static audio_sink_t s_sink = { sink_open, sink_write, sink_close, NULL };

const audio_sink_t *sink_pdm_get(const sink_pdm_cfg_t *cfg)
{
    g.cfg = *cfg;
    if (g.cfg.osr_bits != 32 && g.cfg.osr_bits != 64) g.cfg.osr_bits = 64;
    if (g.cfg.gain < 1) g.cfg.gain = 1;
    if (g.cfg.gain > 8) g.cfg.gain = 8;
    if (g.cfg.buffer_ms < 100) g.cfg.buffer_ms = 100;
    return &s_sink;
}

void sink_pdm_stats(uint32_t *overruns, uint32_t *underruns, uint32_t *nudge_plus,
                    uint32_t *nudge_minus)
{
    if (overruns) *overruns = g.overruns;
    if (underruns) *underruns = g.underruns;
    if (nudge_plus) *nudge_plus = g.nudge_plus;
    if (nudge_minus) *nudge_minus = g.nudge_minus;
}

void sink_pdm_diag(uint32_t *calls, uint32_t *timeouts, uint32_t *restarts, uint32_t *bytes)
{
    if (calls) *calls = g.i2s_calls;
    if (timeouts) *timeouts = g.i2s_timeouts;
    if (restarts) *restarts = g.i2s_restarts;
    if (bytes) *bytes = g.i2s_bytes;
}

void sink_pdm_selftest(const audio_sink_t *sink, unsigned seconds)
{
    /* 1 kHz bei 12 kHz Abtastrate = genau 12 Samples je Periode: eine kleine Tabelle
     * (Amplitude 8000, Sinus), ganz ohne Gleitkomma. */
    static const int16_t period[12] = { 0, 4000, 6928, 8000, 6928, 4000, 0, -4000, -6928, -8000, -6928, -4000 };
    if (sink->open(sink->ctx, 12000) != 0) { ESP_LOGE(TAG, "Selbsttest: Senke nicht oeffnbar"); return; }

    int16_t block[240];                                /* 20 Perioden = 20 ms */
    for (int i = 0; i < 240; i++) block[i] = period[i % 12];
    ESP_LOGI(TAG, "Selbsttest: 1-kHz-Ton fuer %u s (GPIO3)", seconds);
    for (unsigned b = 0; b < seconds * 50u; b++) {     /* 50 Bloecke je Sekunde */
        sink->write(sink->ctx, block, 240);
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    /* Ergebnis des Tests: Hat der I2S-Treiber Daten abgenommen? */
    uint32_t calls, tmo, rst, bytes;
    sink_pdm_diag(&calls, &tmo, &rst, &bytes);
    if (bytes == 0 || tmo > 0)
        ESP_LOGE(TAG, "Selbsttest: I2S-Treiber haengt! Schreibaufrufe %u, Zeitueberschreitungen %u, Wiederanlaeufe %u, Bytes %u",
                 (unsigned)calls, (unsigned)tmo, (unsigned)rst, (unsigned)bytes);
    else
        ESP_LOGI(TAG, "Selbsttest: I2S-Treiber ok (Schreibaufrufe %u, %u Bytes, keine Zeitueberschreitung)",
                 (unsigned)calls, (unsigned)bytes);
    sink->close(sink->ctx);
}
