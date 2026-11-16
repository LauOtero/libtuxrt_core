/**
 * job_queue.h — Job Queue Recovery con 7 tablas SQLite
 *
 * Sistema de gestión de jobs con recuperación tras fallo:
 *   1. tq_job_queue — Cola de jobs
 *   2. tq_axis_state — Estado de ejes por job
 *   3. tq_tool_state — Estado de herramientas por job
 *   4. tq_cursor_state — Cursores multi-toolhead
 *   5. tq_checkpoint — Checkpoints USS
 *   6. tq_wal_index — Índice de WAL por job
 *   7. tq_recovery_log — Log de recuperación KARES
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_JOB_QUEUE_H
#define TUXRT_JOB_QUEUE_H

#include "../../sdk/include/tuxrt_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  Estructuras
 * ============================================================================ */

/**
 * Estado de un job
 */
typedef enum {
    JOB_STATE_PENDING = 0,
    JOB_STATE_RUNNING = 1,
    JOB_STATE_PAUSED = 2,
    JOB_STATE_COMPLETED = 3,
    JOB_STATE_FAILED = 4,
    JOB_STATE_RECOVERING = 5,
    JOB_STATE_CANCELLED = 6
} job_state_t;

/**
 * Información de job
 */
typedef struct {
    uint64_t job_id;
    char gcode_path[512];
    uint32_t total_lines;
    uint32_t current_line;
    uint32_t total_layers;
    uint32_t current_layer;
    job_state_t state;
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    uint32_t tool_count;
    uint32_t axis_count;
    char metadata[1024];
} tuxrt_job_info_t;

/**
 * Estado de eje en job
 */
typedef struct {
    uint64_t job_id;
    uint32_t axis_id;
    double joint_position;
    double target_position;
    double velocity;
    double accel;
    bool homed;
    bool fault;
} tuxrt_job_axis_t;

/**
 * Estado de herramienta en job
 */
typedef struct {
    uint64_t job_id;
    uint32_t tool_id;
    uint32_t line_number;
    uint32_t layer_number;
    double param_offset;
    double task_position[6];
    double e_position;
    uint8_t state;
} tuxrt_job_tool_t;

/**
 * Contexto de job queue
 */
typedef struct tuxrt_job_queue_t {
    void* db_handle;
    char* db_path;
    bool initialized;
} tuxrt_job_queue_t;

/* ============================================================================
 *  API Pública
 * ============================================================================ */

/**
 * Crear job queue
 *
 * @param db_path Ruta a base de datos SQLite
 * @param out_queue Contexto resultante
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_create(
    const char* db_path,
    tuxrt_job_queue_t** out_queue
);

/**
 * Inicializar 7 tablas necesarias
 *
 * @param queue Contexto de job queue
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_init_tables(tuxrt_job_queue_t* queue);

/**
 * Crear nuevo job
 *
 * @param queue Contexto de job queue
 * @param gcode_path Ruta al archivo G-code
 * @param metadata Metadatos
 * @param out_job_id ID del job creado
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_create_job(
    tuxrt_job_queue_t* queue,
    const char* gcode_path,
    const char* metadata,
    uint64_t* out_job_id
);

/**
 * Actualizar estado de job
 *
 * @param queue Contexto de job queue
 * @param job_id ID del job
 * @param state Nuevo estado
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_update_state(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    job_state_t state
);

/**
 * Actualizar cursor de job
 *
 * @param queue Contexto de job queue
 * @param job_id ID del job
 * @param line_number Línea actual
 * @param layer_number Capa actual
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_update_cursor(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    uint32_t line_number,
    uint32_t layer_number
);

/**
 * Guardar estado de ejes para job
 *
 * @param queue Contexto de job queue
 * @param job_id ID del job
 * @param axes Array de estados de ejes
 * @param axis_count Número de ejes
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_save_axis_states(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    const tuxrt_job_axis_t* axes,
    uint32_t axis_count
);

/**
 * Guardar estado de herramientas para job
 *
 * @param queue Contexto de job queue
 * @param job_id ID del job
 * @param tools Array de estados de herramientas
 * @param tool_count Número de herramientas
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_save_tool_states(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    const tuxrt_job_tool_t* tools,
    uint32_t tool_count
);

/**
 * Recuperar job interrumpido
 *
 * @param queue Contexto de job queue
 * @param job_id ID del job
 * @param out_info Información del job
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_recover_job(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    tuxrt_job_info_t* out_info
);

/**
 * Listar jobs interrumpidos
 *
 * @param queue Contexto de job queue
 * @param out_job_ids Array de IDs de jobs
 * @param max_count Máximo número de jobs
 * @param out_count Número real de jobs
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_list_interrupted(
    tuxrt_job_queue_t* queue,
    uint64_t* out_job_ids,
    uint32_t max_count,
    uint32_t* out_count
);

/**
 * Eliminar job completado
 *
 * @param queue Contexto de job queue
 * @param job_id ID del job
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_delete_job(
    tuxrt_job_queue_t* queue,
    uint64_t job_id
);

/**
 * Ejecutar migraciones
 *
 * @param queue Contexto de job queue
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_job_queue_migrate(tuxrt_job_queue_t* queue);

/**
 * Destruir job queue
 *
 * @param queue Contexto de job queue
 */
void tuxrt_job_queue_destroy(tuxrt_job_queue_t* queue);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_JOB_QUEUE_H */
