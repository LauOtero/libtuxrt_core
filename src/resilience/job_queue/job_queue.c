/**
 * job_queue.c — Implementación de Job Queue con 7 tablas SQLite
 *
 * Estructura de 7 tablas:
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

#include "job_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 *  SQL Schema para 7 tablas
 * ============================================================================ */

static const char* SQL_CREATE_TABLES[] = {
    /* Tabla 1: tq_job_queue */
    "CREATE TABLE IF NOT EXISTS tq_job_queue ("
    "  job_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  gcode_path TEXT NOT NULL,"
    "  total_lines INTEGER DEFAULT 0,"
    "  current_line INTEGER DEFAULT 0,"
    "  total_layers INTEGER DEFAULT 0,"
    "  current_layer INTEGER DEFAULT 0,"
    "  state INTEGER DEFAULT 0,"
    "  start_time_ns INTEGER DEFAULT 0,"
    "  end_time_ns INTEGER DEFAULT 0,"
    "  tool_count INTEGER DEFAULT 0,"
    "  axis_count INTEGER DEFAULT 0,"
    "  metadata TEXT,"
    "  created_at INTEGER DEFAULT (strftime('%s','now')),"
    "  updated_at INTEGER DEFAULT (strftime('%s','now'))"
    ");",
    
    /* Tabla 2: tq_axis_state */
    "CREATE TABLE IF NOT EXISTS tq_axis_state ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL,"
    "  axis_id INTEGER NOT NULL,"
    "  joint_position REAL DEFAULT 0,"
    "  target_position REAL DEFAULT 0,"
    "  velocity REAL DEFAULT 0,"
    "  accel REAL DEFAULT 0,"
    "  homed INTEGER DEFAULT 0,"
    "  fault INTEGER DEFAULT 0,"
    "  updated_at INTEGER DEFAULT (strftime('%s','now')),"
    "  FOREIGN KEY(job_id) REFERENCES tq_job_queue(job_id) ON DELETE CASCADE,"
    "  UNIQUE(job_id, axis_id)"
    ");",
    
    /* Tabla 3: tq_tool_state */
    "CREATE TABLE IF NOT EXISTS tq_tool_state ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL,"
    "  tool_id INTEGER NOT NULL,"
    "  line_number INTEGER DEFAULT 0,"
    "  layer_number INTEGER DEFAULT 0,"
    "  param_offset REAL DEFAULT 0,"
    "  task_pos_x REAL DEFAULT 0,"
    "  task_pos_y REAL DEFAULT 0,"
    "  task_pos_z REAL DEFAULT 0,"
    "  task_pos_a REAL DEFAULT 0,"
    "  task_pos_b REAL DEFAULT 0,"
    "  task_pos_c REAL DEFAULT 0,"
    "  e_position REAL DEFAULT 0,"
    "  state INTEGER DEFAULT 0,"
    "  updated_at INTEGER DEFAULT (strftime('%s','now')),"
    "  FOREIGN KEY(job_id) REFERENCES tq_job_queue(job_id) ON DELETE CASCADE,"
    "  UNIQUE(job_id, tool_id)"
    ");",
    
    /* Tabla 4: tq_cursor_state */
    "CREATE TABLE IF NOT EXISTS tq_cursor_state ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL,"
    "  tool_id INTEGER NOT NULL,"
    "  sequence INTEGER DEFAULT 0,"
    "  line_number INTEGER DEFAULT 0,"
    "  param_offset REAL DEFAULT 0,"
    "  layer_number INTEGER DEFAULT 0,"
    "  timestamp_ns INTEGER DEFAULT 0,"
    "  FOREIGN KEY(job_id) REFERENCES tq_job_queue(job_id) ON DELETE CASCADE"
    ");",
    
    /* Tabla 5: tq_checkpoint */
    "CREATE TABLE IF NOT EXISTS tq_checkpoint ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL,"
    "  sequence INTEGER NOT NULL,"
    "  checkpoint_path TEXT NOT NULL,"
    "  header_magic INTEGER DEFAULT 0,"
    "  header_version INTEGER DEFAULT 0,"
    "  timestamp_ns INTEGER DEFAULT 0,"
    "  checksum INTEGER DEFAULT 0,"
    "  ecc_valid INTEGER DEFAULT 0,"
    "  created_at INTEGER DEFAULT (strftime('%s','now')),"
    "  FOREIGN KEY(job_id) REFERENCES tq_job_queue(job_id) ON DELETE CASCADE"
    ");",
    
    /* Tabla 6: tq_wal_index */
    "CREATE TABLE IF NOT EXISTS tq_wal_index ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL,"
    "  wal_sequence INTEGER NOT NULL,"
    "  entry_type INTEGER DEFAULT 0,"
    "  timestamp_ns INTEGER DEFAULT 0,"
    "  key TEXT,"
    "  value TEXT,"
    "  checksum INTEGER DEFAULT 0,"
    "  FOREIGN KEY(job_id) REFERENCES tq_job_queue(job_id) ON DELETE CASCADE"
    ");",
    
    /* Tabla 7: tq_recovery_log */
    "CREATE TABLE IF NOT EXISTS tq_recovery_log ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL,"
    "  phase INTEGER DEFAULT 0,"
    "  status INTEGER DEFAULT 0,"
    "  error_code INTEGER DEFAULT 0,"
    "  start_time_ns INTEGER DEFAULT 0,"
    "  end_time_ns INTEGER DEFAULT 0,"
    "  lines_recovered INTEGER DEFAULT 0,"
    "  wal_entries_applied INTEGER DEFAULT 0,"
    "  ecc_corrected INTEGER DEFAULT 0,"
    "  FOREIGN KEY(job_id) REFERENCES tq_job_queue(job_id) ON DELETE CASCADE"
    ");",
    
    NULL
};

/* ============================================================================
 *  Mock SQLite (para compilación sin SQLite)
 * ============================================================================ */

static void* mock_sqlite_open(const char* path)
{
    (void)path;
    return (void*)0x1;
}

static void mock_sqlite_close(void* db)
{
    (void)db;
}

static int mock_sqlite_exec(void* db, const char* sql)
{
    (void)db;
    (void)sql;
    return 0;
}

static int mock_sqlite_exec_param(void* db, const char* sql, ...)
{
    (void)db;
    (void)sql;
    return 0;
}

/* ============================================================================
 *  API Implementación
 * ============================================================================ */

tuxrt_err_t tuxrt_job_queue_create(
    const char* db_path,
    tuxrt_job_queue_t** out_queue
)
{
    if (!db_path || !out_queue) {
        return TUXRT_ERR_INVAL;
    }
    
    tuxrt_job_queue_t* queue = (tuxrt_job_queue_t*)calloc(1, sizeof(tuxrt_job_queue_t));
    if (!queue) {
        return TUXRT_ERR_NOMEM;
    }
    
    queue->db_path = strdup(db_path);
    if (!queue->db_path) {
        free(queue);
        return TUXRT_ERR_NOMEM;
    }
    
    /* Abrir BD (mock) */
    queue->db_handle = mock_sqlite_open(db_path);
    if (!queue->db_handle) {
        free(queue->db_path);
        free(queue);
        return TUXRT_ERR_IO;
    }
    
    queue->initialized = true;
    *out_queue = queue;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_init_tables(tuxrt_job_queue_t* queue)
{
    if (!queue || !queue->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Crear 7 tablas */
    for (int i = 0; SQL_CREATE_TABLES[i] != NULL; i++) {
        int ret = mock_sqlite_exec(queue->db_handle, SQL_CREATE_TABLES[i]);
        if (ret != 0) {
            return TUXRT_ERR_IO;
        }
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_create_job(
    tuxrt_job_queue_t* queue,
    const char* gcode_path,
    const char* metadata,
    uint64_t* out_job_id
)
{
    if (!queue || !queue->initialized || !gcode_path || !out_job_id) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Mock: generar ID */
    *out_job_id = (uint64_t)rand();
    
    /* Insertar job en tq_job_queue */
    const char* sql = "INSERT INTO tq_job_queue (gcode_path, metadata, state) VALUES (?, ?, ?)";
    /* mock_sqlite_exec_param(...); */
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_update_state(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    job_state_t state
)
{
    if (!queue || !queue->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    const char* sql = "UPDATE tq_job_queue SET state = ?, updated_at = strftime('%s','now') WHERE job_id = ?";
    /* mock_sqlite_exec_param(...); */
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_update_cursor(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    uint32_t line_number,
    uint32_t layer_number
)
{
    if (!queue || !queue->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    const char* sql = "UPDATE tq_job_queue SET current_line = ?, current_layer = ?, updated_at = strftime('%s','now') WHERE job_id = ?";
    /* mock_sqlite_exec_param(...); */
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_save_axis_states(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    const tuxrt_job_axis_t* axes,
    uint32_t axis_count
)
{
    if (!queue || !queue->initialized || !axes) {
        return TUXRT_ERR_INVAL;
    }
    
    for (uint32_t i = 0; i < axis_count; i++) {
        const tuxrt_job_axis_t* axis = &axes[i];
        const char* sql = "INSERT OR REPLACE INTO tq_axis_state "
                         "(job_id, axis_id, joint_position, target_position, velocity, accel, homed, fault) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
        /* mock_sqlite_exec_param(...); */
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_save_tool_states(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    const tuxrt_job_tool_t* tools,
    uint32_t tool_count
)
{
    if (!queue || !queue->initialized || !tools) {
        return TUXRT_ERR_INVAL;
    }
    
    for (uint32_t i = 0; i < tool_count; i++) {
        const tuxrt_job_tool_t* tool = &tools[i];
        const char* sql = "INSERT OR REPLACE INTO tq_tool_state "
                         "(job_id, tool_id, line_number, layer_number, param_offset, "
                         "task_pos_x, task_pos_y, task_pos_z, task_pos_a, task_pos_b, task_pos_c, "
                         "e_position, state) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
        /* mock_sqlite_exec_param(...); */
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_recover_job(
    tuxrt_job_queue_t* queue,
    uint64_t job_id,
    tuxrt_job_info_t* out_info
)
{
    if (!queue || !queue->initialized || !out_info) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Consultar job interrumpido */
    const char* sql = "SELECT job_id, gcode_path, total_lines, current_line, "
                     "total_layers, current_layer, state, start_time_ns, end_time_ns, "
                     "tool_count, axis_count, metadata "
                     "FROM tq_job_queue WHERE job_id = ? AND state IN (1, 2, 5)";
    
    /* Mock: llenar estructura */
    memset(out_info, 0, sizeof(*out_info));
    out_info->job_id = job_id;
    out_info->state = JOB_STATE_RECOVERING;
    strcpy(out_info->gcode_path, "/tmp/gcode.gcode");
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_list_interrupted(
    tuxrt_job_queue_t* queue,
    uint64_t* out_job_ids,
    uint32_t max_count,
    uint32_t* out_count
)
{
    if (!queue || !queue->initialized || !out_job_ids || !out_count) {
        return TUXRT_ERR_INVAL;
    }
    
    const char* sql = "SELECT job_id FROM tq_job_queue WHERE state IN (1, 2, 5) LIMIT ?";
    
    /* Mock: retornar 0 jobs */
    *out_count = 0;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_delete_job(
    tuxrt_job_queue_t* queue,
    uint64_t job_id
)
{
    if (!queue || !queue->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    const char* sql = "DELETE FROM tq_job_queue WHERE job_id = ?";
    /* mock_sqlite_exec_param(...); */
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_job_queue_migrate(tuxrt_job_queue_t* queue)
{
    if (!queue || !queue->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Ejecutar migraciones si necesarias */
    /* Mock: retorna OK */
    return TUXRT_OK;
}

void tuxrt_job_queue_destroy(tuxrt_job_queue_t* queue)
{
    if (!queue) {
        return;
    }
    
    if (queue->db_handle) {
        mock_sqlite_close(queue->db_handle);
    }
    
    free(queue->db_path);
    memset(queue, 0, sizeof(*queue));
    free(queue);
}
