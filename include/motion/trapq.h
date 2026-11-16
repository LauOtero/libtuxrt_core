/**
 * trapq.h — Cola de trapezoides (planificación de movimiento CORE)
 *
 * Buffer circular de segmentos de movimiento (`tuxrt_move_t`) con perfil de
 * velocidad trapezoidal (aceleración / crucero / deceleración). La función
 * `tuxrt_trapq_finalize()` aplica el *lookahead* de unión: recalcula la
 * velocidad de entrada/salida de cada segmento para que la velocidad de
 * esquina entre dos segmentos consecutivos nunca exceda la permitida por la
 * `junction_deviation` (modelo estándar de Klipper), respetando los límites de
 * aceleración y velocidad.
 *
 * El perfil es determinista y se puede consultar en cualquier instante con
 * `tuxrt_trapq_get_position()` / `tuxrt_move_position_at()`.
 *
 * Design: TRT-12 - Motion planner (P0) / F1c-02
 */

#ifndef TUXRT_TRAPQ_H
#define TUXRT_TRAPQ_H

#include <stddef.h>
#include <stdint.h>

#include "tuxrt_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Máximo de ejes por movimiento (coincide con tuxrt_joint_state_t). */
#define TUXRT_TRAPQ_MAX_AXES 12

/* Segmento de movimiento con perfil trapezoidal (espacio de articulaciones). */
typedef struct {
    double  start_pos[TUXRT_TRAPQ_MAX_AXES]; /* mm */
    double  end_pos[TUXRT_TRAPQ_MAX_AXES];   /* mm */
    double  start_v;    /* mm/s  velocidad al inicio del segmento          */
    double  cruise_v;   /* mm/s  velocidad de crucero efectiva             */
    double  end_v;      /* mm/s  velocidad al final del segmento           */
    double  accel;      /* mm/s^2 aceleración (>= 0)                       */
    double  duration;   /* s     duración del segmento                     */
    double  start_time; /* s     instante absoluto de inicio (finalize)    */
    uint8_t axis_count; /* número de ejes activos                          */
} tuxrt_move_t;

/* --- Ciclo de vida -------------------------------------------------------- */

/** Crear una cola con capacidad `capacity` movimientos (potencia de 2 no exigida). */
tuxrt_trapq_t* tuxrt_trapq_create(uint32_t capacity);

/** Liberar la cola. */
void tuxrt_trapq_destroy(tuxrt_trapq_t* q);

/* --- Inserción / consulta ------------------------------------------------- */

/**
 * Añadir un segmento desde la posición actual (final del último segmento, o
 * el origen si la cola está vacía) hasta `end_pos`.
 *
 * @param end_pos   posición final (mm) de los `axis_count` primeros ejes.
 * @param axis_count número de ejes activos (1..TUXRT_TRAPQ_MAX_AXES).
 * @param cruise_v  velocidad de crucero deseada (mm/s).
 * @param accel     aceleración (mm/s^2).
 * @return TUXRT_OK o TUXRT_ERR_INVAL / TUXRT_ERR_NOMEM.
 */
tuxrt_err_t tuxrt_trapq_append(tuxrt_trapq_t* q, const double end_pos[],
                               uint8_t axis_count, double cruise_v, double accel);

/** Número de segmentos encolados. */
size_t tuxrt_trapq_count(const tuxrt_trapq_t* q);

/** Tiempo de inicio del primer segmento encolado (s). */
double tuxrt_trapq_start_time(const tuxrt_trapq_t* q);

/** Tiempo de fin del último segmento encolado (s). */
double tuxrt_trapq_end_time(const tuxrt_trapq_t* q);

/** Acceso al segmento `idx` (0 = cabeza). NULL si fuera de rango. */
const tuxrt_move_t* tuxrt_trapq_at(const tuxrt_trapq_t* q, size_t idx);

/**
 * Aplicar el lookahead de unión y calcular perfiles/tiempos de todos los
 * segmentos encolados. Debe llamarse tras una ráfaga de `append()` y antes de
 * consultar posiciones o comprimir pulsos.
 */
void tuxrt_trapq_finalize(tuxrt_trapq_t* q, double junction_dev,
                          double max_accel, double max_velocity);

/* --- Consulta temporal ---------------------------------------------------- */

/**
 * Posición (mm) en el instante absoluto `t`. Se satura al rango de la cola.
 *
 * @param out_pos  buffer de al menos TUXRT_TRAPQ_MAX_AXES dobles.
 * @param out_axes (opcional) número de ejes del segmento consultado.
 */
tuxrt_err_t tuxrt_trapq_get_position(const tuxrt_trapq_t* q, double t,
                                     double out_pos[], uint8_t* out_axes);

/**
 * Descartar los segmentos cuyo `start_time + duration <= t`.
 * @param out_last_pos (opcional) posición final del último segmento descartado.
 * @return número de segmentos descartados.
 */
size_t tuxrt_trapq_pop_until(tuxrt_trapq_t* q, double t, double out_last_pos[]);

/* --- Primitivas de perfil (compartidas con stepcompress) ------------------ */

/** Longitud euclídea del segmento (mm). */
double tuxrt_move_length(const tuxrt_move_t* m);

/** Posición (mm) dentro del segmento en el tiempo local `t_local` (0..duration). */
void tuxrt_move_position_at(const tuxrt_move_t* m, double t_local,
                            double out_pos[]);

/**
 * Tiempo local (s) necesario para recorrer `s` mm sobre el segmento, invirtiendo
 * el perfil trapezoidal. `s` se satura a [0, length].
 */
double tuxrt_move_time_for_distance(const tuxrt_move_t* m, double s);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_TRAPQ_H */
