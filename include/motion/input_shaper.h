/**
 * input_shaper.h — Shaping de entrada (CORE)
 *
 * Reduce las vibraciones post-movimiento convolucionando la secuencia de
 * pulsos/impulsos con la respuesta impulsional de un shaper (ZV, EI, ZVD, MZV).
 *
 * Las amplitudes y retrasos por impulso se calculan a partir de la frecuencia
 * de resonancia (`freq_hz`) y el factor de amortiguación (`damping`), siguiendo
 * las mismas fórmulas que Kalico/Klipper (`shaper_defs.py`). Las amplitudes se
 * normalizan para que su suma sea 1 (ganancia unitaria en continua).
 *
 * Design: TRT-13 - Step pipeline (P0) / F1c-05
 */

#ifndef TUXRT_INPUT_SHAPER_H
#define TUXRT_INPUT_SHAPER_H

#include <stddef.h>
#include <stdint.h>

#include "tuxrt_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Máximo de impulsos de un shaper (ZV/EI/ZVD/MZV usan <= 3). */
#define TUXRT_SHAPER_MAX_TAPS 8

typedef enum {
    TUXRT_SHAPER_NONE = 0, /* identidad (sin shaping) */
    TUXRT_SHAPER_ZV   = 1, /* 2 impulsos */
    TUXRT_SHAPER_EI   = 2, /* 3 impulsos */
    TUXRT_SHAPER_ZVD  = 3, /* 3 impulsos */
    TUXRT_SHAPER_MZV  = 4  /* 3 impulsos */
} tuxrt_shaper_type_e;

/* Coeficientes calculados de un shaper. */
typedef struct {
    tuxrt_shaper_type_e type;
    uint8_t taps;                       /* nº de impulsos                */
    double  amplitude[TUXRT_SHAPER_MAX_TAPS]; /* amplitudes (suma = 1)   */
    double  delay_s[TUXRT_SHAPER_MAX_TAPS];   /* retardo de cada impulso (s) */
    double  freq_hz;                    /* frecuencia de resonancia      */
    double  damping;                    /* factor de amortiguación       */
} tuxrt_shaper_coeffs_t;

/* --- Ciclo de vida -------------------------------------------------------- */

/**
 * Crear un shaper.
 *
 * @param type      tipo de shaper (NONE → identidad).
 * @param freq_hz   frecuencia de resonancia (Hz); debe ser > 0 salvo NONE.
 * @param damping   factor de amortiguación ζ (0 < ζ < 1).
 * @param out       [out] shaper creado.
 * @return TUXRT_OK, o TUXRT_ERR_INVAL / TUXRT_ERR_NOMEM.
 */
tuxrt_err_t tuxrt_input_shaper_create(tuxrt_shaper_type_e type, double freq_hz,
                                      double damping, tuxrt_input_shaper_t** out);

/** Liberar el shaper. */
void tuxrt_input_shaper_destroy(tuxrt_input_shaper_t* s);

/* --- Consulta de coeficientes --------------------------------------------- */

/** Coeficientes calculados (NULL si `s` es NULL). */
const tuxrt_shaper_coeffs_t* tuxrt_input_shaper_coeffs(
    const tuxrt_input_shaper_t* s);

/** Número de impulsos del shaper (1 para NONE). */
size_t tuxrt_input_shaper_taps(const tuxrt_input_shaper_t* s);

/* --- Aplicación ----------------------------------------------------------- */

/**
 * Convolución discreta de la señal `in[0..n-1]` con los impulsos del shaper.
 * La salida tiene `n + taps - 1` muestras y se escribe en `out`, que debe tener
 * al menos esa capacidad.
 *
 * @param out_n [out] nº de muestras escritas en `out`.
 * @return TUXRT_OK, o TUXRT_ERR_INVAL si algún puntero es NULL o `n == 0`.
 */
tuxrt_err_t tuxrt_input_shaper_apply(const tuxrt_input_shaper_t* s,
                                     const double* in, size_t n,
                                     double* out, size_t* out_n);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_INPUT_SHAPER_H */
