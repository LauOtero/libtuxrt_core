/**
 * stepcompress.h — Compresión DDA de pulsos de motor (CORE)
 *
 * Convierte un segmento de movimiento (`tuxrt_move_t`, con perfil trapezoidal
 * ya finalizado) en una secuencia ordenada en el tiempo de eventos de paso
 * (`tuxrt_step_event_t`), un evento por pulso STEP. El algoritmo es un DDA
 * (Digital Differential Analyzer) sobre la fracción de recorrido del segmento:
 * el k-ésimo paso del eje `i` se produce cuando se ha recorrido la fracción
 * `k / n_i` del movimiento, y su instante se obtiene invirtiendo el perfil
 * trapezoidal (`tuxrt_move_time_for_distance`).
 *
 * La fusión entre ejes se hace con un min-heap de tamaño fijo (máx. ejes), de
 * modo que el stream de salida está globalmente ordenado por tiempo sin
 * asignaciones dinámicas.
 *
 * Design: TRT-13 - Step pipeline (P0) / F1c-03
 */

#ifndef TUXRT_STEPCOMPRESS_H
#define TUXRT_STEPCOMPRESS_H

#include <stdint.h>

#include "trapq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Evento de pulso STEP. */
typedef struct {
    double  time_s; /* instante absoluto (s)              */
    int8_t  dir;    /* +1 / -1 (signo del desplazamiento) */
    uint8_t axis;   /* eje que da el paso                 */
} tuxrt_step_event_t;

/* Consumidor de eventos de paso. */
typedef void (*tuxrt_step_sink_fn)(void* ctx, const tuxrt_step_event_t* ev);

/**
 * Crear un compresor DDA.
 * @param steps_per_mm pasos por mm de cada eje.
 * @param axis_count   número de ejes (1..TUXRT_TRAPQ_MAX_AXES).
 */
tuxrt_stepcompress_t* tuxrt_stepcompress_create(const double steps_per_mm[],
                                                uint8_t axis_count);

/** Liberar el compresor. */
void tuxrt_stepcompress_destroy(tuxrt_stepcompress_t* sc);

/**
 * Comprimir un segmento y emitir sus pulsos a `sink`.
 * @return TUXRT_OK, o TUXRT_ERR_INVAL si los argumentos son nulos.
 */
tuxrt_err_t tuxrt_stepcompress_feed(tuxrt_stepcompress_t* sc,
                                    const tuxrt_move_t* mv,
                                    tuxrt_step_sink_fn sink, void* ctx);

/** Total de pulsos emitidos por este compresor (estadística). */
uint64_t tuxrt_stepcompress_total_steps(const tuxrt_stepcompress_t* sc);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_STEPCOMPRESS_H */
