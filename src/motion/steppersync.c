/**
 * steppersync.c — Sincronización multi-eje de pulsos (CORE)
 *
 * K-way merge sobre las cabezas de las colas por motor. Cada cola es un buffer
 * dinámico que crece por duplicación; los eventos consumidos se descartan
 * avanzando el índice de cabeza y compactando la cola cuando queda vacía.
 *
 * Design: TRT-13 - Step pipeline (P0) / F1c-04
 */

#define TUXRT_CORE_EXPORTS
#include "steppersync.h"

#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Estructura interna
 * ============================================================================= */

typedef struct {
    tuxrt_step_event_t* ev;    /* buffer circular-dinámico            */
    size_t              cap;   /* capacidad reservada                 */
    size_t              head;  /* índice del primer evento pendiente  */
    size_t              count; /* eventos pendientes                  */
    double              last_t;/* último instante encolado (orden)    */
} stepper_q_t;

struct tuxrt_steppersync_t {
    stepper_q_t* q;
    uint8_t      n;
    uint64_t     total;
};

/* =============================================================================
 * Ciclo de vida
 * ============================================================================= */

tuxrt_steppersync_t* tuxrt_steppersync_create(uint8_t stepper_count)
{
    if (stepper_count == 0u || stepper_count > TUXRT_TRAPQ_MAX_AXES) {
        return NULL;
    }
    tuxrt_steppersync_t* ss = calloc(1u, sizeof(*ss));
    if (ss == NULL) {
        return NULL;
    }
    ss->q = calloc(stepper_count, sizeof(*ss->q));
    if (ss->q == NULL) {
        free(ss);
        return NULL;
    }
    for (uint8_t i = 0; i < stepper_count; i++) {
        ss->q[i].last_t = -1.0; /* permite tiempos negativos iniciales */
    }
    ss->n     = stepper_count;
    ss->total = 0u;
    return ss;
}

void tuxrt_steppersync_destroy(tuxrt_steppersync_t* ss)
{
    if (ss == NULL) {
        return;
    }
    for (uint8_t i = 0; i < ss->n; i++) {
        free(ss->q[i].ev);
    }
    free(ss->q);
    free(ss);
}

/* =============================================================================
 * Inserción
 * ============================================================================= */

tuxrt_err_t tuxrt_steppersync_push(tuxrt_steppersync_t* ss, uint8_t stepper_id,
                                   const tuxrt_step_event_t* ev)
{
    if (ss == NULL || ev == NULL || stepper_id >= ss->n) {
        return TUXRT_ERR_INVAL;
    }
    stepper_q_t* q = &ss->q[stepper_id];
    if (q->count > 0u && ev->time_s < q->last_t) {
        return TUXRT_ERR_INVAL; /* orden temporal no decreciente */
    }

    if (q->count == q->cap) {
        size_t ncap = (q->cap == 0u) ? 64u : q->cap * 2u;
        tuxrt_step_event_t* nev =
            realloc(q->ev, ncap * sizeof(*nev));
        if (nev == NULL) {
            return TUXRT_ERR_NOMEM;
        }
        q->ev  = nev;
        q->cap = ncap;
    }
    q->ev[q->count++] = *ev;
    q->last_t = ev->time_s;
    return TUXRT_OK;
}

/* =============================================================================
 * Liberación ordenada
 * ============================================================================= */

/* Índice del motor cuya cabeza tiene el instante mínimo. -1 si no hay eventos. */
static int next_stepper(const tuxrt_steppersync_t* ss)
{
    int best = -1;
    double best_t = 0.0;
    for (uint8_t i = 0; i < ss->n; i++) {
        if (ss->q[i].count == 0u) {
            continue;
        }
        double t = ss->q[i].ev[ss->q[i].head].time_s;
        if (best < 0 || t < best_t) {
            best = (int)i;
            best_t = t;
        }
    }
    return best;
}

size_t tuxrt_steppersync_flush(tuxrt_steppersync_t* ss, double up_to_time,
                               tuxrt_sync_sink_fn sink, void* ctx)
{
    if (ss == NULL || sink == NULL) {
        return 0u;
    }
    size_t emitted = 0u;
    for (;;) {
        int i = next_stepper(ss);
        if (i < 0) {
            break;
        }
        stepper_q_t* q = &ss->q[i];
        const tuxrt_step_event_t* src = &q->ev[q->head];
        if (src->time_s > up_to_time) {
            break;
        }

        tuxrt_sync_event_t out;
        out.time_s     = src->time_s;
        out.stepper_id = (uint8_t)i;
        out.axis       = src->axis;
        out.dir        = src->dir;
        sink(ctx, &out);

        q->head++;
        q->count--;
        ss->total++;
        emitted++;

        if (q->count == 0u) {
            q->head  = 0u;
            q->last_t = -1.0;
        }
    }
    return emitted;
}

size_t tuxrt_steppersync_pending(const tuxrt_steppersync_t* ss)
{
    if (ss == NULL) {
        return 0u;
    }
    size_t total = 0u;
    for (uint8_t i = 0; i < ss->n; i++) {
        total += ss->q[i].count;
    }
    return total;
}

uint64_t tuxrt_steppersync_total_events(const tuxrt_steppersync_t* ss)
{
    return (ss != NULL) ? ss->total : 0u;
}
