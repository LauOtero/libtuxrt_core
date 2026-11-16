/**
 * steppersync.h — Sincronización multi-eje de pulsos (CORE)
 *
 * Fusiona los flujos de eventos de paso de varios motores (uno por
 * `stepper_id`) en un único stream globalmente ordenado por tiempo, y los
 * libera por tramos temporales (`flush` hasta `up_to_time`) para alimentar los
 * buffers de la MCU sin sobrepasar su capacidad.
 *
 * La fusión se hace mediante un *k-way merge* sobre las cabezas de las colas
 * por motor: como cada motor produce sus eventos en orden temporal creciente
 * (garantizado por `tuxrt_stepcompress_feed`), basta comparar la cabeza de cada
 * cola para obtener el siguiente evento global.
 *
 * Design: TRT-13 - Step pipeline (P0) / F1c-04
 */

#ifndef TUXRT_STEPPERSYNC_H
#define TUXRT_STEPPERSYNC_H

#include <stddef.h>
#include <stdint.h>

#include "tuxrt_sdk.h"
#include "stepcompress.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Evento sincronizado: evento de paso + identidad del motor. */
typedef struct {
    double  time_s;     /* instante absoluto (s)  */
    uint8_t stepper_id; /* motor que da el paso   */
    uint8_t axis;       /* eje lógico             */
    int8_t  dir;        /* +1 / -1                */
} tuxrt_sync_event_t;

/* Consumidor de eventos sincronizados. */
typedef void (*tuxrt_sync_sink_fn)(void* ctx, const tuxrt_sync_event_t* ev);

/* --- Ciclo de vida -------------------------------------------------------- */

/**
 * Crear un sincronizador para `stepper_count` motores (1..TUXRT_TRAPQ_MAX_AXES).
 */
tuxrt_steppersync_t* tuxrt_steppersync_create(uint8_t stepper_count);

/** Liberar el sincronizador. */
void tuxrt_steppersync_destroy(tuxrt_steppersync_t* ss);

/* --- Inserción / liberación ----------------------------------------------- */

/**
 * Encolar un evento para `stepper_id`. Los eventos de un mismo motor deben
 * llegar en orden temporal no decreciente; en caso contrario se devuelve
 * TUXRT_ERR_INVAL.
 *
 * @return TUXRT_OK, TUXRT_ERR_INVAL (id/args/orden) o TUXRT_ERR_NOMEM.
 */
tuxrt_err_t tuxrt_steppersync_push(tuxrt_steppersync_t* ss, uint8_t stepper_id,
                                   const tuxrt_step_event_t* ev);

/**
 * Emitir a `sink`, en orden temporal global, todos los eventos cuyo instante es
 * `<= up_to_time`. Los eventos emitidos se descartan de sus colas.
 *
 * @return número de eventos emitidos.
 */
size_t tuxrt_steppersync_flush(tuxrt_steppersync_t* ss, double up_to_time,
                               tuxrt_sync_sink_fn sink, void* ctx);

/** Eventos aún pendientes de emitir. */
size_t tuxrt_steppersync_pending(const tuxrt_steppersync_t* ss);

/** Total de eventos emitidos por este sincronizador (estadística). */
uint64_t tuxrt_steppersync_total_events(const tuxrt_steppersync_t* ss);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_STEPPERSYNC_H */
