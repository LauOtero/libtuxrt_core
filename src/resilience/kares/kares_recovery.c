/**
 * kares_recovery.c — Implementación de las 9 fases de KARES
 *
 * Máquina de estados para recuperación tras fallo:
 *   F0: CONFIG, F1: Z_SAFE, F2: TEMPS, F3: STABILIZE, F4: XY_HOME
 *   F5: STATE (USS + WAL), F6: POSITION, F7: RESUME, F8: ACTUATORS
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#include "kares_recovery.h"
#include "uss.h"
#include "uss_ecc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* ============================================================================
 *  Utilidades
 * ============================================================================ */

static int64_t get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static double fabs_custom(double x)
{
    return (x < 0) ? -x : x;
}

const char* tuxrt_kares_phase_name(kares_phase_t phase)
{
    switch (phase) {
        case KARES_PHASE_IDLE: return "IDLE";
        case KARES_PHASE_CONFIG: return "CONFIG (F0)";
        case KARES_PHASE_Z_SAFE: return "Z_SAFE (F1)";
        case KARES_PHASE_TEMPS: return "TEMPS (F2)";
        case KARES_PHASE_STABILIZE: return "STABILIZE (F3)";
        case KARES_PHASE_XY_HOME: return "XY_HOME (F4)";
        case KARES_PHASE_STATE: return "STATE (F5)";
        case KARES_PHASE_POSITION: return "POSITION (F6)";
        case KARES_PHASE_RESUME: return "RESUME (F7)";
        case KARES_PHASE_ACTUATORS: return "ACTUATORS (F8)";
        case KARES_PHASE_COMPLETE: return "COMPLETE";
        case KARES_PHASE_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

const char* tuxrt_kares_status_name(kares_status_t status)
{
    switch (status) {
        case KARES_STATUS_IDLE: return "IDLE";
        case KARES_STATUS_RUNNING: return "RUNNING";
        case KARES_STATUS_PAUSED: return "PAUSED";
        case KARES_STATUS_COMPLETE: return "COMPLETE";
        case KARES_STATUS_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

/* ============================================================================
 *  Gestión del contexto
 * ============================================================================ */

tuxrt_err_t tuxrt_kares_recovery_create(
    const kares_config_t* config,
    kares_recovery_ctx_t** out_ctx
)
{
    if (!config || !out_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    kares_recovery_ctx_t* ctx = (kares_recovery_ctx_t*)calloc(1, sizeof(kares_recovery_ctx_t));
    if (!ctx) {
        return TUXRT_ERR_NOMEM;
    }
    
    /* Copiar configuración */
    memcpy(&ctx->config, config, sizeof(kares_config_t));
    
    /* Inicializar estado */
    ctx->current_phase = KARES_PHASE_IDLE;
    ctx->status = KARES_STATUS_IDLE;
    ctx->flags = 0;
    ctx->uss_ctx = NULL;
    ctx->wal_ctx = NULL;
    ctx->initialized = true;
    ctx->retry_count = 0;
    ctx->phase_callback = NULL;
    ctx->user_data = NULL;
    
    /* Inicializar estado de máquina */
    memset(&ctx->machine_state, 0, sizeof(ctx->machine_state));
    memset(&ctx->camt_state, 0, sizeof(ctx->camt_state));
    memset(&ctx->temps, 0, sizeof(ctx->temps));
    
    *out_ctx = ctx;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_recovery_config_uss(
    kares_recovery_ctx_t* ctx,
    const char* checkpoint_path_a,
    const char* checkpoint_path_b
)
{
    if (!ctx || !ctx->initialized || !checkpoint_path_a || !checkpoint_path_b) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Crear contexto USS */
    tuxrt_err_t ret = tuxrt_uss_create(
        checkpoint_path_a,
        checkpoint_path_b,
        ctx->config.checkpoint_interval_secs,
        &ctx->uss_ctx
    );
    
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    /* Inicializar ECC para USS */
    ret = tuxrt_uss_init_ecc(ctx->uss_ctx);
    if (ret != TUXRT_OK) {
        tuxrt_uss_destroy(ctx->uss_ctx);
        ctx->uss_ctx = NULL;
        return ret;
    }
    
    return TUXRT_OK;
}

void tuxrt_kares_recovery_destroy(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return;
    }
    
    /* Destruir USS */
    if (ctx->uss_ctx) {
        tuxrt_uss_destroy(ctx->uss_ctx);
    }
    
    /* Destruir WAL si existe */
    if (ctx->wal_ctx) {
        /* tuxrt_wal_destroy(ctx->wal_ctx); */
    }
    
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

/* ============================================================================
 *  Callbacks de hardware (placeholders - implementar por el usuario)
 * ============================================================================ */

static struct {
    kares_move_callback_t move_cb;
    kares_home_callback_t home_cb;
    kares_temp_callback_t temp_cb;
    kares_extrude_callback_t extrude_cb;
    kares_spindle_callback_t spindle_cb;
    kares_laser_callback_t laser_cb;
    kares_position_callback_t position_cb;
    void* user_data;
} g_hw_callbacks = {0};

tuxrt_err_t tuxrt_kares_register_hw_callbacks(
    kares_recovery_ctx_t* ctx,
    kares_move_callback_t move_cb,
    kares_home_callback_t home_cb,
    kares_temp_callback_t temp_cb,
    kares_extrude_callback_t extrude_cb,
    kares_spindle_callback_t spindle_cb,
    kares_laser_callback_t laser_cb,
    kares_position_callback_t position_cb,
    void* user_data
)
{
    (void)ctx;
    
    g_hw_callbacks.move_cb = move_cb;
    g_hw_callbacks.home_cb = home_cb;
    g_hw_callbacks.temp_cb = temp_cb;
    g_hw_callbacks.extrude_cb = extrude_cb;
    g_hw_callbacks.spindle_cb = spindle_cb;
    g_hw_callbacks.laser_cb = laser_cb;
    g_hw_callbacks.position_cb = position_cb;
    g_hw_callbacks.user_data = user_data;
    
    return TUXRT_OK;
}

/* ============================================================================
 *  Implementación de las 9 fases
 * ============================================================================ */

/**
 * F0: CONFIG — Verificar hashes de configuración y cinemática
 */
static tuxrt_err_t kares_phase_config(kares_recovery_ctx_t* ctx)
{
    if (!ctx || !ctx->uss_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Leer snapshot USS */
    bool corrected = false;
    tuxrt_err_t ret = tuxrt_uss_read(ctx->uss_ctx, &ctx->uss_snapshot, &corrected);
    if (ret != TUXRT_OK) {
        /* Intentar con snapshot anterior */
        ret = tuxrt_uss_load_previous(ctx->uss_ctx, &ctx->uss_snapshot, &corrected);
        if (ret != TUXRT_OK) {
            return ret;
        }
    }
    
    /* Verificar magic */
    if (ctx->uss_snapshot.header.magic != USS_MAGIC) {
        return TUXRT_ERR_INVALID_MAGIC;
    }
    
    /* Verificar version */
    if (ctx->uss_snapshot.header.version != USS_VERSION) {
        return TUXRT_ERR_VERSION_MISMATCH;
    }
    
    /* Verificar flags */
    if (!(ctx->uss_snapshot.header.flags & USS_FLAG_VALID)) {
        return TUXRT_ERR_INVALID_STATE;
    }
    
    /* TODO: Verificar config_hash contra configuración actual */
    /* TODO: Verificar kinematics_hash contra solver actual */
    
    ctx->flags |= KARES_FLAG_CONFIG_DONE;
    
    return TUXRT_OK;
}

/**
 * F1: Z_SAFE — Elevar Z a safe_z
 */
static tuxrt_err_t kares_phase_z_safe(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Obtener posición Z actual */
    double current_z = 0;
    if (g_hw_callbacks.position_cb) {
        g_hw_callbacks.position_cb(g_hw_callbacks.user_data, 2, &current_z);  /* Eje 2 = Z */
    }
    
    /* Calcular posición objetivo */
    double target_z = ctx->config.safe_z;
    
    /* Mover Z a safe_z */
    if (g_hw_callbacks.move_cb) {
        tuxrt_err_t ret = g_hw_callbacks.move_cb(
            g_hw_callbacks.user_data,
            2,  /* Eje Z */
            target_z,
            50.0,  /* 50% velocidad */
            true   /* Esperar completación */
        );
        
        if (ret != TUXRT_OK) {
            return ret;
        }
    }
    
    ctx->flags |= KARES_FLAG_Z_SAFE_DONE;
    
    return TUXRT_OK;
}

/**
 * F2: TEMPS — Leer y estabilizar temperaturas (target - 20°C)
 */
static tuxrt_err_t kares_phase_temps(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Leer temperaturas actuales */
    kares_temp_readings_t readings = {0};
    
    /* TODO: Leer temperaturas reales del hardware */
    /* Por ahora, usar valores del snapshot USS */
    ctx->temps.hotend_current = 0;  /* Leer de sensors */
    ctx->temps.hotend_target = 0;   /* Leer de snapshot */
    ctx->temps.bed_current = 0;
    ctx->temps.bed_target = 0;
    
    /* Calentar hasta target - margin */
    double target_hotend = ctx->temps.hotend_target - ctx->config.temp_margin;
    double target_bed = ctx->temps.bed_target - ctx->config.temp_margin;
    
    if (g_hw_callbacks.temp_cb) {
        g_hw_callbacks.temp_cb(g_hw_callbacks.user_data, "hotend", target_hotend);
        g_hw_callbacks.temp_cb(g_hw_callbacks.user_data, "bed", target_bed);
    }
    
    /* Esperar alcance de temperatura (simplificado) */
    /* En implementación real, poll hasta alcanzar target */
    
    ctx->flags |= KARES_FLAG_TEMPS_DONE;
    
    return TUXRT_OK;
}

/**
 * F3: STABILIZE — Esperar 30s, verificar deriva < 2°C
 */
static tuxrt_err_t kares_phase_stabilize(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    int64_t start_time = get_timestamp_ns();
    int64_t stabilize_time_ns = (int64_t)(ctx->config.stabilize_time * 1000000000.0);
    
    /* Esperar tiempo de estabilización */
    while (1) {
        int64_t now = get_timestamp_ns();
        int64_t elapsed = now - start_time;
        
        if (elapsed >= stabilize_time_ns) {
            break;
        }
        
        /* TODO: Muestrear temperaturas y verificar deriva */
        /* Deriva máxima permitida: config.temp_drift_max */
    }
    
    /* Verificar deriva de temperatura */
    double drift = fabs_custom(ctx->temps.hotend_current - ctx->temps.hotend_target);
    if (drift > ctx->config.temp_drift_max) {
        /* Reintentar si deriva es excesiva */
        if (ctx->retry_count < ctx->config.max_retries) {
            ctx->retry_count++;
            return TUXRT_ERR_RETRY;
        }
        return TUXRT_ERR_TEMP_DRIFT;
    }
    
    ctx->flags |= KARES_FLAG_STABILIZE_DONE;
    ctx->retry_count = 0;
    
    return TUXRT_OK;
}

/**
 * F4: XY_HOME — Home de X/Y (no Z)
 */
static tuxrt_err_t kares_phase_xy_home(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Hacer home de X (eje 0) */
    if (g_hw_callbacks.home_cb) {
        tuxrt_err_t ret = g_hw_callbacks.home_cb(g_hw_callbacks.user_data, 0);
        if (ret != TUXRT_OK) {
            return ret;
        }
    }
    
    /* Hacer home de Y (eje 1) */
    if (g_hw_callbacks.home_cb) {
        tuxrt_err_t ret = g_hw_callbacks.home_cb(g_hw_callbacks.user_data, 1);
        if (ret != TUXRT_OK) {
            return ret;
        }
    }
    
    /* TODO: Hacer home de ejes rotativos si existen */
    
    ctx->flags |= KARES_FLAG_XY_HOME_DONE;
    
    return TUXRT_OK;
}

/**
 * F5: STATE — Cargar USS Checkpoint + WAL replay + validar ECC BCH
 */
static tuxrt_err_t kares_phase_state(kares_recovery_ctx_t* ctx)
{
    if (!ctx || !ctx->uss_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* El ECC del snapshot ya fue verificado y corregido en F0 por tuxrt_uss_read(),
     * por lo que el snapshot en ctx es válido en este punto. */
    /* Aplicar WAL replay si está habilitado */
    if (ctx->config.enable_wal_replay && ctx->wal_ctx) {
        tuxrt_err_t ret = tuxrt_kares_apply_wal_replay(ctx);
        if (ret != TUXRT_OK) {
            /* WAL replay falló, pero continuar con USS */
            /* El USS checkpoint es suficiente para recuperación básica */
        }
    }
    
    /* Cargar estado de la máquina desde USS */
    for (uint32_t i = 0; i < ctx->uss_snapshot.header.axis_count && i < USS_MAX_AXES; i++) {
        ctx->machine_state.joint_position[i] = ctx->uss_snapshot.axes[i].joint_position;
        ctx->machine_state.actuator_setpoint[i] = ctx->uss_snapshot.axes[i].actuator_setpoint;
        ctx->machine_state.actuator_ramp[i] = ctx->uss_snapshot.axes[i].actuator_ramp;
        ctx->machine_state.actuator_dir[i] = ctx->uss_snapshot.axes[i].actuator_dir;
        ctx->machine_state.coolant[i] = ctx->uss_snapshot.axes[i].coolant;
        
        /* Compensaciones */
        ctx->backlash_comp[i] = ctx->uss_snapshot.axes[i].backlash_comp;
        ctx->screw_comp[i] = ctx->uss_snapshot.axes[i].screw_comp;
        ctx->thermal_comp[i] = ctx->uss_snapshot.axes[i].thermal_comp;
    }
    
    /* Cargar estado de herramientas */
    for (uint32_t t = 0; t < ctx->uss_snapshot.header.tool_count && t < USS_MAX_TOOLS; t++) {
        ctx->camt_state.tool_line_number[t] = ctx->uss_snapshot.tools[t].line_number;
        ctx->camt_state.tool_layer_number[t] = ctx->uss_snapshot.tools[t].layer_number;
        ctx->camt_state.tool_param_offset[t] = ctx->uss_snapshot.tools[t].param_offset;
        ctx->camt_state.tool_state[t] = ctx->uss_snapshot.tools[t].state;
        ctx->camt_state.tool_type[t] = ctx->uss_snapshot.tools[t].tool_type;
        
        /* Task space */
        for (int i = 0; i < 6; i++) {
            ctx->machine_state.task_position[t][i] = ctx->uss_snapshot.tools[t].task_position[i];
        }
    }
    
    ctx->flags |= KARES_FLAG_STATE_LOADED;
    
    return TUXRT_OK;
}

/**
 * F6: POSITION — Mover a joint_position del USS (50% velocidad)
 */
static tuxrt_err_t kares_phase_position(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Mover cada eje a su posición guardada */
    for (uint32_t axis = 0; axis < ctx->uss_snapshot.header.axis_count && axis < USS_MAX_AXES; axis++) {
        /* Skip Z (ya está en safe_z) */
        if (axis == 2) {  /* Eje Z */
            continue;
        }
        
        /* Skip SPINDLE y LASER (se restauran en F8) */
        if (ctx->uss_snapshot.axes[axis].axis_type == USS_AXIS_SPINDLE ||
            ctx->uss_snapshot.axes[axis].axis_type == USS_AXIS_LASER) {
            continue;
        }
        
        double target_pos = ctx->uss_snapshot.axes[axis].joint_position;
        
        if (g_hw_callbacks.move_cb) {
            tuxrt_err_t ret = g_hw_callbacks.move_cb(
                g_hw_callbacks.user_data,
                axis,
                target_pos,
                ctx->config.position_speed,  /* 50% velocidad */
                true  /* Esperar completación */
            );
            
            if (ret != TUXRT_OK) {
                return ret;
            }
        }
    }
    
    ctx->flags |= KARES_FLAG_POSITION_DONE;
    
    return TUXRT_OK;
}

/**
 * F7: RESUME — Priming + reanudar desde line_number/param_offset exacto
 */
static tuxrt_err_t kares_phase_resume(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Obtener herramienta activa */
    uint32_t active_tool = ctx->uss_snapshot.header.active_tool;
    
    /* Priming para FDM (extruir 2-5 mm) */
    if (active_tool < USS_MAX_TOOLS && 
        ctx->camt_state.tool_type[active_tool] == USS_TOOL_EXTRUDER) {
        
        if (g_hw_callbacks.extrude_cb) {
            tuxrt_err_t ret = g_hw_callbacks.extrude_cb(
                g_hw_callbacks.user_data,
                ctx->config.priming_amount  /* 2-5 mm */
            );
            
            if (ret != TUXRT_OK) {
                return ret;
            }
        }
    }
    
    /* Reanudar G-code desde line_number + param_offset */
    uint32_t resume_line = ctx->camt_state.tool_line_number[active_tool];
    double param_offset = ctx->camt_state.tool_param_offset[active_tool];
    
    /* TODO: Reanudar G-code en (resume_line, param_offset) */
    /* param_offset permite reanudar en punto exacto del movimiento */
    
    ctx->flags |= KARES_FLAG_RESUME_DONE;
    
    return TUXRT_OK;
}

/**
 * F8: ACTUATORS — Restaurar husillo/láser con rampa suave
 */
static tuxrt_err_t kares_phase_actuators(kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Restaurar husillo y láser según tipo de herramienta */
    for (uint32_t axis = 0; axis < ctx->uss_snapshot.header.axis_count && axis < USS_MAX_AXES; axis++) {
        uint8_t axis_type = ctx->uss_snapshot.axes[axis].axis_type;
        
        if (axis_type == USS_AXIS_SPINDLE) {
            /* Restaurar husillo con rampa */
            if (g_hw_callbacks.spindle_cb) {
                double rpm = ctx->machine_state.actuator_setpoint[axis];
                uint8_t dir = ctx->machine_state.actuator_dir[axis];
                uint8_t coolant = ctx->machine_state.coolant[axis];
                
                tuxrt_err_t ret = g_hw_callbacks.spindle_cb(
                    g_hw_callbacks.user_data,
                    axis,
                    rpm,
                    dir,
                    coolant,
                    ctx->config.actuator_ramp_ms  /* 200-500 ms */
                );
                
                if (ret != TUXRT_OK) {
                    return ret;
                }
            }
        } else if (axis_type == USS_AXIS_LASER) {
            /* Restaurar láser con rampa */
            if (g_hw_callbacks.laser_cb) {
                double power = ctx->machine_state.actuator_setpoint[axis];
                uint8_t mode = ctx->machine_state.actuator_dir[axis];
                
                tuxrt_err_t ret = g_hw_callbacks.laser_cb(
                    g_hw_callbacks.user_data,
                    axis,
                    power,
                    mode,
                    ctx->config.actuator_ramp_ms
                );
                
                if (ret != TUXRT_OK) {
                    return ret;
                }
            }
        }
    }
    
    ctx->flags |= KARES_FLAG_ACTUATORS_DONE;
    
    return TUXRT_OK;
}

/* ============================================================================
 *  Máquina de estados principal
 * ============================================================================ */

tuxrt_err_t tuxrt_kares_recovery_start(kares_recovery_ctx_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    ctx->status = KARES_STATUS_RUNNING;
    ctx->current_phase = KARES_PHASE_CONFIG;
    ctx->phase_start_time_ns = get_timestamp_ns();
    ctx->retry_count = 0;
    
    /* Notificar callback */
    if (ctx->phase_callback) {
        ctx->phase_callback(ctx, ctx->current_phase, KARES_STATUS_RUNNING, ctx->user_data);
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_recovery_step(kares_recovery_ctx_t* ctx, bool* should_continue)
{
    if (!ctx || !ctx->initialized || !should_continue) {
        return TUXRT_ERR_INVAL;
    }
    
    if (ctx->status != KARES_STATUS_RUNNING) {
        *should_continue = false;
        return TUXRT_OK;
    }
    
    tuxrt_err_t ret = TUXRT_OK;
    
    /* Ejecutar fase actual */
    switch (ctx->current_phase) {
        case KARES_PHASE_CONFIG:
            ret = kares_phase_config(ctx);
            break;
        
        case KARES_PHASE_Z_SAFE:
            ret = kares_phase_z_safe(ctx);
            break;
        
        case KARES_PHASE_TEMPS:
            ret = kares_phase_temps(ctx);
            break;
        
        case KARES_PHASE_STABILIZE:
            ret = kares_phase_stabilize(ctx);
            break;
        
        case KARES_PHASE_XY_HOME:
            ret = kares_phase_xy_home(ctx);
            break;
        
        case KARES_PHASE_STATE:
            ret = kares_phase_state(ctx);
            break;
        
        case KARES_PHASE_POSITION:
            ret = kares_phase_position(ctx);
            break;
        
        case KARES_PHASE_RESUME:
            ret = kares_phase_resume(ctx);
            break;
        
        case KARES_PHASE_ACTUATORS:
            ret = kares_phase_actuators(ctx);
            break;
        
        default:
            ret = TUXRT_ERR_INVAL;
            break;
    }
    
    /* Manejar resultado */
    if (ret == TUXRT_OK) {
        /* Fase completada, avanzar a siguiente */
        ctx->current_phase++;
        ctx->phase_start_time_ns = get_timestamp_ns();
        
        /* Notificar callback */
        if (ctx->phase_callback) {
            ctx->phase_callback(ctx, ctx->current_phase, KARES_STATUS_RUNNING, ctx->user_data);
        }
        
        /* Verificar si completó todas las fases */
        if (ctx->current_phase >= KARES_PHASE_COMPLETE) {
            ctx->status = KARES_STATUS_COMPLETE;
            ctx->final_phase = KARES_PHASE_COMPLETE;
            *should_continue = false;
        } else {
            *should_continue = true;
        }
        
    } else if (ret == TUXRT_ERR_RETRY) {
        /* Reintentar fase actual */
        ctx->retry_count++;
        
        if (ctx->retry_count >= ctx->config.max_retries) {
            ctx->status = KARES_STATUS_FAILED;
            ctx->final_phase = ctx->current_phase;
            ctx->error_code = ret;
            *should_continue = false;
        } else {
            *should_continue = true;
        }
        
    } else {
        /* Error fatal */
        ctx->status = KARES_STATUS_FAILED;
        ctx->final_phase = ctx->current_phase;
        ctx->error_code = ret;
        *should_continue = false;
    }
    
    return ret;
}

kares_phase_t tuxrt_kares_recovery_get_phase(const kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return KARES_PHASE_IDLE;
    }
    return ctx->current_phase;
}

kares_status_t tuxrt_kares_recovery_get_status(const kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return KARES_STATUS_IDLE;
    }
    return ctx->status;
}

double tuxrt_kares_recovery_get_progress(const kares_recovery_ctx_t* ctx)
{
    if (!ctx) {
        return 0.0;
    }
    
    /* Progreso basado en fase actual (9 fases totales) */
    double progress = (double)ctx->current_phase / 9.0;
    
    /* Clamp a [0.0, 1.0] */
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;
    
    return progress;
}

tuxrt_err_t tuxrt_kares_recovery_pause(kares_recovery_ctx_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    if (ctx->status != KARES_STATUS_RUNNING) {
        return TUXRT_ERR_INVALID_STATE;
    }
    
    ctx->status = KARES_STATUS_PAUSED;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_recovery_resume(kares_recovery_ctx_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    if (ctx->status != KARES_STATUS_PAUSED) {
        return TUXRT_ERR_INVALID_STATE;
    }
    
    ctx->status = KARES_STATUS_RUNNING;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_recovery_cancel(kares_recovery_ctx_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    ctx->status = KARES_STATUS_FAILED;
    ctx->final_phase = ctx->current_phase;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_recovery_get_result(
    const kares_recovery_ctx_t* ctx,
    kares_result_t* result
)
{
    if (!ctx || !result) {
        return TUXRT_ERR_INVAL;
    }
    
    result->success = (ctx->status == KARES_STATUS_COMPLETE);
    result->last_phase = ctx->final_phase;
    result->error_code = ctx->error_code;
    result->total_time_ns = ctx->total_time_ns;
    result->lines_recovered = ctx->lines_recovered;
    result->wal_entries_applied = (uint32_t)ctx->wal_entries_applied;
    result->ecc_corrected = false;  /* TODO: obtener de USS */
    result->ecc_bits_corrected = 0;  /* TODO: obtener de USS */
    
    return TUXRT_OK;
}

bool tuxrt_kares_verify_consistency(
    const kares_recovery_ctx_t* ctx,
    uint32_t tool_idx,
    char* error_msg,
    size_t error_msg_len
)
{
    if (!ctx || tool_idx >= USS_MAX_TOOLS) {
        if (error_msg && error_msg_len > 0) {
            snprintf(error_msg, error_msg_len, "Índice de herramienta inválido");
        }
        return false;
    }
    
    /* TODO: Verificar consistencia joint ↔ task usando forward kinematics */
    /* forward(joint_positions) ≈ task_position */
    
    /* Por ahora, siempre retornar true */
    return true;
}

tuxrt_err_t tuxrt_kares_apply_wal_replay(kares_recovery_ctx_t* ctx)
{
    if (!ctx || !ctx->wal_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    /* TODO: Implementar WAL replay desde wal_sequence_start */
    /* 1. Leer entradas WAL desde secuencia USS */
    /* 2. Aplicar cada entrada al estado de la máquina */
    /* 3. Verificar CRC32 de cada entrada */
    /* 4. Actualizar camt_state con cursores */
    
    ctx->wal_entries_applied = 0;
    
    return TUXRT_OK;
}
