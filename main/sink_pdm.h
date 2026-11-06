/**
 * @file sink_pdm.h
 * @brief ESP8266-Audio-Senke: 16-Bit-PCM -> 1-Bit-Dichtestrom ueber I2S (Pin GPIO3 / "RX").
 *
 * Der ESP8266 hat KEINEN DAC. Der I2S-Block kann aber einen beliebigen Bitstrom per DMA
 * ausgeben. Wir fuettern ihn mit einem Delta-Sigma-Bitstrom (siehe kiwi_pdm.h); ein
 * RC-Tiefpass am Pin macht daraus Audio.
 *
 *     GPIO3 ---[ 3,3 kOhm ]---+---[ 10 uF ]---> Verstaerker / Kopfhoerer-Eingang
 *                            |
 *                          [ 10 nF ]
 *                            |
 *                           GND
 *
 * (Tiefpass ca. 5 kHz; der Kondensator in Reihe entfernt die Gleichspannung. Ein Pegel-
 *  steller 1-10 kOhm zwischen Kondensator und Verstaerker ist sinnvoll. Nie direkt an
 *  Lautsprecher/Kopfhoerer: der Pin liefert nur wenige mA.)
 */
#ifndef SINK_PDM_H
#define SINK_PDM_H

#include "kiwi_audio_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gain;            /**< Software-Verstaerkung 1..8                                   */
    int buffer_ms;       /**< Jitterpuffer in ms (Ringpuffer im RAM, 12 kHz x 2 Byte)      */
    int osr_bits;        /**< PDM-Bits je Ausgabesample: 32 oder 64                        */
    int swap_words;      /**< 1 = je zwei 16-Bit-Woerter vertauschen (siehe README)        */
    int use_bck_ws_pins; /**< 1 = I2S-Takt auf GPIO15/GPIO2 ausgeben (nicht noetig)        */
    int dma_buf_count;   /**< Anzahl I2S-DMA-Puffer                                        */
    int dma_buf_len;     /**< Laenge je DMA-Puffer (Einheit laut SDK)                      */
} sink_pdm_cfg_t;

/** Liefert die (einzige) PDM-Senke. Die Konfiguration wird kopiert. */
const audio_sink_t *sink_pdm_get(const sink_pdm_cfg_t *cfg);

/** Zaehler fuer Diagnose: Ueberlaeufe, Unterlaeufe, Taktkorrekturen (+1 / -1). */
void sink_pdm_stats(uint32_t *overruns, uint32_t *underruns, uint32_t *nudge_plus,
                    uint32_t *nudge_minus);

/**
 * Diagnose des I2S-Treibers: Anzahl i2s_write()-Aufrufe, Zeitueberschreitungen (der Treiber gab
 * keinen freien Puffer frei), Wiederanlaeufe und tatsaechlich abgenommene Bytes. Zeitueberschreitungen
 * > 0 bei Bytes ~ 0 bedeuten: der I2S-DMA laeuft nicht (siehe README, Abschnitt 5).
 */
void sink_pdm_diag(uint32_t *calls, uint32_t *timeouts, uint32_t *restarts, uint32_t *bytes);

/**
 * Hoertest ohne WLAN: spielt @p seconds lang einen 1-kHz-Ton ueber denselben Pfad.
 * Damit prueft man Schaltung und Pegel, bevor Netzwerk ins Spiel kommt.
 */
void sink_pdm_selftest(const audio_sink_t *sink, unsigned seconds);

#ifdef __cplusplus
}
#endif
#endif /* SINK_PDM_H */
