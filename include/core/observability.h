/**
 * observability.h — API de Observabilidad y Métricas (TRT-19)
 *
 * Definiciones de API para métricas de operaciones y notificaciones OTLP.
 * Incluye alertas I-III basadas en métricas:
 *   - Tráfico de red (ETH0/1/USB)
 *   - Actividad de hilos (timers, workqueues, epoll)
 *   - Uso de CPU per-thread
 *   - Temperatura CPU / GPU (thermal zones)
 *   - Entrada/salida de archivos (frecuencia, tamaño, retardo)
 *   - Uso de GPU (memoria, shader util, tile fills)
 *   - IOPS (SSD/NVMe, SATA)
 *   - GPU mem, recuento de G0/65k, retardo de red
 *   - HWD actualizaciones por interrupt, timestamp de eventos
 */

#ifndef TUXRT_OBSERVABILITY_H
#define TUXRT_OBSERVABILITY_H

#include "../../sdk/include/tuxrt_sdk.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Tipos de datos para métricas (OTLP exportable)
// =============================================================================

typedef struct {
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t packets_in;
    uint64_t packets_out;
    uint32_t drops;
    uint32_t errors;
    uint32_t usage_percent;
} tuxrt_network_metrics_t;

typedef struct {
    uint64_t timer_ticks;
    uint64_t workqueue_runs;
    uint64_t epoll_events;
    uint64_t context_switches;
    uint32_t thread_count;
    uint32_t cpu_percent;
    uint32_t quantum_overruns;
} tuxrt_thread_metrics_t;

typedef struct {
    uint32_t current_temp_celsius;
    uint32_t max_temp_celsius;
    uint32_t thermal_events;
    uint32_t throttles;
    bool is_thermal_guard;
} tuxrt_thermal_metrics_t;

typedef struct {
    uint64_t read_count;
    uint64_t write_count;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint32_t avg_read_us;
    uint32_t avg_write_us;
    uint32_t avg_io_percent;
} tuxrt_fileio_metrics_t;

typedef struct {
    uint64_t memory_used_bytes;
    uint64_t memory_total_bytes;
    uint32_t gpu_temp_celsius;
    uint32_t shader_util_percent;
    uint32_t tile_fills_per_sec;
    uint32_t vrams_busy_bytes;
    uint32_t frequency_mhz;
} tuxrt_gpu_metrics_t;

typedef struct {
    uint64_t read_ops_per_sec;
    uint64_t write_ops_per_sec;
    uint64_t read_mbytes_per_sec;
    uint64_t write_mbytes_per_sec;
    uint32_t queue_depth;
    uint32_t block_size_kb;
    bool is_ssd;
    bool is_nvme;
} tuxrt_storage_metrics_t;

typedef struct {
    uint64_t g0_count;
    uint64_t count_65k;
    uint64_t timestep_ns;
    uint32_t error_count;
    uint32_t warning_count;
    uint64_t max_error_rate;
} tuxrt_kinematics_metrics_t;

typedef struct {
    uint64_t ping_us;
    uint64_t jitter_us;
    uint64_t packet_loss_percent;
    uint32_t rt_delay_max_us;
    uint32_t rt_delay_avg_us;
    int64_t clock_drift_ns_per_sec;
} tuxrt_network_quality_t;

typedef struct {
    uint64_t interrupt_count;
    uint64_t interrupt_rate_hz;
    uint64_t update_time_ns;
    uint32_t softirq_time_total_ns;
} tuxrt_hwd_metrics_t;

typedef struct {
    uint64_t timestamp_ns;
    uint32_t event_id;
    uint32_t thread_id;
    uint32_t cpu_id;
    uint64_t cpu_cycles;
    uint64_t ll_cache_misses;
    uint64_t dcache_loads;
    uint64_t branches_taken;
    uint64_t branches_mispredicted;
} tuxrt_perf_event_t;

// =============================================================================
// Tipos de configuración (deben estar antes de las funciones que los usan)
// =============================================================================

typedef struct tuxrt_observability_ctx tuxrt_observability_ctx_t;
typedef uint64_t tuxrt_alert_id_t;

typedef struct tuxrt_metrics_snapshot {
    uint64_t timestamp_ns;
    tuxrt_network_metrics_t network;
    tuxrt_thread_metrics_t thread;
    tuxrt_thermal_metrics_t thermal;
    tuxrt_fileio_metrics_t fileio;
    tuxrt_gpu_metrics_t gpu;
    tuxrt_storage_metrics_t storage;
    tuxrt_kinematics_metrics_t kinematics;
    tuxrt_network_quality_t network_quality;
    tuxrt_hwd_metrics_t hwd;
    uint32_t perf_event_count;
    tuxrt_perf_event_t* perf_events;
} tuxrt_metrics_snapshot_t;

typedef struct tuxrt_alert_context {
    tuxrt_alert_id_t id;
    const char* name;
    uint64_t timestamp_ns;
    tuxrt_metrics_snapshot_t* snapshot;
    uint32_t metric_value;
    const char* condition;
} tuxrt_alert_context_t;

typedef struct tuxrt_observability_opts {
    uint32_t collection_interval_ms;
    bool enable_perf_counters;
    bool enable_perf_events;
    char storage_path[256];
    uint32_t max_metric_points;
    char otel_endpoint[256];
    uint32_t otel_timeout_ms;
    bool enable_network_metrics;
    bool enable_thread_metrics;
    bool enable_thermal_metrics;
    bool enable_fileio_metrics;
    bool enable_gpu_metrics;
    bool enable_storage_metrics;
    bool enable_kinematics_metrics;
    bool enable_network_quality;
    bool enable_hwd_metrics;
    bool enable_perf_events_metrics;
} tuxrt_observability_opts_t;

typedef struct tuxrt_alert_opts {
    uint32_t check_interval_ms;
    uint32_t severity; // 0=info, 1=warning, 2=error, 3=fatal
    bool is_persistent;
    char description[256];
} tuxrt_alert_opts_t;

// =============================================================================
// Callbacks
// =============================================================================

// Forward declarations for callbacks
typedef bool (*tuxrt_alert_condition_fn_t)(const tuxrt_metrics_snapshot_t*);
typedef void (*tuxrt_alert_callback_fn_t)(const tuxrt_alert_context_t*);

// =============================================================================
// API principal
// =============================================================================

/**
 * Inicializar el colector de observabilidad.
 */
tuxrt_err_t tuxrt_observability_init(const tuxrt_observability_opts_t* opts);

/**
 * Iniciar recolección de métricas en tiempo real.
 */
tuxrt_err_t tuxrt_observability_start(tuxrt_observability_ctx_t* ctx);

/**
 * Detener la recolección de métricas.
 */
tuxrt_err_t tuxrt_observability_stop(tuxrt_observability_ctx_t* ctx);

/**
 * Destruir contexto de observabilidad.
 */
void tuxrt_observability_destroy(tuxrt_observability_ctx_t* ctx);

/**
 * Obtener métricas actuales en JSON OTLP para exportación.
 */
tuxrt_err_t tuxrt_observability_get_metrics_json(tuxrt_observability_ctx_t* ctx, char* out_json, size_t out_len);

/**
 * Enviar métricas a backend OTLP.
 */
tuxrt_err_t tuxrt_observability_send_metrics(tuxrt_observability_ctx_t* ctx, const char* endpoint, uint32_t timeout_ms);

/**
 * Crear alerta basada en umbral.
 */
tuxrt_err_t tuxrt_observability_create_alert(const char* name,
                                             tuxrt_alert_condition_fn_t condition,
                                             tuxrt_alert_callback_fn_t callback,
                                             const tuxrt_alert_opts_t* opts);

/**
 * Eliminar una alerta.
 */
tuxrt_err_t tuxrt_observability_delete_alert(tuxrt_alert_id_t alert_id);

/**
 * Poblar snapshot de métricas actual.
 */
tuxrt_err_t tuxrt_observability_populate_snapshot(tuxrt_observability_ctx_t* ctx,
                                                 tuxrt_metrics_snapshot_t** out_snapshot);

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif // TUXRT_OBSERVABILITY_H