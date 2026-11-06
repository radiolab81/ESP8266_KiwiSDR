/**
 * @file kiwi_pdm.h
 * @brief PCM -> 1-Bit-Dichtestrom (PDM / Delta-Sigma) fuer Chips OHNE DAC (ESP8266).
 *
 * LEHRBUCH-HINTERGRUND: Wie macht man aus einem Digitalpin ein Audiosignal?
 * ------------------------------------------------------------------------
 * Ein Pin kann nur "0" oder "3,3 V". Ein Tiefpass (Widerstand + Kondensator) bildet aber
 * den MITTELWERT ueber die Zeit. Schaltet man den Pin sehr schnell und laesst einen
 * Anteil p der Zeit auf "1", entsteht am Tiefpass die Spannung p * 3,3 V:
 *
 *      Sample-Wert  ->  Anteil der "1"-Bits (Pulsdichte)  ->  Tiefpass  ->  Analogspannung
 *
 * Das nennt man PDM (Pulse Density Modulation). Es ist verwandt mit PWM, aber die
 * "1"-Bits werden gleichmaessig verteilt statt zu einem Block zusammengefasst. Dadurch
 * liegt das Quantisierungsrauschen bei sehr hohen Frequenzen, wo der Tiefpass es schluckt
 * ("Noise Shaping").
 *
 * Der hier benutzte Modulator ist ein Delta-Sigma-Modulator 1. Ordnung - in der
 * einfachsten Form ein Akkumulator mit Ueberlauf ("Phasenakkumulator"):
 *
 *      acc += u;              u = Eingangswert 0..65535
 *      bit  = Ueberlauf (Carry) von acc  (acc auf 16 Bit begrenzen)
 *
 * Ist u = 16384 (25 %), kommt etwa bei jedem 4. Takt ein Ueberlauf -> jedes 4. Bit ist "1".
 *
 * Wichtige Kenngroessen:
 *   - OSR (Oversampling-Ratio) = Bits pro Audio-Sample. Mehr Bits pro Sample = weniger
 *     Rauschen im Hoerbereich, aber mehr Rechenzeit und schnellerer Bittakt.
 *   - Theorie 1. Ordnung (Vollaussteuerung): SNR ~ 6,6 dB + 30 dB * log10(OSR_Nutzband).
 *     Beispiel: 24 kHz Ausgabe x 64 Bit = 1,536 Mbit/s, Nutzband 5 kHz (AM-Rundfunk)
 *     -> OSR ~ 150 -> rund 68 dB. GEMESSEN (pc/test_pdm.c, Bitstrom-Spektrum, Ton mit
 *     Pegel 0,24 Vollaussteuerung): 53 dB bei 64 Bit/Sample, 46 dB bei 32 Bit/Sample im
 *     Band 0-5 kHz. Ein 8-Bit-DAC erreicht bei diesem Pegel rechnerisch ca. 38 dB. Das
 *     Kurzwellensignal selbst hat meist nur 30-50 dB SNR - die Ausgabe ist also nicht
 *     der begrenzende Faktor.
 *   - Ausgangsfilter: ein RC-Tiefpass bei ca. 5 kHz (z. B. 3,3 kOhm + 10 nF) entfernt den
 *     Bittakt (Megahertz) und das hochfrequente Rauschen.
 *   - Bekannte Schwaeche der 1. Ordnung: bei sehr einfachen Eingangswerten (z. B. Stille,
 *     exakt 25 %) entstehen periodische Bitmuster ("Idle-Tones"). Bei 1,5 Mbit/s liegen
 *     sie weit oberhalb des Hoerbereichs und werden vom Tiefpass entfernt.
 *
 * Dieser Teil ist reines C ohne Abhaengigkeit vom SDK und laeuft deshalb auch auf dem PC
 * (siehe pc/test_pdm.c: Simulation mit Signal-Rausch-Messung).
 */
#ifndef KIWI_PDM_H
#define KIWI_PDM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bits pro Ausgabewort. Passt zu I2S "16 Bit pro Kanal". */
#define KIWI_PDM_WORD_BITS 16

/** Zustand des Modulators (nur der Akkumulator). */
typedef struct {
    uint32_t acc;
} kiwi_pdm_t;

void kiwi_pdm_init(kiwi_pdm_t *m);

/**
 * Kodiert EIN PCM-Sample als @p nwords * 16 Bit Pulsdichte. Das erste Bit der Zeit-
 * reihenfolge steht im hoechstwertigen Bit (MSB first) des ersten Wortes - so sendet auch
 * der I2S-Ausgang.
 * @param value  Sample, wird auf -32768..32767 begrenzt
 */
void kiwi_pdm_encode(kiwi_pdm_t *m, int32_t value, uint16_t *out, unsigned nwords);

/* ------------------------------------------------------------------ */
/* Kompletter Ausgabepfad: Hochrechnen + Lautstaerke + PDM            */
/* ------------------------------------------------------------------ */

/**
 * Pipeline, wie sie der ESP8266-Audiotask benutzt:
 *   Kiwi-Sample (12 kHz) -> lineare Interpolation auf "up" Ausgabesamples
 *   (z. B. 24 kHz) -> Verstaerkung -> PDM (OSR Bits je Ausgabesample).
 * Die lineare Interpolation unterdrueckt die Spiegelfrequenzen oberhalb von 6 kHz, die
 * sonst durch das einfache Wiederholen von Samples entstuenden.
 */
typedef struct {
    kiwi_pdm_t mod;
    int32_t    prev;             /**< letztes Eingangssample (Startpunkt der Interpolation) */
    unsigned   up;               /**< Ausgabesamples pro Eingangssample (1..4)              */
    unsigned   words;            /**< 16-Bit-Woerter je Ausgabesample (= OSR / 16)          */
    int        gain;             /**< Verstaerkung 1..8                                     */
} kiwi_pdm_pipe_t;

/** Waehlt den Hochrechnungsfaktor so, dass die Ausgaberate etwa 24 kHz betraegt. */
unsigned kiwi_pdm_choose_up(uint32_t source_rate_hz);

void kiwi_pdm_pipe_init(kiwi_pdm_pipe_t *p, unsigned up, unsigned words_per_sample, int gain);

/** Obergrenze fuer die Ausgabeworte zu @p n Eingangssamples (fuer die Puffergroesse). */
size_t kiwi_pdm_pipe_max_words(const kiwi_pdm_pipe_t *p, size_t n);

/**
 * Verarbeitet @p n Eingangssamples.
 * @param extra  Taktkorrektur -1/0/+1: das ERSTE Eingangssample erzeugt ein Ausgabesample
 *               weniger/mehr. So gleicht der Audiotask kleine Taktunterschiede zwischen
 *               Kiwi und I2S-Takt aus, ohne dass ein Sample hart verworfen wird (die
 *               Interpolation haelt den Verlauf glatt).
 * @return Anzahl geschriebener 16-Bit-Woerter in @p out
 */
size_t kiwi_pdm_pipe_run(kiwi_pdm_pipe_t *p, const int16_t *in, size_t n, int extra,
                         uint16_t *out);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_PDM_H */
