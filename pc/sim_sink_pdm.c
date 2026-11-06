/**
 * @file sim_sink_pdm.c
 * @brief Zeitraffer-Simulation der ESP8266-Audiosenke (main/sink_pdm.c) am PC.
 *
 * Die ECHTE Datei sink_pdm.c wird unveraendert uebersetzt; nur FreeRTOS und der I2S-Treiber
 * sind durch kleine Simulationen ersetzt (pthreads, "i2s_write" verbraucht Daten im I2S-Takt).
 * So laesst sich pruefen, was auf dem Chip nur schwer zu messen ist:
 *   - Ringpuffer und Vorpufferung arbeiten ueber Stunden (hier: virtuelle Minuten) stabil
 *   - die Taktkorrektur gleicht einen zu schnellen/langsamen I2S-Takt aus, ohne dass der
 *     Puffer ueber- oder leerlaeuft
 *   - ein Netzwerk-Aussetzer fuehrt zu Stille und anschliessend sauberer Wiederaufnahme
 *   - der ausgegebene Bitstrom enthaelt den eingespeisten Ton in richtiger Hoehe
 *
 * ZEIT: Eine VIRTUELLE Uhr ersetzt die Systemuhr. Sie rueckt nur vor, wenn ALLE Threads
 * schlafen (diskrete Ereignissimulation). Dadurch ist das Ergebnis unabhaengig von der
 * Rechnerlast und der Zahl der CPU-Kerne reproduzierbar - ein Test mit echter Zeit im
 * Zeitraffer waere auf einem ausgelasteten Rechner unzuverlaessig.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"        /* wie im echten SDK: setzt bool und TickType_t voraus */
#include "sink_pdm.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FEHLER Zeile %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

/* ---------------- Virtuelle Uhr ---------------- */

#define MAX_THR 8
static pthread_mutex_t g_vm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vc = PTHREAD_COND_INITIALIZER;
static double g_vtime = 0.0;                    /* virtuelle Zeit in Sekunden */
static int    g_nthreads = 1;                   /* lebende Threads (der Haupt-Thread zaehlt) */
static int    g_nsleeping = 0;
static double g_wake[MAX_THR];                  /* Weckzeit je Slot, HUGE_VAL = wach */
static int    g_slot_used[MAX_THR];
static __thread int t_slot = -1;

static double now_s(void)
{
    pthread_mutex_lock(&g_vm);
    double t = g_vtime;
    pthread_mutex_unlock(&g_vm);
    return t;
}

/** Wenn alle Threads schlafen: Zeit auf die naechste Weckzeit vorstellen und diese Threads wecken. */
static void advance_locked(void)
{
    while (g_nthreads > 0 && g_nsleeping == g_nthreads) {
        double tmin = HUGE_VAL;
        for (int i = 0; i < MAX_THR; i++) if (g_slot_used[i] && g_wake[i] < tmin) tmin = g_wake[i];
        if (tmin == HUGE_VAL) return;
        g_vtime = tmin;
        for (int i = 0; i < MAX_THR; i++)
            if (g_slot_used[i] && g_wake[i] <= g_vtime) { g_wake[i] = HUGE_VAL; g_nsleeping--; }
        pthread_cond_broadcast(&g_vc);
    }
}

static void sleep_until(double t)
{
    pthread_mutex_lock(&g_vm);
    if (t_slot < 0) {
        for (int i = 0; i < MAX_THR; i++) if (!g_slot_used[i]) { g_slot_used[i] = 1; g_wake[i] = HUGE_VAL; t_slot = i; break; }
    }
    if (t > g_vtime) {
        g_wake[t_slot] = t;
        g_nsleeping++;
        advance_locked();
        while (g_wake[t_slot] != HUGE_VAL) pthread_cond_wait(&g_vc, &g_vm);
    }
    pthread_mutex_unlock(&g_vm);
}

/* ---------------- FreeRTOS-Ersatz ---------------- */

typedef struct { void (*fn)(void *); void *arg; } thr_t;
static void *thr_main(void *p) { thr_t t = *(thr_t *)p; free(p); t.fn(t.arg); return NULL; }

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *h)
{
    (void)name; (void)stack; (void)prio; (void)h;
    thr_t *t = (thr_t *)malloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    pthread_mutex_lock(&g_vm); g_nthreads++; pthread_mutex_unlock(&g_vm);   /* zaehlt als "wach", bis er schlaeft */
    pthread_t th;
    if (pthread_create(&th, NULL, thr_main, t) != 0) {
        pthread_mutex_lock(&g_vm); g_nthreads--; pthread_mutex_unlock(&g_vm);
        return 0;
    }
    pthread_detach(th);
    return pdPASS;
}

void vTaskDelete(TaskHandle_t h)
{
    (void)h;
    pthread_mutex_lock(&g_vm);
    if (t_slot >= 0) { g_slot_used[t_slot] = 0; t_slot = -1; }
    g_nthreads--;
    advance_locked();                            /* die uebrigen schlafen evtl. schon alle */
    pthread_mutex_unlock(&g_vm);
    pthread_exit(NULL);
}

void vTaskDelay(TickType_t ticks) { sleep_until(now_s() + (double)ticks * 1e-3); }

/* ---------------- I2S-Ersatz ---------------- */

static double g_i2s_rate_hz, g_drift;            /* g_drift: +0.003 = Takt 0,3 % zu schnell */
static double g_deadline;                        /* wann die bisher "gesendeten" Bits fertig waeren */
static pthread_mutex_t g_cap_mx = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *g_cap; static size_t g_cap_n, g_cap_max; static int g_capturing;
static uint32_t g_silence_words, g_audio_words;

/* Simulierter Fehler "DMA liefert keine Interrupts" (wie im SDK-Issue #981): i2s_write() wartet bis zum
 * Zeitlimit und liefert 0 Bytes. Je nach Modus behebt das erst ein Neustart (i2s_stop/start) oder
 * erst ein komplettes Neuaufsetzen des Treibers (uninstall + install) - oder nie. */
static int g_stall_mode;                          /* 0 kein Fehler, 1 bis zum 2. Start, 2 bis zur Neuinstallation, 3 nie */
static int g_stalled, g_starts, g_installs, g_uninstalls;
static int g_installed;

esp_err_t i2s_driver_install(i2s_port_t p, const i2s_config_t *c, int q, void *qh)
{
    (void)p; (void)q; (void)qh;
    g_i2s_rate_hz = c->sample_rate; g_deadline = now_s();
    g_installs++; g_installed = 1;
    if (g_stall_mode == 2 && g_installs >= 2) g_stalled = 0;     /* Neuinstallation behebt es */
    return ESP_OK;
}
esp_err_t i2s_driver_uninstall(i2s_port_t p) { (void)p; g_uninstalls++; g_installed = 0; return ESP_OK; }
esp_err_t i2s_start(i2s_port_t p)
{
    (void)p; g_starts++;
    if (g_stall_mode == 1 && g_starts >= 2) g_stalled = 0;       /* zweiter Neustart behebt es */
    return ESP_OK;
}
esp_err_t i2s_stop(i2s_port_t p) { (void)p; return ESP_OK; }
esp_err_t i2s_set_pin(i2s_port_t p, const i2s_pin_config_t *pins) { (void)p; (void)pins; return ESP_OK; }

esp_err_t i2s_write(i2s_port_t p, const void *src, size_t size, size_t *written, TickType_t wait)
{
    (void)p;
    const uint16_t *w = (const uint16_t *)src;
    size_t nw = size / 2;
    if (!g_installed) { if (written) *written = 0; return -1; }  /* Treiber nicht installiert: sofort Fehler */
    if (g_stalled) {                                             /* DMA haengt: Zeitlimit abwarten, nichts schreiben */
        if (wait != portMAX_DELAY) sleep_until(now_s() + (double)wait * 1e-3);
        else for (;;) sleep_until(now_s() + 3600.0);             /* haengt "fuer immer" (nur ohne Zeitlimit) */
        if (written) *written = 0;
        return ESP_OK;
    }
    /* Schreibdauer = Bits / Bittakt (je Frame 32 Bit bei i2s_rate Frames/s) */
    double dur = (double)nw * 16.0 / (g_i2s_rate_hz * 32.0 * (1.0 + g_drift));
    double now = now_s();
    if (g_deadline < now) g_deadline = now;              /* DMA lief leer: ab jetzt neu zaehlen */
    g_deadline += dur;
    sleep_until(g_deadline);

    int is_silence = 1;
    for (size_t i = 0; i < nw && is_silence; i++) if (w[i] != 0xAAAA) is_silence = 0;
    if (is_silence) g_silence_words += (uint32_t)nw; else g_audio_words += (uint32_t)nw;

    pthread_mutex_lock(&g_cap_mx);
    if (g_capturing && g_cap_n + nw <= g_cap_max) { memcpy(g_cap + g_cap_n, w, nw * 2); g_cap_n += nw; }
    pthread_mutex_unlock(&g_cap_mx);
    if (written) *written = size;
    return ESP_OK;
}

/* ---------------- Testaufbau ---------------- */

static const sink_pdm_cfg_t k_cfg = { .gain = 1, .buffer_ms = 300, .osr_bits = 64, .swap_words = 0,
                                      .use_bck_ws_pins = 0, .dma_buf_count = 6, .dma_buf_len = 256 };

static const int16_t k_period[12] = { 0, 4000, 6928, 8000, 6928, 4000, 0, -4000, -6928, -8000, -6928, -4000 };

/** Erzeugt wie der Kiwi-Task 512-Sample-Bloecke im Takt von 12 kHz. stall_*: Aussetzer in virtueller Zeit. */
static void feed(const audio_sink_t *s, double virt_seconds, double stall_at, double stall_len)
{
    int16_t blk[512];
    int ph = 0;
    double t0 = now_s(), next = t0, per = 512.0 / 12000.0;
    double tend = t0 + virt_seconds;
    int stalled = 0;
    while (now_s() < tend) {
        double v = now_s() - t0;
        if (stall_len > 0 && !stalled && v >= stall_at) {          /* Netz haengt */
            sleep_until(now_s() + stall_len);
            stalled = 1; next = now_s();
        }
        for (int i = 0; i < 512; i++) blk[i] = k_period[(ph++) % 12];
        s->write(s->ctx, blk, 512);
        next += per;
        sleep_until(next);
    }
}

typedef struct { uint32_t ov, un, np, nm, silence, audio; } result_t;

static result_t run_scenario(double drift, double virt_s, double stall_at, double stall_len)
{
    g_drift = drift; g_silence_words = g_audio_words = 0;
    g_installed = 1;
    const audio_sink_t *s = sink_pdm_get(&k_cfg);
    CHECK(s->open(s->ctx, 12000) == 0);
    feed(s, virt_s, stall_at, stall_len);
    result_t r;
    sink_pdm_stats(&r.ov, &r.un, &r.np, &r.nm);
    r.silence = g_silence_words; r.audio = g_audio_words;
    s->close(s->ctx);
    return r;
}

int main(void)
{
    (void)0;
    printf("  Simulation (virtuelle Uhr), 12 kHz Quelle, 300 ms Puffer, 64 Bit/Sample\n");

    /* 1) Verschiedene Takt-Abweichungen des I2S-Blocks: Puffer darf nie ueber-/leerlaufen */
    static const double drifts[] = { 0.0, +0.003, -0.003, +0.005, -0.005 };
    for (size_t i = 0; i < sizeof drifts / sizeof drifts[0]; i++) {
        result_t r = run_scenario(drifts[i], 45.0, 0, 0);
        printf("    Takt %+.1f %%: Ueberlauf %u, Unterlauf %u, Korrektur +%u/-%u\n",
               drifts[i] * 100.0, (unsigned)r.ov, (unsigned)r.un, (unsigned)r.np, (unsigned)r.nm);
        CHECK(r.ov == 0);
        CHECK(r.un == 0);
        if (drifts[i] > 0.001)  CHECK(r.np > 0 && r.np > 5 * r.nm);   /* I2S zu schnell -> Daten strecken (+) */
        if (drifts[i] < -0.001) CHECK(r.nm > 0 && r.nm > 5 * r.np);   /* I2S zu langsam -> Daten stauchen (-) */
        if (drifts[i] == 0.0)   CHECK(r.np + r.nm < 100);             /* ohne Abweichung kaum Eingriffe */
    }

    /* 2) Netzwerk-Aussetzer von 1,5 s: Unterlauf, Stille, danach geht es sauber weiter */
    result_t r = run_scenario(0.0, 20.0, 8.0, 1.5);
    printf("    Netz-Aussetzer 1,5 s: Unterlauf %u, Stille %u Woerter, Audio %u Woerter\n",
           (unsigned)r.un, (unsigned)r.silence, (unsigned)r.audio);
    CHECK(r.un >= 1 && r.un <= 3);
    CHECK(r.audio > 100000);                                          /* danach wieder Audio */

    /* 2b) I2S-DMA haengt (kein Interrupt): Der Audio-Task darf nicht fuer immer blockieren */
    static const struct { int mode; const char *name; } stalls[] = {
        { 1, "behoben durch 2. Neustart (stop/start)" },
        { 2, "behoben erst durch Treiber-Neuinstallation" },
    };
    for (size_t i = 0; i < sizeof stalls / sizeof stalls[0]; i++) {
        g_stall_mode = stalls[i].mode; g_stalled = 1; g_starts = 0; g_installs = 0; g_uninstalls = 0;
        result_t rs = run_scenario(0.0, 12.0, 0, 0);
        uint32_t calls, tmo, rst, bytes;
        sink_pdm_diag(&calls, &tmo, &rst, &bytes);
        printf("    DMA-Stillstand %s: Zeitueb. %u, Neustarts %u, Audio %u Woerter\n", stalls[i].name,
               (unsigned)tmo, (unsigned)rst, (unsigned)rs.audio);
        CHECK(tmo >= 3 && rst >= 1);                 /* Stillstand erkannt und Wiederanlauf versucht */
        CHECK(!g_stalled);                           /* ... und erfolgreich */
        CHECK(rs.audio > 100000);                    /* danach laeuft das Audio */
    }
    {   /* dauerhafter Stillstand: close() darf nicht haengen, Ring darf nicht ausser Kontrolle geraten */
        g_stall_mode = 3; g_stalled = 1; g_starts = 0; g_installs = 0; g_uninstalls = 0;
        result_t rp = run_scenario(0.0, 6.0, 0, 0);
        uint32_t calls, tmo, rst, bytes;
        sink_pdm_diag(&calls, &tmo, &rst, &bytes);
        printf("    Dauerhafter DMA-Stillstand: Zeitueb. %u, Neustarts %u, Bytes %u, close() kehrt zurueck\n",
               (unsigned)tmo, (unsigned)rst, (unsigned)bytes);
        CHECK(tmo >= 6 && rst >= 3);
        CHECK(bytes == 0 && rp.audio == 0);
        CHECK(g_uninstalls >= 1);                    /* Stufe 2 (Neuinstallation) wurde versucht */
    }
    g_stall_mode = 0; g_stalled = 0; g_installed = 1;

    /* 3) Ausgegebener Bitstrom: 1-kHz-Ton in richtiger Hoehe? (Takt exakt, Ton ueber 4 s) */
    g_cap_max = 1536000 / 16 * 6; g_cap = (uint16_t *)malloc(g_cap_max * 2); g_cap_n = 0;
    g_drift = 0.0;
    const audio_sink_t *s = sink_pdm_get(&k_cfg);
    CHECK(s->open(s->ctx, 12000) == 0);
    g_capturing = 0;
    pthread_t dummy; (void)dummy;
    /* erst 2 virtuelle Sekunden einschwingen lassen, dann mitschneiden */
    { double t0 = now_s(); int16_t blk[512]; int ph = 0; double next = t0, per = 512.0 / 12000.0;
      while (now_s() < t0 + 6.0) {
          if (now_s() >= t0 + 2.0) { pthread_mutex_lock(&g_cap_mx); g_capturing = 1; pthread_mutex_unlock(&g_cap_mx); }
          for (int i = 0; i < 512; i++) blk[i] = k_period[(ph++) % 12];
          s->write(s->ctx, blk, 512); next += per; sleep_until(next); } }
    pthread_mutex_lock(&g_cap_mx); g_capturing = 0; pthread_mutex_unlock(&g_cap_mx);
    s->close(s->ctx);

    /* Mitschnitt in 64-Bit-Gruppen mitteln (= 24 kHz) und Ton bei 1 kHz messen */
    size_t nsamp = g_cap_n / 4;                                        /* 4 Woerter = 64 Bit je Ausgabesample */
    double *y = (double *)malloc(sizeof(double) * nsamp);
    for (size_t n = 0; n < nsamp; n++) {
        int ones = 0;
        for (int w = 0; w < 4; w++) for (int b = 0; b < 16; b++) ones += (g_cap[n * 4 + (size_t)w] >> b) & 1;
        y[n] = 2.0 * ones / 64.0 - 1.0;
    }
    double mean = 0; for (size_t n = 0; n < nsamp; n++) mean += y[n]; mean /= (double)nsamp;
    double w = 2.0 * M_PI * 1000.0 / 24000.0, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (size_t n = 0; n < nsamp; n++) { double s0 = (y[n] - mean) + c * s1 - s2; s2 = s1; s1 = s0; }
    double amp = 2.0 * sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (double)nsamp;
    printf("    Mitschnitt: %zu Ausgabesamples, 1-kHz-Amplitude %.3f (Soll %.3f), Gleichanteil %+.4f\n", nsamp, amp, 8000.0 / 32768.0, mean);
    CHECK(nsamp > 24000 * 3 / 2);                                       /* mindestens ca. 1,5 s */
    CHECK(fabs(amp - 8000.0 / 32768.0) < 0.03);
    CHECK(fabs(mean) < 0.02);
    free(y); free(g_cap);

    if (g_fail) { printf("%d Test(s) fehlgeschlagen\n", g_fail); return 1; }
    printf("Alle Simulationstests der Audio-Senke bestanden.\n");
    return 0;
}
