/**
 * @file test_pdm.c
 * @brief Selbsttests fuer den Delta-Sigma-Modulator und den ESP8266-Ausgabepfad
 *        (laeuft auf dem PC, ohne ESP8266).
 *
 * Wichtigster Test: Ein 1-kHz-Ton wird ueber DIESELBE Funktionskette wie auf dem Chip
 * (Interpolation 12 -> 24 kHz, Verstaerkung, 1-Bit-Modulator mit 64 Bit je Sample) in einen
 * Bitstrom verwandelt. Danach wird er wie von einem Tiefpass "zurueckgemittelt" und
 * seine Qualitaet gemessen (Signal-Rausch-Abstand im Nutzband 0-5 kHz).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiwi_pdm.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FEHLER Zeile %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

/** Zaehlt die "1"-Bits in n Woertern. */
static unsigned popcount_words(const uint16_t *w, size_t n)
{
    unsigned c = 0;
    for (size_t i = 0; i < n; i++) for (int b = 0; b < 16; b++) c += (w[i] >> b) & 1u;
    return c;
}

static void test_basics(void)
{
    kiwi_pdm_t m;
    uint16_t w[8];

    /* Bitreihenfolge: MSB zuerst. Bei maximalem Eingang (u=65535) ist das erste Bit 0 (der
     * Akkumulator laeuft erst beim 2. Takt ueber), danach nur Einsen: 0111 1111 1111 1111. */
    kiwi_pdm_init(&m);
    kiwi_pdm_encode(&m, 32767, w, 1);
    CHECK(w[0] == 0x7FFF);

    /* Stille (0) = 50 % Pulsdichte, abwechselnd 1010...: halbe Versorgung am Ausgang */
    kiwi_pdm_init(&m);
    kiwi_pdm_encode(&m, 0, w, 4);
    CHECK(popcount_words(w, 4) == 32);                 /* genau die Haelfte von 64 Bit */
    CHECK(w[0] == 0x5555 || w[0] == 0xAAAA);

    /* Dichte entspricht dem Wert: v -> (v+32768)/65536 */
    static const int vals[] = { -32768, -20000, -8192, 0, 8192, 20000, 32767 };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        uint16_t buf[256];                             /* 4096 Bit */
        kiwi_pdm_init(&m);
        kiwi_pdm_encode(&m, vals[i], buf, 256);
        double dens = popcount_words(buf, 256) / 4096.0;
        double want = (vals[i] + 32768) / 65536.0;
        CHECK(fabs(dens - want) < 0.002);
    }

    /* Uebersteuerung wird begrenzt, nicht umgebrochen */
    uint16_t big[8], max[8];
    kiwi_pdm_init(&m);  kiwi_pdm_encode(&m, 1000000, big, 8);
    kiwi_pdm_init(&m);  kiwi_pdm_encode(&m, 32767, max, 8);
    CHECK(memcmp(big, max, sizeof big) == 0);
}

static void test_pipe_counts(void)
{
    kiwi_pdm_pipe_t p;
    int16_t in[10] = { 0, 100, 200, 300, 400, 500, 600, 700, 800, 900 };
    uint16_t out[10 * 3 * 4];

    CHECK(kiwi_pdm_choose_up(12000) == 2 && kiwi_pdm_choose_up(8000) == 3 &&
          kiwi_pdm_choose_up(24000) == 1 && kiwi_pdm_choose_up(48000) == 1);

    kiwi_pdm_pipe_init(&p, 2, 4, 1);                   /* 12 -> 24 kHz, 64 Bit je Sample */
    CHECK(kiwi_pdm_pipe_run(&p, in, 10, 0, out) == 10 * 2 * 4);
    kiwi_pdm_pipe_init(&p, 2, 4, 1);
    CHECK(kiwi_pdm_pipe_run(&p, in, 10, +1, out) == 10 * 2 * 4 + 4);   /* ein Ausgabesample mehr */
    kiwi_pdm_pipe_init(&p, 2, 4, 1);
    CHECK(kiwi_pdm_pipe_run(&p, in, 10, -1, out) == 10 * 2 * 4 - 4);   /* ein Ausgabesample weniger */
    kiwi_pdm_pipe_init(&p, 1, 4, 1);
    CHECK(kiwi_pdm_pipe_run(&p, in, 10, -1, out) == 10 * 1 * 4);       /* nie unter 1 je Eingang */
    CHECK(kiwi_pdm_pipe_max_words(&p, 10) == 10 * (1 + 1) * 4);

    /* Parameter werden begrenzt */
    kiwi_pdm_pipe_init(&p, 99, 0, 99);
    CHECK(p.up == 4 && p.words == 1 && p.gain == 8);
}

/** Leistung des Bitstroms (+1/-1) bei Frequenz-Bin k mittels Goertzel (M Bits). */
static double goertzel_pow(const signed char *x, size_t m, int k)
{
    double w = 2.0 * M_PI * (double)k / (double)m, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < m; i++) {
        double s0 = (double)x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

typedef struct { double snr_ideal_db; double snr_rc_db; double amp; } quality_t;

/**
 * Fuehrt einen Ton durch die Pipeline und misst die Qualitaet DIREKT im Spektrum des
 * Bitstroms (kein Zwischen-Dezimieren, das Rauschen hereinfalten wuerde):
 *   snr_ideal_db : Nutzband 0..5 kHz mit idealem Rechteck-Tiefpass
 *   snr_rc_db    : Hoerbereich 0..16 kHz hinter einem einfachen RC-Tiefpass (Grenzfrequenz
 *                  5 kHz) - so wie es die echte Schaltung mit R + C macht. Signal und
 *                  Rauschen werden mit |H(f)|^2 = 1 / (1 + (f/5000)^2) bewertet.
 */
static quality_t measure(int osr_bits, int gain, double amp_in)
{
    const int N = 2048;                                /* Ausgabesamples @ 24 kHz in der Messung */
    const double fout = 24000.0, ftone = 1031.25;      /* 88 volle Perioden in N -> kein Leck */
    const int lead = 256;                              /* Einschwingen vorweg verwerfen */
    int up = 2, words = osr_bits / 16;
    int nin = (N + 2 * lead) / up;

    int16_t *in = malloc(sizeof(int16_t) * (size_t)nin);
    for (int i = 0; i < nin; i++)
        in[i] = (int16_t)lround(amp_in * sin(2.0 * M_PI * ftone * i / (fout / up)));

    kiwi_pdm_pipe_t p;
    kiwi_pdm_pipe_init(&p, (unsigned)up, (unsigned)words, gain);
    uint16_t *bits = malloc(sizeof(uint16_t) * kiwi_pdm_pipe_max_words(&p, (size_t)nin));
    size_t nw = kiwi_pdm_pipe_run(&p, in, (size_t)nin, 0, bits);
    CHECK(nw == (size_t)nin * (size_t)up * (size_t)words);

    /* Auswertefenster: N Ausgabesamples = N*osr Bits, als +1/-1 */
    size_t m = (size_t)N * (size_t)osr_bits;
    signed char *x = malloc(m);
    size_t w0 = (size_t)lead * (size_t)words;
    for (size_t i = 0; i < m; i++) {
        uint16_t w = bits[w0 + i / 16];
        x[i] = ((w >> (15 - i % 16)) & 1) ? 1 : -1;        /* MSB first = Zeitreihenfolge */
    }

    double fbit = fout * osr_bits, df = fbit / (double)m;   /* Bin-Abstand in Hz */
    int ksig = (int)lround(ftone / df);
    int k5 = (int)(5000.0 / df), k16 = (int)(16000.0 / df);
    double psig = goertzel_pow(x, m, ksig), n_ideal = 0, n_rc = 0;
    for (int k = 1; k <= k16; k++) {
        if (k == ksig) continue;
        double pk = goertzel_pow(x, m, k), f = k * df;
        if (k <= k5) n_ideal += pk;
        n_rc += pk / (1.0 + (f / 5000.0) * (f / 5000.0));
    }
    double hsig = 1.0 / (1.0 + (ftone / 5000.0) * (ftone / 5000.0));

    quality_t q;
    q.snr_ideal_db = 10.0 * log10(psig / n_ideal);
    q.snr_rc_db = 10.0 * log10(psig * hsig / n_rc);
    q.amp = 2.0 * sqrt(psig) / (double)m;                   /* Amplitude des Tons (Vollaussteuerung = 1) */
    free(in); free(bits); free(x);
    return q;
}

static void test_tone_quality(void)
{
    quality_t q64 = measure(64, 1, 8000.0), q32 = measure(32, 1, 8000.0), q64g = measure(64, 2, 8000.0);
    printf("  Messung: 1 kHz, Pegel 8000/32768, 24 kHz Ausgabe\n");
    printf("    OSR 64 Bit/Sample (1,536 Mbit/s): SNR 0-5 kHz ideal %.1f dB | 0-16 kHz hinter RC(5 kHz) %.1f dB\n", q64.snr_ideal_db, q64.snr_rc_db);
    printf("    OSR 32 Bit/Sample (0,768 Mbit/s): SNR 0-5 kHz ideal %.1f dB | 0-16 kHz hinter RC(5 kHz) %.1f dB\n", q32.snr_ideal_db, q32.snr_rc_db);
    printf("    (zum Vergleich: ein 8-Bit-DAC erreicht bei diesem Pegel rechnerisch ca. 38 dB)\n");

    /* Amplitude stimmt: der Modulator liefert +-Vollaussteuerung = +-1, Soll 8000/32768 */
    CHECK(fabs(q64.amp - 8000.0 / 32768.0) < 0.01);
    CHECK(fabs(q32.amp - 8000.0 / 32768.0) < 0.01);
    CHECK(fabs(q64g.amp - 2.0 * q64.amp) < 0.01);       /* gain 2 = doppelte Amplitude */
    /* Mehr Bits je Sample muss messbar besser sein (1. Ordnung: ca. +9 dB je Verdopplung) */
    CHECK(q64.snr_ideal_db > q32.snr_ideal_db + 5.0);
    CHECK(q64.snr_ideal_db > 45.0);
    CHECK(q32.snr_ideal_db > 38.0);
}

int main(void)
{
    test_basics();
    test_pipe_counts();
    test_tone_quality();
    if (g_fail) { printf("%d Test(s) fehlgeschlagen\n", g_fail); return 1; }
    printf("Alle PDM-Tests bestanden.\n");
    return 0;
}
