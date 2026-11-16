/**
 * trapq.c — Cola de trapezoides + lookahead de unión (CORE)
 *
 * Implementa el buffer circular de segmentos de movimiento con perfil
 * trapezoidal y el cálculo de velocidades de unión (junction deviation) que
 * hace de *lookahead*: garantiza que la velocidad en cada esquina entre dos
 * segmentos consecutivos es alcanzable con la aceleración disponible y no
 * excede el límite geométrico impuesto por `junction_dev`.
 *
 * Design: TRT-12 - Motion planner (P0) / F1c-02
 */

#define TUXRT_CORE_EXPORTS
#include "trapq.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Estructura interna
 * ============================================================================= */

struct tuxrt_trapq_t {
    tuxrt_move_t* moves;
    uint32_t      capacity;
    uint32_t      head;   /* índice del primer segmento válido   */
    uint32_t      count;  /* segmentos encolados                 */
    double        start_time;                       /* s, inicio del primer segmento    */
    double        last_pos[TUXRT_TRAPQ_MAX_AXES];   /* posición de la cabeza (entrada)  */
    uint8_t       last_axes;
};

/* =============================================================================
 * Primitivas de perfil
 * ============================================================================= */

double tuxrt_move_length(const tuxrt_move_t* m)
{
    if (m == NULL) {
        return 0.0;
    }
    double acc = 0.0;
    for (uint8_t i = 0; i < m->axis_count && i < TUXRT_TRAPQ_MAX_AXES; i++) {
        double d = m->end_pos[i] - m->start_pos[i];
        acc += d * d;
    }
    return sqrt(acc);
}

/* Descompone el perfil en fases: aceleración (t_a,s_a), crucero (t_c,s_c),
 * deceleración (t_d,s_d). `m->cruise_v` es la velocidad de crucero efectiva. */
static void profile_phases(const tuxrt_move_t* m, double L,
                           double* t_a, double* s_a, double* t_c, double* s_c,
                           double* t_d, double* s_d)
{
    double a  = m->accel;
    double v0 = m->start_v;
    double vc = m->cruise_v;
    double v1 = m->end_v;

    *t_a = *s_a = *t_c = *s_c = *t_d = *s_d = 0.0;
    if (L <= 0.0) {
        return;
    }

    if (a <= 0.0) { /* Sin aceleración: velocidad constante (o nula). */
        *t_c = (vc > 0.0) ? L / vc : 0.0;
        *s_c = L;
        return;
    }

    if (vc < v0) vc = v0;
    if (vc < v1) vc = v1;

    double ta = (vc - v0) / a;
    double td = (vc - v1) / a;
    if (ta < 0.0) ta = 0.0;
    if (td < 0.0) td = 0.0;

    double sa = (v0 + vc) * 0.5 * ta;
    double sd = (vc + v1) * 0.5 * td;

    if (sa + sd <= L) {
        *t_a = ta; *s_a = sa;
        *t_d = td; *s_d = sd;
        double sc = L - sa - sd;
        if (sc < 0.0) sc = 0.0;
        *s_c = sc;
        *t_c = (vc > 0.0) ? sc / vc : 0.0;
    } else {
        /* Perfil triangular: sin fase de crucero. */
        double vp = sqrt((2.0 * a * L + v0 * v0 + v1 * v1) * 0.5);
        ta = (vp - v0) / a;
        td = (vp - v1) / a;
        if (ta < 0.0) ta = 0.0;
        if (td < 0.0) td = 0.0;
        *t_a = ta; *s_a = (v0 + vp) * 0.5 * ta;
        *t_d = td; *s_d = (vp + v1) * 0.5 * td;
        *t_c = 0.0; *s_c = 0.0;
    }
}

/* Velocidad de crucero efectiva (pico real) para un segmento feasible. */
static double effective_cruise(double v0, double v1, double a, double L,
                               double commanded, double max_velocity)
{
    double vc = commanded < max_velocity ? commanded : max_velocity;
    if (a <= 0.0) {
        if (vc < v0) vc = v0;
        if (vc < v1) vc = v1;
        return vc;
    }
    double v_peak = sqrt((2.0 * a * L + v0 * v0 + v1 * v1) * 0.5);
    if (vc > v_peak) vc = v_peak;
    if (vc < v0) vc = v0;
    if (vc < v1) vc = v1;
    return vc;
}

void tuxrt_move_position_at(const tuxrt_move_t* m, double t_local, double out_pos[])
{
    if (m == NULL || out_pos == NULL) {
        return;
    }
    double L = tuxrt_move_length(m);
    double t_a, s_a, t_c, s_c, t_d, s_d;
    profile_phases(m, L, &t_a, &s_a, &t_c, &s_c, &t_d, &s_d);

    if (t_local < 0.0) t_local = 0.0;
    if (t_local > m->duration) t_local = m->duration;

    double s;
    if (t_local <= t_a) {
        s = m->start_v * t_local + 0.5 * m->accel * t_local * t_local;
    } else if (t_local <= t_a + t_c) {
        s = s_a + m->cruise_v * (t_local - t_a);
    } else {
        double td = t_local - t_a - t_c;
        s = s_a + s_c + m->cruise_v * td - 0.5 * m->accel * td * td;
    }

    double p = (L > 0.0) ? s / L : 0.0;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;

    for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
        out_pos[i] = m->start_pos[i] + p * (m->end_pos[i] - m->start_pos[i]);
    }
}

double tuxrt_move_time_for_distance(const tuxrt_move_t* m, double s)
{
    if (m == NULL) {
        return 0.0;
    }
    double L = tuxrt_move_length(m);
    if (s <= 0.0 || L <= 0.0) {
        return 0.0;
    }
    if (s >= L) {
        return m->duration;
    }

    double t_a, s_a, t_c, s_c, t_d, s_d;
    profile_phases(m, L, &t_a, &s_a, &t_c, &s_c, &t_d, &s_d);
    (void)t_d; (void)s_d;

    double a = m->accel;
    if (s <= s_a) {
        if (a > 0.0) {
            return (sqrt(m->start_v * m->start_v + 2.0 * a * s) - m->start_v) / a;
        }
        return (m->start_v > 0.0) ? s / m->start_v : 0.0;
    }
    if (s <= s_a + s_c) {
        return t_a + ((m->cruise_v > 0.0) ? (s - s_a) / m->cruise_v : 0.0);
    }
    double s2 = s - s_a - s_c;
    if (a > 0.0) {
        double disc = m->cruise_v * m->cruise_v - 2.0 * a * s2;
        if (disc < 0.0) disc = 0.0;
        return t_a + t_c + (m->cruise_v - sqrt(disc)) / a;
    }
    return t_a + t_c + ((m->cruise_v > 0.0) ? s2 / m->cruise_v : 0.0);
}

/* =============================================================================
 * Ciclo de vida
 * ============================================================================= */

tuxrt_trapq_t* tuxrt_trapq_create(uint32_t capacity)
{
    if (capacity == 0u) {
        return NULL;
    }
    tuxrt_trapq_t* q = calloc(1u, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->moves = calloc(capacity, sizeof(tuxrt_move_t));
    if (q->moves == NULL) {
        free(q);
        return NULL;
    }
    q->capacity   = capacity;
    q->head       = 0u;
    q->count      = 0u;
    q->start_time = 0.0;
    q->last_axes  = 0u;
    return q;
}

void tuxrt_trapq_destroy(tuxrt_trapq_t* q)
{
    if (q == NULL) {
        return;
    }
    free(q->moves);
    free(q);
}

/* =============================================================================
 * Inserción / consulta
 * ============================================================================= */

tuxrt_err_t tuxrt_trapq_append(tuxrt_trapq_t* q, const double end_pos[],
                               uint8_t axis_count, double cruise_v, double accel)
{
    if (q == NULL || end_pos == NULL) {
        return TUXRT_ERR_INVAL;
    }
    if (axis_count == 0u || axis_count > TUXRT_TRAPQ_MAX_AXES) {
        return TUXRT_ERR_INVAL;
    }
    if (q->count >= q->capacity) {
        return TUXRT_ERR_MOTION_QUEUE_FULL;
    }

    uint32_t idx = (q->head + q->count) % q->capacity;
    tuxrt_move_t* m = &q->moves[idx];
    memset(m, 0, sizeof(*m));

    for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
        m->start_pos[i] = q->last_pos[i];
        m->end_pos[i]   = (i < axis_count) ? end_pos[i] : q->last_pos[i];
    }
    m->axis_count = axis_count;
    m->cruise_v   = (cruise_v > 0.0) ? cruise_v : 0.0;
    m->accel      = (accel > 0.0) ? accel : 0.0;

    q->count++;
    for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
        q->last_pos[i] = m->end_pos[i];
    }
    q->last_axes = axis_count;
    return TUXRT_OK;
}

size_t tuxrt_trapq_count(const tuxrt_trapq_t* q)
{
    return (q != NULL) ? (size_t)q->count : 0u;
}

const tuxrt_move_t* tuxrt_trapq_at(const tuxrt_trapq_t* q, size_t idx)
{
    if (q == NULL || idx >= q->count) {
        return NULL;
    }
    return &q->moves[(q->head + (uint32_t)idx) % q->capacity];
}

double tuxrt_trapq_start_time(const tuxrt_trapq_t* q)
{
    return (q != NULL) ? q->start_time : 0.0;
}

double tuxrt_trapq_end_time(const tuxrt_trapq_t* q)
{
    if (q == NULL || q->count == 0u) {
        return (q != NULL) ? q->start_time : 0.0;
    }
    const tuxrt_move_t* last =
        &q->moves[(q->head + q->count - 1u) % q->capacity];
    return last->start_time + last->duration;
}

/* =============================================================================
 * Lookahead de unión (junction deviation)
 * ============================================================================= */

static void move_unit_dir(const tuxrt_move_t* m, double L, double u[])
{
    if (L <= 0.0) {
        for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
            u[i] = 0.0;
        }
        return;
    }
    for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
        u[i] = (m->end_pos[i] - m->start_pos[i]) / L;
    }
}

void tuxrt_trapq_finalize(tuxrt_trapq_t* q, double junction_dev,
                          double max_accel, double max_velocity)
{
    if (q == NULL || q->count == 0u) {
        return;
    }
    if (junction_dev < 0.0) junction_dev = 0.0;
    if (max_accel <= 0.0)     max_accel = 1.0;
    if (max_velocity <= 0.0)  max_velocity = 1.0;

    uint32_t n = q->count;

    /* Longitudes y velocidades de unión iniciales vj[0..n]. */
    double* vj = calloc((size_t)n + 1u, sizeof(double));
    if (vj == NULL) {
        return; /* sin memoria: dejar el perfil como esté */
    }
    vj[0] = 0.0;
    vj[n] = 0.0;

    double u_prev[TUXRT_TRAPQ_MAX_AXES];
    double L_prev = 0.0;
    const tuxrt_move_t* mprev = &q->moves[q->head];
    L_prev = tuxrt_move_length(mprev);
    move_unit_dir(mprev, L_prev, u_prev);

    for (uint32_t i = 1u; i < n; i++) {
        const tuxrt_move_t* m =
            &q->moves[(q->head + i) % q->capacity];
        double L = tuxrt_move_length(m);
        double u[TUXRT_TRAPQ_MAX_AXES];
        move_unit_dir(m, L, u);

        double cos_t = 0.0;
        for (uint8_t k = 0; k < TUXRT_TRAPQ_MAX_AXES; k++) {
            cos_t += u_prev[k] * u[k];
        }
        if (cos_t > 1.0) cos_t = 1.0;
        if (cos_t < -1.0) cos_t = -1.0;
        double sin_half = sqrt((1.0 - cos_t) * 0.5);

        double vjunc;
        if (sin_half <= 1e-9) {
            vjunc = max_velocity; /* colineal: sin penalización de esquina */
        } else if (sin_half >= 1.0) {
            vjunc = 0.0; /* reversa total (180°): parada */
        } else {
            double num = max_accel * junction_dev * sin_half;
            double den = 1.0 - sin_half;
            vjunc = (den > 1e-12) ? sqrt(num / den) : max_velocity;
        }
        if (vjunc > max_velocity) vjunc = max_velocity;
        vj[i] = vjunc;

        memcpy(u_prev, u, sizeof(u_prev));
        L_prev = L;
    }
    (void)L_prev;

    /* Pasada hacia delante: no acelerar más de lo posible. */
    for (uint32_t i = 0u; i < n; i++) {
        const tuxrt_move_t* m =
            &q->moves[(q->head + i) % q->capacity];
        double L = tuxrt_move_length(m);
        double a = (m->accel > 0.0) ? m->accel : max_accel;
        double vmax = sqrt(vj[i] * vj[i] + 2.0 * a * L);
        if (vj[i + 1u] > vmax) vj[i + 1u] = vmax;
    }
    /* Pasada hacia atrás: poder frenar hasta la velocidad de salida. */
    for (uint32_t i = n; i-- > 0u; ) {
        const tuxrt_move_t* m =
            &q->moves[(q->head + i) % q->capacity];
        double L = tuxrt_move_length(m);
        double a = (m->accel > 0.0) ? m->accel : max_accel;
        double vmax = sqrt(vj[i + 1u] * vj[i + 1u] + 2.0 * a * L);
        if (vj[i] > vmax) vj[i] = vmax;
    }
    /* Segunda pasada hacia delante para asentar entradas. */
    for (uint32_t i = 0u; i < n; i++) {
        const tuxrt_move_t* m =
            &q->moves[(q->head + i) % q->capacity];
        double L = tuxrt_move_length(m);
        double a = (m->accel > 0.0) ? m->accel : max_accel;
        double vmax = sqrt(vj[i] * vj[i] + 2.0 * a * L);
        if (vj[i + 1u] > vmax) vj[i + 1u] = vmax;
    }

    /* Asignar perfiles y tiempos. */
    double t = q->start_time;
    for (uint32_t i = 0u; i < n; i++) {
        tuxrt_move_t* m = &q->moves[(q->head + i) % q->capacity];
        double L = tuxrt_move_length(m);
        double a = (m->accel > 0.0) ? m->accel : max_accel;

        m->start_v = vj[i];
        m->end_v   = vj[i + 1u];
        m->cruise_v = effective_cruise(m->start_v, m->end_v, m->accel, L,
                                       m->cruise_v, max_velocity);
        m->accel = a; /* si no se especificó, usar max_accel */

        double t_a, s_a, t_c, s_c, t_d, s_d;
        profile_phases(m, L, &t_a, &s_a, &t_c, &s_c, &t_d, &s_d);
        m->duration   = t_a + t_c + t_d;
        m->start_time = t;
        t += m->duration;
    }

    free(vj);
}

/* =============================================================================
 * Consulta temporal
 * ============================================================================= */

tuxrt_err_t tuxrt_trapq_get_position(const tuxrt_trapq_t* q, double t,
                                     double out_pos[], uint8_t* out_axes)
{
    if (q == NULL || out_pos == NULL) {
        return TUXRT_ERR_INVAL;
    }

    if (q->count == 0u) {
        for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
            out_pos[i] = q->last_pos[i];
        }
        if (out_axes != NULL) *out_axes = q->last_axes;
        return TUXRT_OK;
    }

    const tuxrt_move_t* first = &q->moves[q->head];
    if (t <= first->start_time) {
        for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
            out_pos[i] = first->start_pos[i];
        }
        if (out_axes != NULL) *out_axes = first->axis_count;
        return TUXRT_OK;
    }

    for (uint32_t i = 0u; i < q->count; i++) {
        const tuxrt_move_t* m = &q->moves[(q->head + i) % q->capacity];
        double end = m->start_time + m->duration;
        if (t <= end) {
            tuxrt_move_position_at(m, t - m->start_time, out_pos);
            if (out_axes != NULL) *out_axes = m->axis_count;
            return TUXRT_OK;
        }
    }

    /* Más allá del final: devolver el último punto. */
    const tuxrt_move_t* last = &q->moves[(q->head + q->count - 1u) % q->capacity];
    memcpy(out_pos, last->end_pos, sizeof(last->end_pos));
    if (out_axes != NULL) *out_axes = last->axis_count;
    return TUXRT_OK;
}

size_t tuxrt_trapq_pop_until(tuxrt_trapq_t* q, double t, double out_last_pos[])
{
    if (q == NULL) {
        return 0u;
    }
    size_t popped = 0u;
    while (q->count > 0u) {
        const tuxrt_move_t* m = &q->moves[q->head];
        if (m->start_time + m->duration > t) {
            break;
        }
        if (out_last_pos != NULL) {
            memcpy(out_last_pos, m->end_pos, sizeof(m->end_pos));
        }
        q->last_axes = m->axis_count;
        q->start_time = m->start_time + m->duration;
        q->head = (q->head + 1u) % q->capacity;
        q->count--;
        popped++;
    }
    return popped;
}
