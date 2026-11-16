/**
 * wal.h — Write-Ahead Log (WAL) con 11 tipos de entrada para KARES
 *
 * 11 tipos de entrada según especificación KARES:
 *   1. WAL_AXIS_JOINT_UPDATE — Posición, velocidad, aceleración de ejes
 *   2. WAL_TASK_POSE_UPDATE — Pose en task space
 *   3. WAL_CURSOR_UPDATE — Cursor G-code (line_number, param_offset)
 *   4. WAL_FEEDRATE_UPDATE — Feedrate y override
 *   5. WAL_SPINDLE_UPDATE — RPM, dirección, refrigerante
 *   6. WAL_LASER_UPDATE — Potencia, modo
 *   7. WAL_EXTRUDER_UPDATE — E-position, pressure advance, flow
 *   8. WAL_TOOL_CHANGE — Cambio de herramienta
 *   9. WAL_COORD_SYSTEM_CHANGE — Cambio de WCS, G92
 *  10. WAL_HOMING_STATE — Estado de homing
 *  11. WAL_JOB_CURSOR — Cursor del job global
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_WAL_H
#define TUXRT_WAL_H

#include "../../sdk/include/tuxrt_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  Tipos de entrada WAL
 * ============================================================================ */

/**
 * Tipos de entrada WAL según especificación KARES
 * 11 tipos definidos en el informe completo
 */
typedef enum {
    WAL_AXIS_JOINT_UPDATE = 0,      /* Eje: posición, velocidad, aceleración */
    WAL_TASK_POSE_UPDATE = 1,       /* Herramienta: pose en task space */
    WAL_CURSOR_UPDATE = 2,          /* Cursor G-code: line_number, param_offset */
    WAL_FEEDRATE_UPDATE = 3,        /* Feedrate y override */
    WAL_SPINDLE_UPDATE = 4,         /* Husillo: rpm, dir, coolant */
    WAL_LASER_UPDATE = 5,           /* Láser: power_pct, mode */
    WAL_EXTRUDER_UPDATE = 6,        /* Extrusor: e_position, pressure_adv, flow */
    WAL_TOOL_CHANGE = 7,            /* Cambio de herramienta */
    WAL_COORD_SYSTEM_CHANGE = 8,    /* Cambio de sistema de coordenadas (WCS, G92) */
    WAL_HOMING_STATE = 9,           /* Estado de homing */
    WAL_JOB_CURSOR = 10             /* Cursor del job global */
} wal_entry_type_t;

/**
 * Estructura de entrada WAL (en memoria)
 */
typedef struct {
    uint64_t sequence;
    uint32_t type;
    tuxrt_timestamp_ns_t timestamp;
    char* key;
    char* value;
    uint32_t crc32;
} tuxrt_wal_entry_t;

/**
 * Contexto WAL
 */
typedef struct tuxrt_wal_t {
    int fd;
    char* path;
    uint64_t sequence;
    void* mutex;
    size_t max_segment_bytes;
    char* segment_path;
    uint64_t entries_written;
    uint64_t entries_replayed;
    bool group_commit_enabled;
} tuxrt_wal_t;

/* ============================================================================
 *  Estructuras específicas por tipo
 * ============================================================================ */

/**
 * Entrada WAL_AXIS_JOINT_UPDATE
 * Eje: posición, velocidad, aceleración
 */
typedef struct {
    uint32_t axis_id;
    double joint_position;
    double velocity;
    double accel;
} wal_axis_joint_t;

/**
 * Entrada WAL_TASK_POSE_UPDATE
 * Herramienta: pose en task space
 */
typedef struct {
    uint32_t tool_id;
    double task_position[6];
    double velocity[6];
} wal_task_pose_t;

/**
 * Entrada WAL_CURSOR_UPDATE
 * Cursor G-code
 */
typedef struct {
    uint32_t tool_id;
    uint32_t line_number;
    double param_offset;
} wal_cursor_t;

/**
 * Entrada WAL_FEEDRATE_UPDATE
 */
typedef struct {
    double feedrate;
    double override_pct;
} wal_feedrate_t;

/**
 * Entrada WAL_SPINDLE_UPDATE
 */
typedef struct {
    double rpm;
    uint8_t dir;
    uint8_t coolant;
} wal_spindle_t;

/**
 * Entrada WAL_LASER_UPDATE
 */
typedef struct {
    double power_pct;
    uint8_t mode;
} wal_laser_t;

/**
 * Entrada WAL_EXTRUDER_UPDATE
 */
typedef struct {
    uint32_t tool_id;
    double e_position;
    double pressure_adv;
    double flow;
    uint8_t retract_state;
} wal_extruder_t;

/**
 * Entrada WAL_TOOL_CHANGE
 */
typedef struct {
    uint32_t from_tool;
    uint32_t to_tool;
    uint8_t state;
} wal_tool_change_t;

/**
 * Entrada WAL_COORD_SYSTEM_CHANGE
 */
typedef struct {
    uint32_t wcs_idx;
    double offset[6];
    double g92[6];
} wal_coord_change_t;

/**
 * Entrada WAL_HOMING_STATE
 */
typedef struct {
    uint32_t axis_mask;
    uint8_t state;
} wal_homing_t;

/**
 * Entrada WAL_JOB_CURSOR
 */
typedef struct {
    uint64_t job_id;
    uint32_t global_line;
    uint32_t global_layer;
} wal_job_cursor_t;

/* ============================================================================
 *  API Pública
 * ============================================================================ */

/**
 * Crear contexto WAL
 *
 * @param path Ruta del archivo WAL
 * @param max_segment_mb Tamaño máximo del segmento en MB
 * @param out_wal Contexto resultante
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_create(
    const char* path,
    size_t max_segment_mb,
    tuxrt_wal_t** out_wal
);

/**
 * Destruir contexto WAL
 *
 * @param wal Contexto WAL
 */
void tuxrt_wal_destroy(tuxrt_wal_t* wal);

/**
 * Añadir entrada WAL genérica
 *
 * @param wal Contexto WAL
 * @param type Tipo de entrada
 * @param key Clave (opcional)
 * @param value Valor (opcional)
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append(
    tuxrt_wal_t* wal,
    uint32_t type,
    const char* key,
    const char* value
);

/**
 * Añadir entrada WAL_AXIS_JOINT_UPDATE
 *
 * @param wal Contexto WAL
 * @param axis_id ID del eje
 * @param position Posición
 * @param velocity Velocidad
 * @param accel Aceleración
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_axis_joint(
    tuxrt_wal_t* wal,
    uint32_t axis_id,
    double position,
    double velocity,
    double accel
);

/**
 * Añadir entrada WAL_TASK_POSE_UPDATE
 *
 * @param wal Contexto WAL
 * @param tool_id ID de herramienta
 * @param task_position Posición en task space [6]
 * @param velocity Velocidad [6]
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_task_pose(
    tuxrt_wal_t* wal,
    uint32_t tool_id,
    const double task_position[6],
    const double velocity[6]
);

/**
 * Añadir entrada WAL_CURSOR_UPDATE
 *
 * @param wal Contexto WAL
 * @param tool_id ID de herramienta
 * @param line_number Línea G-code
 * @param param_offset Offset de parámetro (0.0..1.0)
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_cursor(
    tuxrt_wal_t* wal,
    uint32_t tool_id,
    uint32_t line_number,
    double param_offset
);

/**
 * Añadir entrada WAL_FEEDRATE_UPDATE
 *
 * @param wal Contexto WAL
 * @param feedrate Feedrate
 * @param override_pct Override (%)
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_feedrate(
    tuxrt_wal_t* wal,
    double feedrate,
    double override_pct
);

/**
 * Añadir entrada WAL_SPINDLE_UPDATE
 *
 * @param wal Contexto WAL
 * @param rpm RPM
 * @param dir Dirección
 * @param coolant Refrigerante
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_spindle(
    tuxrt_wal_t* wal,
    double rpm,
    uint8_t dir,
    uint8_t coolant
);

/**
 * Añadir entrada WAL_LASER_UPDATE
 *
 * @param wal Contexto WAL
 * @param power_pct Potencia (%)
 * @param mode Modo
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_laser(
    tuxrt_wal_t* wal,
    double power_pct,
    uint8_t mode
);

/**
 * Añadir entrada WAL_EXTRUDER_UPDATE
 *
 * @param wal Contexto WAL
 * @param tool_id ID de herramienta
 * @param e_position Posición e
 * @param pressure_adv Pressure advance
 * @param flow Tasa de flujo
 * @param retract_state Estado de retracción
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_extruder(
    tuxrt_wal_t* wal,
    uint32_t tool_id,
    double e_position,
    double pressure_adv,
    double flow,
    uint8_t retract_state
);

/**
 * Añadir entrada WAL_TOOL_CHANGE
 *
 * @param wal Contexto WAL
 * @param from_tool Herramienta origen
 * @param to_tool Herramienta destino
 * @param state Estado
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_tool_change(
    tuxrt_wal_t* wal,
    uint32_t from_tool,
    uint32_t to_tool,
    uint8_t state
);

/**
 * Añadir entrada WAL_COORD_SYSTEM_CHANGE
 *
 * @param wal Contexto WAL
 * @param wcs_idx Índice WCS
 * @param offset Offset [6]
 * @param g92 Offset G92 [6]
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_coord_change(
    tuxrt_wal_t* wal,
    uint32_t wcs_idx,
    const double offset[6],
    const double g92[6]
);

/**
 * Añadir entrada WAL_HOMING_STATE
 *
 * @param wal Contexto WAL
 * @param axis_mask Máscara de ejes
 * @param state Estado
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_homing(
    tuxrt_wal_t* wal,
    uint32_t axis_mask,
    uint8_t state
);

/**
 * Añadir entrada WAL_JOB_CURSOR
 *
 * @param wal Contexto WAL
 * @param job_id ID del job
 * @param global_line Línea global
 * @param global_layer Capa global
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_append_job_cursor(
    tuxrt_wal_t* wal,
    uint64_t job_id,
    uint32_t global_line,
    uint32_t global_layer
);

/**
 * Replay WAL desde secuencia inicial
 *
 * @param wal Contexto WAL
 * @param start_seq Secuencia inicial
 * @param fn Callback para procesar entradas
 * @param user_data Datos de usuario
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_replay(
    tuxrt_wal_t* wal,
    uint64_t start_seq,
    tuxrt_err_t (*fn)(const tuxrt_wal_entry_t* entry, void* user_data),
    void* user_data
);

/**
 * Commit de grupo (flush)
 *
 * @param wal Contexto WAL
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_checkpoint(tuxrt_wal_t* wal);

/**
 * Habilitar/deshabilitar group commit
 *
 * @param wal Contexto WAL
 * @param enabled Habilitar
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_set_group_commit(
    tuxrt_wal_t* wal,
    bool enabled
);

/**
 * Verificar integridad del WAL
 *
 * @param wal Contexto WAL
 * @param out_ok Salida: true si está íntegro
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_verify(
    tuxrt_wal_t* wal,
    bool* out_ok
);

/**
 * Obtener estadísticas del WAL
 *
 * @param wal Contexto WAL
 * @param out_entries_written Entradas escritas
 * @param out_entries_replayed Entradas replayeadas
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_wal_get_stats(
    const tuxrt_wal_t* wal,
    uint64_t* out_entries_written,
    uint64_t* out_entries_replayed
);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_WAL_H */
