/**
 * input_shaper.c — Shaping de entrada (CORE)
 *
 * Calcula los impulsos (amplitud y retardo) de los shapers ZV/EI/ZVD/MZV a
 * partir de la frecuencia de resonancia y el amortiguamiento, y aplica la
 * convolución discreta de una señal con dichos impulsos.
 *
 * Fórmulas idénticas a Kalico/Klipper (`klippy/extras/shaper_defs.py`).
 * Las amplitudes se normalizan para que sumen 1 (ganancia unitaria).
 *
 * Design: TRT-13 - Step pipeline (P0) / F1c-05
 */

#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "input_shaper.h"

#include <math.h>
#include <stdlib.h>

/* Tolerancia de vibración del shaper EI (1/20 → 0.05). */
#define EI_V_TOL 0.05

struct tuxrt_input_shaper_t {
    tuxrt_shaper_coeffs_t c;
};

/* =============================================================================
 * Cálculo de coeficientes
 * ============================================================================= */

static void normalize(tuxrt_shaper_coeffs_t* c)
{
    double sum = 0.0;
    for (uint8_t i = 0; i < c->taps; i++) {
        sum += c->amplitude[i];
    }
    if (sum > 0.0) {
        for (uint8_t i = 0; i < c->taps; i++) {
            c->amplitude[i] /= sum;
        }
    }
}

static void shape_none(tuxrt_shaper_coeffs_t* c)
{
    c->taps = 1;
    c->amplitude[0] = 1.0;
    c->delay_s[0]   = 0.0;
}

static void shape_zv(tuxrt_shaper_coeffs_t* c, double f, double zeta)
{
    double df = sqrt(1.0 - zeta * zeta);
    double K  = exp(-zeta * M_PI / df);
    double td = 1.0 / (f * df);

    c->taps = 2;
    c->amplitude[0] = 1.0;
    c->amplitude[1] = K;
    c->delay_s[0]   = 0.0;
    c->delay_s[1]   = 0.5 * td;
}

static void shape_zvd(tuxrt_shaper_coeffs_t* c, double f, double zeta)
{
    double df = sqrt(1.0 - zeta * zeta);
    double K  = exp(-zeta * M_PI / df);
    double td = 1.0 / (f * df);

    c->taps = 3;
    c->amplitude[0] = 1.0;
    c->amplitude[1] = 2.0 * K;
    c->amplitude[2] = K * K;
    c->delay_s[0]   = 0.0;
    c->delay_s[1]   = 0.5 * td;
    c->delay_s[2]   = td;
}

static void shape_mzv(tuxrt_shaper_coeffs_t* c, double f, double zeta)
{
    double df = sqrt(1.0 - zeta * zeta);
    double K  = exp(-0.75 * zeta * M_PI / df);
    double td = 1.0 / (f * df);

    double a1 = 1.0 - 1.0 / sqrt(2.0);
    double a2 = (sqrt(2.0) - 1.0) * K;
    double a3 = a1 * K * K;

    c->taps = 3;
    c->amplitude[0] = a1;
    c->amplitude[1] = a2;
    c->amplitude[2] = a3;
    c->delay_s[0]   = 0.0;
    c->delay_s[1]   = 0.375 * td;
    c->delay_s[2]   = 0.75 * td;
}

static void shape_ei(tuxrt_shaper_coeffs_t* c, double f, double zeta)
{
    double df = sqrt(1.0 - zeta * zeta);
    double K  = exp(-zeta * M_PI / df);
    double td = 1.0 / (f * df);

    double a1 = 0.25 * (1.0 + EI_V_TOL);
    double a2 = 0.5 * (1.0 - EI_V_TOL) * K;
    double a3 = a1 * K * K;

    c->taps = 3;
    c->amplitude[0] = a1;
    c->amplitude[1] = a2;
    c->amplitude[2] = a3;
    c->delay_s[0]   = 0.0;
    c->delay_s[1]   = 0.5 * td;
    c->delay_s[2]   = td;
}

/* =============================================================================
 * Ciclo de vida
 * ============================================================================= */

tuxrt_err_t tuxrt_input_shaper_create(tuxrt_shaper_type_e type, double freq_hz,
                                      double damping, tuxrt_input_shaper_t** out)
{
    if (out == NULL) {
        return TUXRT_ERR_INVAL;
    }
    *out = NULL;

    if (type != TUXRT_SHAPER_NONE) {
        if (!(freq_hz > 0.0) || !(damping > 0.0) || !(damping < 1.0)) {
            return TUXRT_ERR_INVAL;
        }
    }

    tuxrt_input_shaper_t* s = calloc(1u, sizeof(*s));
    if (s == NULL) {
        return TUXRT_ERR_NOMEM;
    }
    s->c.type    = type;
    s->c.freq_hz = freq_hz;
    s->c.damping = damping;

    switch (type) {
    case TUXRT_SHAPER_NONE: shape_none(&s->c); break;
    case TUXRT_SHAPER_ZV:   shape_zv(&s->c, freq_hz, damping); break;
    case TUXRT_SHAPER_EI:   shape_ei(&s->c, freq_hz, damping); break;
    case TUXRT_SHAPER_ZVD:  shape_zvd(&s->c, freq_hz, damping); break;
    case TUXRT_SHAPER_MZV:  shape_mzv(&s->c, freq_hz, damping); break;
    default:
        free(s);
        return TUXRT_ERR_INVAL;
    }
    normalize(&s->c);

    *out = s;
    return TUXRT_OK;
}

void tuxrt_input_shaper_destroy(tuxrt_input_shaper_t* s)
{
    free(s);
}

const tuxrt_shaper_coeffs_t* tuxrt_input_shaper_coeffs(
    const tuxrt_input_shaper_t* s)
{
    return (s != NULL) ? &s->c : NULL;
}

size_t tuxrt_input_shaper_taps(const tuxrt_input_shaper_t* s)
{
    return (s != NULL) ? (size_t)s->c.taps : 0u;
}

/* =============================================================================
 * Convolución discreta
 * ============================================================================= */

tuxrt_err_t tuxrt_input_shaper_apply(const tuxrt_input_shaper_t* s,
                                     const double* in, size_t n,
                                     double* out, size_t* out_n)
{
    if (s == NULL || in == NULL || out == NULL || n == 0u) {
        return TUXRT_ERR_INVAL;
    }
    size_t taps = (size_t)s->c.taps;
    size_t on   = n + taps - 1u;

    for (size_t i = 0; i < on; i++) {
        double acc = 0.0;
        for (size_t j = 0; j < taps; j++) {
            if (i >= j && (i - j) < n) {
                acc += s->c.amplitude[j] * in[i - j];
            }
        }
        out[i] = acc;
    }
    if (out_n != NULL) {
        *out_n = on;
    }
    return TUXRT_OK;
}
