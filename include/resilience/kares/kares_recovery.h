/**
 * kares_recovery.h — KARES Kinematic-Aware Recovery & Emergency System (9 fases)
 *
 * Sistema de recuperación en 9 fases tras fallo:
 *   F0: CONFIG — Verificar hashes de configuración y cinemática
 *   F1: Z_SAFE — Elevar Z a safe_z
 *   F2: TEMPS — Leer y estabilizar temperaturas (target - 20°C)
 *   F3: STABILIZE — Esperar 30s, verificar deriva < 2°C
 *   F4: XY_HOME — Home de X/Y (no Z)
 *   F5: STATE — Cargar USS Checkpoint + WAL replay + validar ECC BCH
 *   F6: POSITION — Mover a joint_position del USS (50% velocidad)
 *   F7: RESUME — Priming + reanudar desde line_number/param_offset exacto
 *   F8: ACTUATORS — Restaurar husillo/láser con rampa suave
 *
 * Integración con USS Checkpoint (ECC BCH 32768,32640), CAMT, WAL.
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_KARES_RECOVERY_H
#define TUXRT_KARES_RECOVERY_H

#include "../../sdk/include/tuxrt_sdk.h"
#include "uss.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fases de KARES (9 fases completas)
 */
typedef enum {
    KARES_PHASE_IDLE = 0,
    KARES_PHASE_CONFIG = 1,       /* Verificar hashes */
    KARES_PHASE_Z_SAFE = 2,       /* Elevar Z */
    KARES_PHASE_TEMPS = 3,        /* Calentar */
    KARES_PHASE_STABILIZE = 4,    /* Estabilizar temps */
    KARES_PHASE_XY_HOME = 5,      /* Home X/Y */
    KARES_PHASE_STATE = 6,        /* USS + WAL replay */
    KARES_PHASE_POSITION = 7,     /* Mover a posiciones */
    KARES_PHASE_RESUME = 8,       /* Reanudar */
    KARES_PHASE_ACTUATORS = 9,    /* Restaurar actuadores */
    KARES_PHASE_COMPLETE = 10,
    KARES_PHASE_FAILED = 11
} kares_phase_t;

/**
 * Estados del sistema KARES
 */
typedef enum {
    KARES_STATUS_IDLE = 0,
    KARES_STATUS_RUNNING = 1,
    KARES_STATUS_PAUSED = 2,
    KARES_STATUS_COMPLETE = 3,
    KARES_STATUS_FAILED = 4
} kares_status_t;

/**
 * Flags de estado de recuperación
 */
typedef enum {
    KARES_FLAG_NONE = 0,
    KARES_FLAG_CONFIG_DONE = (1 << 0),
    KARES_FLAG_Z_SAFE_DONE = (1 << 1),
    KARES_FLAG_TEMPS_DONE = (1 << 2),
    KARES_FLAG_STABILIZE_DONE = (1 << 3),
    KARES_FLAG_XY_HOME_DONE = (1 << 4),
    KARES_FLAG_STATE_LOADED = (1 << 5),
    KARES_FLAG_POSITION_DONE = (1 << 6),
    KARES_FLAG_RESUME_DONE = (1 << 7),
    KARES_FLAG_ACTUATORS_DONE = (1 << 8)
} kares_flags_t;

/**
 * Configuración de KARES
 */
typedef struct {
    double safe_z;                      /* Altura segura de Z (mm) */
    double tool_retract_z;              /* Altura de retracción de herramienta */
    double temp_margin;                 /* Margen de temperatura (°C), típicamente 20 */
    double stabilize_time;              /* Tiempo de estabilización (s), típicamente 30 */
    double temp_drift_max;              /* Deriva máxima de temp (°C), típicamente 2 */
    double position_speed;              /* Velocidad de posicionamiento (%), típicamente 50 */
    double priming_amount;              /* Cantidad de priming (mm), típicamente 2-5 */
    double actuator_ramp_ms;            /* Rampa para actuadores (ms), típicamente 200-500 */
    uint32_t max_retries;               /* Reintentos máximos por fase */
    uint32_t checkpoint_interval_secs;  /* Frecuencia de checkpoint (segundos) */
    uint32_t max_ecc_errors;            /* Máximo de errores ECC correctables (16) */
    bool enable_camt;                   /* Habilitar CAMT multi-toolhead */
    bool enable_wal_replay;             /* Habilitar WAL replay */
} kares_config_t;

/**
 * Estado de temperaturas para estabilización
 */
typedef struct {
    double hotend_current;
    double hotend_target;
    double bed_current;
    double bed_target;
    double coolant_current;
    double coolant_max;
    double hotend_history[30];  /* Historial 30s */
    double bed_history[30];
    size_t history_count;
} kares_temps_state_t;

/**
 * Callback de fase de KARES
 *
 * @param ctx Contexto de KARES
 * @param phase Fase actual
 * @param phase_status Estado de la fase (RUNNING, COMPLETE, FAILED)
 * @param user_data Datos de usuario
 */
typedef void (*kares_phase_callback_t)(
    void* ctx,
    kares_phase_t phase,
    kares_status_t phase_status,
    void* user_data
);

/**
 * Contexto principal de KARES con USS integrando
 */
typedef struct kares_recovery_ctx {
    kares_config_t config;
    kares_phase_t current_phase;
    kares_status_t status;
    uint32_t flags;
    
    /* Estado USS */
    uss_ctx_t* uss_ctx;
    uss_snapshot_t uss_snapshot;
    
    /* Estado de la máquina */
    struct {
        double joint_position[USS_MAX_AXES];
        double task_position[USS_MAX_TOOLS][6];
        double actuator_setpoint[USS_MAX_AXES];
        double actuator_ramp[USS_MAX_AXES];
        uint8_t actuator_dir[USS_MAX_AXES];
        uint8_t coolant[USS_MAX_AXES];
    } machine_state;
    
    /* Cursores multi-toolhead (CAMT) */
    struct {
        uint32_t tool_line_number[USS_MAX_TOOLS];
        uint32_t tool_layer_number[USS_MAX_TOOLS];
        double tool_param_offset[USS_MAX_TOOLS];
        uint8_t tool_state[USS_MAX_TOOLS];
        uint8_t tool_type[USS_MAX_TOOLS];
    } camt_state;
    
    /* Temperaturas */
    kares_temps_state_t temps;
    
    /* Control de reintentos */
    uint32_t retry_count;
    
    /* Callbacks */
    kares_phase_callback_t phase_callback;
    void* user_data;
    
    /* Estado interno */
    bool initialized;
    int64_t phase_start_time_ns;
    double initial_temp_hotend;
    double initial_temp_bed;
    
    /* WAL */
    void* wal_ctx;  /* tuxrt_wal_t* */
    uint64_t wal_sequence_start;
    uint64_t wal_entries_applied;
    
    /* Resultados */
    kares_phase_t final_phase;
    uint32_t error_code;
    uint64_t total_time_ns;
    uint32_t lines_recovered;
    
    /* Estado de compensación */
    double backlash_comp[USS_MAX_AXES];
    double screw_comp[USS_MAX_AXES];
    double thermal_comp[USS_MAX_AXES];
} kares_recovery_ctx_t;

/**
 * Resultado de la recuperación
 */
typedef struct {
    bool success;
    kares_phase_t last_phase;
    uint32_t error_code;
    uint64_t total_time_ns;
    uint32_t lines_recovered;
    uint32_t wal_entries_applied;
    bool ecc_corrected;
    uint32_t ecc_bits_corrected;
} kares_result_t;

/* ============================================================================
 *  API Pública - Gestión del contexto
 * ============================================================================ */

/**
 * Crear contexto de recuperación KARES
 *
 * @param config Configuración de KARES
 * @param out_ctx Contexto resultante
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_create(
    const kares_config_t* config,
    kares_recovery_ctx_t** out_ctx
);

/**
 * Configurar USS para KARES
 *
 * @param ctx Contexto de KARES
 * @param checkpoint_path_a Ruta buffer A
 * @param checkpoint_path_b Ruta buffer B
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_config_uss(
    kares_recovery_ctx_t* ctx,
    const char* checkpoint_path_a,
    const char* checkpoint_path_b
);

/**
 * Iniciar recuperación KARES
 *
 * @param ctx Contexto de KARES
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_start(kares_recovery_ctx_t* ctx);

/**
 * Ejecutar un paso de la máquina de estados de KARES
 * Debe llamarse en un loop hasta que should_continue sea false
 *
 * @param ctx Contexto de KARES
 * @param should_continue Salida: true si debe continuar, false si completó/falló
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_step(kares_recovery_ctx_t* ctx, bool* should_continue);

/**
 * Obtener fase actual
 *
 * @param ctx Contexto de KARES
 * @return Fase actual
 */
kares_phase_t tuxrt_kares_recovery_get_phase(const kares_recovery_ctx_t* ctx);

/**
 * Obtener estado actual
 *
 * @param ctx Contexto de KARES
 * @return Estado actual
 */
kares_status_t tuxrt_kares_recovery_get_status(const kares_recovery_ctx_t* ctx);

/**
 * Obtener progreso de la fase actual (0.0 - 1.0)
 *
 * @param ctx Contexto de KARES
 * @return Progreso
 */
double tuxrt_kares_recovery_get_progress(const kares_recovery_ctx_t* ctx);

/**
 * Pausar recuperación
 *
 * @param ctx Contexto de KARES
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_pause(kares_recovery_ctx_t* ctx);

/**
 * Reanudar recuperación
 *
 * @param ctx Contexto de KARES
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_resume(kares_recovery_ctx_t* ctx);

/**
 * Cancelar recuperación
 *
 * @param ctx Contexto de KARES
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_cancel(kares_recovery_ctx_t* ctx);

/**
 * Obtener resultado de la recuperación
 *
 * @param ctx Contexto de KARES
 * @param result Resultado
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_recovery_get_result(
    const kares_recovery_ctx_t* ctx,
    kares_result_t* result
);

/**
 * Destruir contexto de KARES
 *
 * @param ctx Contexto de KARES
 */
void tuxrt_kares_recovery_destroy(kares_recovery_ctx_t* ctx);

/**
 * Obtener nombre legible de la fase
 *
 * @param phase Fase
 * @return Nombre de la fase
 */
const char* tuxrt_kares_phase_name(kares_phase_t phase);

/**
 * Obtener nombre legible del estado
 *
 * @param status Estado
 * @return Nombre del estado
 */
const char* tuxrt_kares_status_name(kares_status_t status);

/**
 * Verificar consistencia joint ↔ task
 *
 * @param ctx Contexto de KARES
 * @param tool_idx Índice de herramienta
 * @param error_msg Mensaje de error opcional
 * @return true si es consistente
 */
bool tuxrt_kares_verify_consistency(
    const kares_recovery_ctx_t* ctx,
    uint32_t tool_idx,
    char* error_msg,
    size_t error_msg_len
);

/**
 * Aplicar WAL replay desde secuencia USS
 *
 * @param ctx Contexto de KARES
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_apply_wal_replay(kares_recovery_ctx_t* ctx);

/* ============================================================================
 *  API de hardware (callbacks a implementar por el usuario)
 * ============================================================================ */

/**
 * Leer temperaturas actuales
 */
typedef struct {
    double hotend_current;
    double hotend_target;
    double bed_current;
    double bed_target;
    double coolant_current;
} kares_temp_readings_t;

/**
 * Mover eje a posición objetivo
 */
typedef tuxrt_err_t (*kares_move_callback_t)(
    void* user_data,
    uint32_t axis_id,
    double position,
    double max_velocity_pct,
    bool wait_for_completion
);

/**
 * Hacer home de eje
 */
typedef tuxrt_err_t (*kares_home_callback_t)(
    void* user_data,
    uint32_t axis_id
);

/**
 * Establecer temperatura
 */
typedef tuxrt_err_t (*kares_temp_callback_t)(
    void* user_data,
    const char* sensor_name,
    double target_temp
);

/**
 * Extruir filamento (priming)
 */
typedef tuxrt_err_t (*kares_extrude_callback_t)(
    void* user_data,
    double amount_mm
);

/**
 * Configurar husillo
 */
typedef tuxrt_err_t (*kares_spindle_callback_t)(
    void* user_data,
    uint32_t axis_id,
    double rpm_target,
    uint8_t direction,
    uint8_t coolant,
    double ramp_ms
);

/**
 * Configurar láser
 */
typedef tuxrt_err_t (*kares_laser_callback_t)(
    void* user_data,
    uint32_t axis_id,
    double power_pct,
    uint8_t mode,
    double ramp_ms
);

/**
 * Obtener posición actual del eje
 */
typedef tuxrt_err_t (*kares_position_callback_t)(
    void* user_data,
    uint32_t axis_id,
    double* position
);

/**
 * Registrar callbacks de hardware
 */
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
);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_KARES_RECOVERY_H */
