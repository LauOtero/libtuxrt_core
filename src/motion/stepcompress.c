/**
 * stepcompress.c — Compresión DDA de pulsos de motor (CORE)
 *
 * Convierte un segmento de movimiento (`tuxrt_move_t`) con perfil trapezoidal
 * ya finalizado en una secuencia de eventos de paso ordenada por tiempo.
 *
 * Algoritmo (DDA sobre la fracción de recorrido):
 *   - El eje `i` recorre `delta_i = end_pos[i] - start_pos[i]` mm en todo el
 *     segmento, cuya longitud de trayectoria es `L = tuxrt_move_length(mv)`.
 *   - El número de pulsos es `n_i = round(|delta_i| * steps_per_mm[i])`.
 *   - El k-ésimo pulso (k = 1..n_i) se emite cuando la trayectoria ha avanzado
 *     la fracción `k / n_i`, es decir a la distancia `s = (k / n_i) * L`; su
 *     instante es `mv->start_time + tuxrt_move_time_for_distance(mv, s)`.
 *
 * La fusión entre ejes se hace con un min-heap de tamaño fijo (≤ 12), de modo
 * que la salida queda globalmente ordenada por tiempo sin asignaciones
 * dinámicas en el camino caliente.
 *
 * Design: TRT-13 - Step pipeline (P0) / F1c-03
 */

#define TUXRT_CORE_EXPORTS
#include "stepcompress.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Estructura interna
 * ============================================================================= */

struct tuxrt_stepcompress_t {
    double   steps_per_mm[TUXRT_TRAPQ_MAX_AXES];
    uint8_t  axis_count;
    uint64_t total_steps;
};

/* Cursor de emisión de un eje dentro del segmento actual. */
typedef struct {
    uint8_t  axis;
    int8_t   dir;
    uint32_t n;        /* pulsos totales del eje en el segmento   */
    uint32_t k;        /* próximo pulso a emitir (1..n)           */
    double   L;        /* longitud del segmento (mm)              */
    double   t0;       /* instante absoluto de inicio (s)         */
    double   time;     /* instante del próximo pulso (s)          */
} axis_cursor_t;

/* =============================================================================
 * Min-heap de cursores (clave: time)
 * ============================================================================= */

static void heap_swap(axis_cursor_t* h, uint32_t a, uint32_t b)
{
    axis_cursor_t tmp = h[a];
    h[a] = h[b];
    h[b] = tmp;
}

static void heap_sift_down(axis_cursor_t* h, uint32_t n, uint32_t i)
{
    for (;;) {
        uint32_t l = 2u * i + 1u;
        uint32_t r = 2u * i + 2u;
        uint32_t smallest = i;
        if (l < n && h[l].time < h[smallest].time) smallest = l;
        if (r < n && h[r].time < h[smallest].time) smallest = r;
        if (smallest == i) break;
        heap_swap(h, i, smallest);
        i = smallest;
    }
}

/* =============================================================================
 * Ciclo de vida
 * ============================================================================= */

tuxrt_stepcompress_t* tuxrt_stepcompress_create(const double steps_per_mm[],
                                                uint8_t axis_count)
{
    if (steps_per_mm == NULL || axis_count == 0u ||
        axis_count > TUXRT_TRAPQ_MAX_AXES) {
        return NULL;
    }
    tuxrt_stepcompress_t* sc = calloc(1u, sizeof(*sc));
    if (sc == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < axis_count; i++) {
        sc->steps_per_mm[i] = steps_per_mm[i];
    }
    sc->axis_count  = axis_count;
    sc->total_steps = 0u;
    return sc;
}

void tuxrt_stepcompress_destroy(tuxrt_stepcompress_t* sc)
{
    free(sc);
}

/* =============================================================================
 * Compresión DDA
 * ============================================================================= */

tuxrt_err_t tuxrt_stepcompress_feed(tuxrt_stepcompress_t* sc,
                                    const tuxrt_move_t* mv,
                                    tuxrt_step_sink_fn sink, void* ctx)
{
    if (sc == NULL || mv == NULL || sink == NULL) {
        return TUXRT_ERR_INVAL;
    }

    double L = tuxrt_move_length(mv);
    if (L <= 0.0) {
        return TUXRT_OK;   /* movimiento nulo: no emite pulsos */
    }

    axis_cursor_t heap[TUXRT_TRAPQ_MAX_AXES];
    uint32_t heap_n = 0u;

    uint8_t axes = mv->axis_count;
    if (axes > TUXRT_TRAPQ_MAX_AXES) axes = TUXRT_TRAPQ_MAX_AXES;
    if (axes > sc->axis_count)        axes = sc->axis_count;

    for (uint8_t i = 0; i < axes; i++) {
        double delta = mv->end_pos[i] - mv->start_pos[i];
        double spm   = sc->steps_per_mm[i];
        if (spm <= 0.0) {
            continue;
        }
        double n_real = fabs(delta) * spm;
        uint32_t n = (uint32_t)(n_real + 0.5);
        if (n == 0u) {
            continue;
        }
        axis_cursor_t* c = &heap[heap_n++];
        c->axis = i;
        c->dir  = (delta >= 0.0) ? (int8_t)1 : (int8_t)-1;
        c->n    = n;
        c->k    = 1u;
        c->L    = L;
        c->t0   = mv->start_time;
        /* El primer pulso se emite a la fracción 1/n del recorrido. */
        double s = (double)c->k / (double)n * L;
        c->time = c->t0 + tuxrt_move_time_for_distance(mv, s);
    }

    /* Construir el heap. */
    for (uint32_t i = heap_n / 2u; i-- > 0u; ) {
        heap_sift_down(heap, heap_n, i);
    }

    /* Extraer eventos en orden temporal. */
    uint32_t active = heap_n;
    while (active > 0u) {
        axis_cursor_t c = heap[0];

        tuxrt_step_event_t ev;
        ev.time_s = c.time;
        ev.dir    = c.dir;
        ev.axis   = c.axis;
        sink(ctx, &ev);
        sc->total_steps++;

        /* Avanzar el cursor y recalcular su próximo instante. */
        if (c.k >= c.n) {
            /* Reemplazar la raíz por el último y reducir el heap. */
            heap[0] = heap[active - 1u];
            active--;
            if (active > 0u) {
                heap_sift_down(heap, active, 0u);
            }
        } else {
            c.k++;
            double s = (double)c.k / (double)c.n * c.L;
            c.time = c.t0 + tuxrt_move_time_for_distance(mv, s);
            heap[0] = c;
            heap_sift_down(heap, active, 0u);
        }
    }

    return TUXRT_OK;
}

uint64_t tuxrt_stepcompress_total_steps(const tuxrt_stepcompress_t* sc)
{
    return (sc != NULL) ? sc->total_steps : 0u;
}
