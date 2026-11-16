/**
 * motion.c — Motion Planning (CORE)
 *
 * Planificador de movimiento con perfiles trapezoidales y lookahead de unión.
 * Integra el pipeline completo de generación de pulsos:
 *
 *   comando (pose) → cinemática inversa → trapq (perfil + junction deviation)
 *                  → stepcompress (DDA) → steppersync (orden global)
 *                  → hilo stepper (flush al reloj del pipeline)
 *
 * El hilo stepper consume la cola de trapezoides conforme avanza el reloj
 * monotónico, alimenta el compresor DDA de los segmentos pendientes y libera
 * los eventos ordenados hacia el consumidor de pulsos.
 *
 * Design: TRT-12/TRT-13 - Motion planner + Step pipeline (P0) / F1c-01
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include "trapq.h"
#include "stepcompress.h"
#include "steppersync.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

// =============================================================================
// Internal
// =============================================================================

#define MAX_QUEUE 256
#define DEFAULT_STEPS_PER_MM 80.0

struct tuxrt_motion_t {
    const tuxrt_config_t* cfg;
    const tuxrt_kinematics_t* kin;
    tuxrt_motion_config_t mcfg;

    /* Cola de comandos (producer/consumer externo) */
    tuxrt_motion_cmd_t queue[MAX_QUEUE];
    size_t q_head, q_tail, q_count;
    pthread_mutex_t q_mutex;

    /* Pipeline de pulsos */
    tuxrt_trapq_t* trapq;
    tuxrt_stepcompress_t* sc;
    tuxrt_steppersync_t* ss;
    double steps_per_mm[TUXRT_TRAPQ_MAX_AXES];
    size_t fed_total;    /* movimientos ya comprimidos                  */
    size_t popped_total; /* movimientos ya consumidos y descartados     */

    tuxrt_pose_t last_pose;   /* extremo del último movimiento planificado */
    int64_t t0_ns;            /* referencia del reloj del pipeline         */
    double clock_s;           /* último instante procesado (s)             */
    uint64_t steps_emitted;   /* pulsos liberados al consumidor (stats)    */

    /* Estado */
    tuxrt_joint_state_t current_joints;
    _Atomic int status;
    _Atomic bool running;
    pthread_t stepper_thread;
};

static double clamp(double v, double min, double max) {
    return v < min ? min : (v > max ? max : v);
}

static double monotonic_s(int64_t t0_ns) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ns = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
    return (double)(now_ns - t0_ns) * 1e-9;
}

/* Sink de pulsos: reenvía los eventos del DDA al sincronizador (síncrono fuera de mutex). */
static void feed_step(void* ctx, const tuxrt_step_event_t* ev) {
   struct tuxrt_motion_t* m = (struct tuxrt_motion_t*)ctx;
   tuxrt_steppersync_push(m->ss, ev->axis, ev);
}

/* Sink de liberación del sincronizador (transporte MCU: fase F1e). */
static void discard_sink(void* ctx, const tuxrt_sync_event_t* ev) {
    (void)ctx;
    (void)ev;
}

/* Crea el pipeline (trapq + stepcompress + steppersync) una sola vez. */
static tuxrt_err_t ensure_pipeline(struct tuxrt_motion_t* m) {
    if (m->trapq != NULL) {
        return TUXRT_OK;
    }
    m->trapq = tuxrt_trapq_create(MAX_QUEUE);
    m->sc    = tuxrt_stepcompress_create(m->steps_per_mm, TUXRT_TRAPQ_MAX_AXES);
    m->ss    = tuxrt_steppersync_create(TUXRT_TRAPQ_MAX_AXES);
    if (m->trapq == NULL || m->sc == NULL || m->ss == NULL) {
        tuxrt_trapq_destroy(m->trapq);
        tuxrt_stepcompress_destroy(m->sc);
        tuxrt_steppersync_destroy(m->ss);
        m->trapq = NULL;
        m->sc = NULL;
        m->ss = NULL;
        return TUXRT_ERR_NOMEM;
    }
    return TUXRT_OK;
}

static void* stepper_thread(void* arg) {
    struct tuxrt_motion_t* m = (struct tuxrt_motion_t*)arg;
    const struct timespec tick = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */

    while (atomic_load(&m->running)) {
        nanosleep(&tick, NULL);
        if (atomic_load(&m->status) != TUXRT_MOTION_STATUS_RUNNING) {
            continue;
        }

        /* Calcular clock_s FUERA del mutex (solo lectura de reloj monotónico). */
        double clock_s = monotonic_s(m->t0_ns);

        /* Actualizar posición articular FUERA del mutex (solo lectura de trapq). */
        double pos[TUXRT_TRAPQ_MAX_AXES];
        uint8_t axes = 0;
        if (tuxrt_trapq_get_position(m->trapq, clock_s, pos, &axes) == TUXRT_OK) {
            memset(&m->current_joints, 0, sizeof(m->current_joints));
            for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
                m->current_joints.position[i] = pos[i];
            }
            m->current_joints.axis_count = axes;
        }

        /* Descartar los movimientos ya terminados FUERA del mutex. */
        m->popped_total += tuxrt_trapq_pop_until(m->trapq, clock_s, NULL);
        m->clock_s = clock_s;

        /* Ahora sí, entrar en mutex solo para la sección crítica de feed + flush. */
        pthread_mutex_lock(&m->q_mutex);

        /* Comprimir los movimientos aún no emitidos (DDA → steppersync). */
        size_t idx = m->fed_total - m->popped_total;
        size_t n = tuxrt_trapq_count(m->trapq);
        for (; idx < n; idx++) {
            const tuxrt_move_t* mv = tuxrt_trapq_at(m->trapq, idx);
            if (mv != NULL) {
                tuxrt_stepcompress_feed(m->sc, mv, feed_step, m);
            }
            m->fed_total++;
        }

        /* Liberar los pulsos cuyo instante ya ha vencido. */
        m->steps_emitted +=
            (uint64_t)tuxrt_steppersync_flush(m->ss, clock_s, discard_sink, NULL);

        pthread_mutex_unlock(&m->q_mutex);
    }
    return NULL;
}

// =============================================================================
// API
// =============================================================================

tuxrt_err_t tuxrt_motion_create(const tuxrt_config_t* cfg, const tuxrt_kinematics_t* kin, tuxrt_motion_t** out_motion) {
    if (!cfg || !kin || !out_motion) return TUXRT_ERR_INVAL;

    tuxrt_motion_t* m = calloc(1, sizeof(tuxrt_motion_t));
    if (!m) return TUXRT_ERR_NOMEM;

    m->cfg = cfg;
    m->kin = kin;
    m->mcfg = (tuxrt_motion_config_t){
        .profile = TUXRT_MOTION_TRAPEZOIDAL,
        .max_velocity = 100.0,
        .max_acceleration = 1000.0,
        .max_jerk = 10000.0,
        .junction_deviation = 0.01,
        .lookahead = 10
    };
    m->q_head = m->q_tail = m->q_count = 0;

    /* Pasos por mm: por defecto, o desde la configuración si está presente. */
    double spm = DEFAULT_STEPS_PER_MM;
    double cfg_spm = 0.0;
    if (tuxrt_config_get_double(cfg, "stepper.steps_per_mm", &cfg_spm) == TUXRT_OK &&
        cfg_spm > 0.0) {
        spm = cfg_spm;
    }
    for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
        m->steps_per_mm[i] = spm;
    }

    atomic_store(&m->status, TUXRT_MOTION_STATUS_IDLE);
    pthread_mutex_init(&m->q_mutex, NULL);

    m->t0_ns = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    m->t0_ns = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;

    atomic_store(&m->running, true);
    if (pthread_create(&m->stepper_thread, NULL, stepper_thread, m) != 0) {
        atomic_store(&m->running, false);
        pthread_mutex_destroy(&m->q_mutex);
        free(m);
        return TUXRT_ERR_IO;
    }

    *out_motion = m;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_motion_enqueue(tuxrt_motion_t* motion, const tuxrt_motion_cmd_t* cmd) {
    if (!motion || !cmd) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&motion->q_mutex);
    if (motion->q_count >= MAX_QUEUE) {
        pthread_mutex_unlock(&motion->q_mutex);
        return TUXRT_ERR_MOTION_QUEUE_FULL;
    }

    /* Pose objetivo (absoluta o relativa al último planificado). */
    tuxrt_pose_t target = cmd->target;
    if (cmd->relative) {
        target.pos.x += motion->last_pose.pos.x;
        target.pos.y += motion->last_pose.pos.y;
        target.pos.z += motion->last_pose.pos.z;
    }

    tuxrt_err_t err = ensure_pipeline(motion);
    tuxrt_joint_state_t joints;
    if (err == TUXRT_OK) {
        err = tuxrt_kinematics_inverse(motion->kin, &target, &joints);
    }

    if (err == TUXRT_OK) {
        double v = sqrt(cmd->velocity.x * cmd->velocity.x +
                        cmd->velocity.y * cmd->velocity.y +
                        cmd->velocity.z * cmd->velocity.z);
        if (!(v > 0.0)) {
            v = motion->mcfg.max_velocity;
        }
        v = clamp(v, 0.0, motion->mcfg.max_velocity);
        err = tuxrt_trapq_append(motion->trapq, joints.position,
                                 joints.axis_count, v,
                                 motion->mcfg.max_acceleration);
    }

    if (err == TUXRT_OK) {
        tuxrt_trapq_finalize(motion->trapq, motion->mcfg.junction_deviation,
                             motion->mcfg.max_acceleration,
                             motion->mcfg.max_velocity);
        motion->last_pose = target;

        motion->queue[motion->q_tail] = *cmd;
        motion->q_tail = (motion->q_tail + 1) % MAX_QUEUE;
        motion->q_count++;
        atomic_store(&motion->status, TUXRT_MOTION_STATUS_RUNNING);
    }

    pthread_mutex_unlock(&motion->q_mutex);
    return err;
}

tuxrt_err_t tuxrt_motion_step(tuxrt_motion_t* motion, tuxrt_timestamp_ns_t now, tuxrt_joint_state_t* out_joints) {
    if (!motion || !out_joints) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&motion->q_mutex);
    if (tuxrt_trapq_count(motion->trapq) > 0u) {
        double pos[TUXRT_TRAPQ_MAX_AXES];
        uint8_t axes = 0;
        double t = (double)now * 1e-9;
        if (tuxrt_trapq_get_position(motion->trapq, t, pos, &axes) == TUXRT_OK) {
            memset(out_joints, 0, sizeof(*out_joints));
            for (uint8_t i = 0; i < TUXRT_TRAPQ_MAX_AXES; i++) {
                out_joints->position[i] = pos[i];
            }
            out_joints->axis_count = axes;
        } else {
            *out_joints = motion->current_joints;
        }
    } else {
        *out_joints = motion->current_joints;
    }
    pthread_mutex_unlock(&motion->q_mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_motion_status(const tuxrt_motion_t* motion, tuxrt_motion_status_t* out_status, size_t* out_queued) {
    if (!motion || !out_status) return TUXRT_ERR_INVAL;
    *out_status = (tuxrt_motion_status_t)atomic_load(&motion->status);
    if (out_queued) *out_queued = motion->q_count;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_motion_pause(tuxrt_motion_t* motion) {
    if (!motion) return TUXRT_ERR_INVAL;
    atomic_store(&motion->status, TUXRT_MOTION_STATUS_PAUSED);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_motion_resume(tuxrt_motion_t* motion) {
    if (!motion) return TUXRT_ERR_INVAL;
    if (atomic_load(&motion->status) == TUXRT_MOTION_STATUS_PAUSED) {
        atomic_store(&motion->status, TUXRT_MOTION_STATUS_RUNNING);
    }
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_motion_emergency_stop(tuxrt_motion_t* motion) {
    if (!motion) return TUXRT_ERR_INVAL;
    atomic_store(&motion->status, TUXRT_MOTION_STATUS_ERROR);
    pthread_mutex_lock(&motion->q_mutex);
    motion->q_count = 0;
    motion->q_head = motion->q_tail = 0;
    pthread_mutex_unlock(&motion->q_mutex);
    return TUXRT_OK;
}

void tuxrt_motion_destroy(tuxrt_motion_t* motion) {
    if (!motion) return;
    atomic_store(&motion->running, false);
    if (motion->stepper_thread) pthread_join(motion->stepper_thread, NULL);
    tuxrt_trapq_destroy(motion->trapq);
    tuxrt_stepcompress_destroy(motion->sc);
    tuxrt_steppersync_destroy(motion->ss);
    pthread_mutex_destroy(&motion->q_mutex);
    free(motion);
}
