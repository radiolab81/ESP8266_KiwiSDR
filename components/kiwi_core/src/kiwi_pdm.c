/**
 * @file kiwi_pdm.c
 * @brief Delta-Sigma-Modulator und Ausgabepfad (siehe kiwi_pdm.h).
 */
#include "kiwi_pdm.h"

void kiwi_pdm_init(kiwi_pdm_t *m)
{
    m->acc = 0;
}

void kiwi_pdm_encode(kiwi_pdm_t *m, int32_t value, uint16_t *out, unsigned nwords)
{
    if (value >  32767) value =  32767;       /* hart begrenzen statt ueberlaufen */
    if (value < -32768) value = -32768;
    /* Vorzeichenbehaftet (-32768..32767) -> vorzeichenlos (0..65535) = Pulsdichte u/65536.
     * Stille (0) ergibt 32768 = 50 % "1": die Ausgangsspannung liegt dann bei halber
     * Versorgung (ca. 1,65 V) - die Gleichspannung entfernt spaeter ein Koppelkondensator. */
    uint32_t u = (uint32_t)(value + 32768);
    uint32_t acc = m->acc;

    for (unsigned w = 0; w < nwords; w++) {
        uint32_t word = 0;
        for (unsigned b = 0; b < KIWI_PDM_WORD_BITS; b++) {
            acc += u;                          /* Akkumulator weiterzaehlen ...        */
            word = (word << 1) | (acc >> 16);  /* ... Ueberlauf (Bit 16) ist das Ausgabebit */
            acc &= 0xFFFFu;                    /* Rest bleibt erhalten = Fehlerrueckkopplung */
        }
        out[w] = (uint16_t)word;
    }
    m->acc = acc;
}

/* ------------------------------------------------------------------ */

unsigned kiwi_pdm_choose_up(uint32_t source_rate_hz)
{
    /* Ziel: Ausgaberate ~ 24 kHz. 12 kHz -> 2, 8 kHz -> 3, ab 24 kHz -> 1. */
    if (source_rate_hz == 0 || source_rate_hz >= 24000u) return 1;
    unsigned up = (24000u + source_rate_hz - 1u) / source_rate_hz;
    return up > 4 ? 4 : up;
}

void kiwi_pdm_pipe_init(kiwi_pdm_pipe_t *p, unsigned up, unsigned words_per_sample, int gain)
{
    kiwi_pdm_init(&p->mod);
    p->prev = 0;
    p->up = up < 1 ? 1 : (up > 4 ? 4 : up);
    p->words = words_per_sample < 1 ? 1 : words_per_sample;
    p->gain = gain < 1 ? 1 : (gain > 8 ? 8 : gain);
}

size_t kiwi_pdm_pipe_max_words(const kiwi_pdm_pipe_t *p, size_t n)
{
    return n * (p->up + 1u) * p->words;       /* +1: Taktkorrektur "extra" */
}

size_t kiwi_pdm_pipe_run(kiwi_pdm_pipe_t *p, const int16_t *in, size_t n, int extra,
                         uint16_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t x = in[i];
        int reps = (int)p->up;                 /* Ausgabesamples fuer dieses Eingangssample */
        if (i == 0) {
            reps += extra;                     /* Taktkorrektur nur am Anfang des Blocks */
            if (reps < 1) reps = 1;
        }
        for (int k = 0; k < reps; k++) {
            /* Lineare Interpolation von prev nach x in 'reps' gleichen Schritten. Beim
             * letzten Schritt (k = reps-1) ergibt sich genau x. Ganzzahlig, ohne float. */
            int32_t v = p->prev + ((x - p->prev) * (k + 1)) / reps;
            v *= p->gain;                      /* Lautstaerke (Begrenzung im Modulator) */
            kiwi_pdm_encode(&p->mod, v, out + o, p->words);
            o += p->words;
        }
        p->prev = x;
    }
    return o;
}
