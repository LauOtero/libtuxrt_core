/**
 * tuxrt_sdk.h — C-ABI Estable para TuxRT
 * 
 * Este header define la interfaz binaria estable entre:
 * - CORE (C17): libtuxrt_core.so
 * - Mojo (Mojo 1.1): FFI bindings
 * - SDK Bridge: OwnedDLHandle, KPLUG loader, Python wrappers
 * - Plugins externos (KPLUG)
 * 
 * VERSION: 0.1.0-alpha.1
 * REGLA: Una vez publicado, NUNCA romper ABI. Añadir solo al final.
 *        Cambios breaking → nueva versión mayor + header versionado (tuxrt_sdk_v2.h)
 * 
 * Convenciones:
 * - Tipos opacos: struct tuxrt_*_t; // forward declaration
 * - Constructores: tuxrt_*_create() → devuelve puntero o NULL + errno
 * - Destructores: tuxrt_*_destroy(ptr) → void, NULL-safe
 * - Errores: int return (0=OK, <0=errno), salida por out-param
 * - Ownership: caller-owns en create, callee-owns en destroy
 * - Thread-safety: documentado por función
 * - Alineación: 64-byte para structs hot-path (cache-line)
 */

#ifndef TUXRT_SDK_H
#define TUXRT_SDK_H

#if !defined(_GNU_SOURCE)
  #define _GNU_SOURCE
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>

// Contrato universal de proveedor (TuxRT Provider ABI): símbolo descriptor que
// exporta todo proveedor externo (p. ej. ChronosRT). Autocontenido.
#include "tuxrt_provider.h"

// =============================================================================
// Macro de exportación (visibilidad) — DEBE ir ANTES de cualquier uso
// =============================================================================

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef TUXRT_CORE_EXPORTS
    #define TUXRT_SDK_API __declspec(dllexport)
  #else
    #define TUXRT_SDK_API __declspec(dllimport)
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define TUXRT_SDK_API __attribute__((visibility("default")))
  #else
    #define TUXRT_SDK_API
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Versionado y metadatos
// =============================================================================

#define TUXRT_SDK_VERSION_MAJOR 0
#define TUXRT_SDK_VERSION_MINOR 1
#define TUXRT_SDK_VERSION_PATCH 0
#define TUXRT_SDK_VERSION_STRING "0.1.0-alpha.1"

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    const char* string;        // "0.1.0-alpha.1"
    const char* git_commit;    // short SHA
    const char* build_date;    // ISO 8601
    const char* mojo_version;  // "1.1.0"
} tuxrt_sdk_version_t;

TUXRT_SDK_API const tuxrt_sdk_version_t* tuxrt_sdk_version(void);

// =============================================================================
// Códigos de error (subconjunto de errno + específicos TuxRT)
// =============================================================================

typedef enum {
    TUXRT_OK                    = 0,
    TUXRT_ERR_NOMEM             = -1,   // ENOMEM
    TUXRT_ERR_INVAL             = -2,   // EINVAL
    TUXRT_ERR_NOENT             = -3,   // ENOENT
    TUXRT_ERR_EXIST             = -4,   // EEXIST
    TUXRT_ERR_BUSY              = -5,   // EBUSY
    TUXRT_ERR_AGAIN             = -6,   // EAGAIN
    TUXRT_ERR_NOSYS             = -7,   // ENOSYS
    TUXRT_ERR_PERM              = -8,   // EPERM
    TUXRT_ERR_IO                = -9,   // EIO
    TUXRT_ERR_TIMEDOUT          = -10,  // ETIMEDOUT
    TUXRT_ERR_PROTO             = -11,  // EPROTO
    TUXRT_ERR_OVERFLOW          = -12,  // EOVERFLOW
    
    // Específicos TuxRT (negativos, no colisionan con errno)
    TUXRT_ERR_NOT_INITIALIZED   = -1000,
    TUXRT_ERR_ALREADY_RUNNING   = -1001,
    TUXRT_ERR_CONFIG_INVALID    = -1002,
    TUXRT_ERR_HARDWARE_FAULT    = -1003,
    TUXRT_ERR_RT_VIOLATION      = -1004,  // Deadline miss, priority inversion
    TUXRT_ERR_SYNC_LOST         = -1005,  // PHSYNC drift > threshold
    TUXRT_ERR_PLUGIN_LOAD       = -1006,
    TUXRT_ERR_PLUGIN_API_MISMATCH = -1007,
    TUXRT_ERR_KINEMATICS_SINGULAR = -1008,
    TUXRT_ERR_MOTION_QUEUE_FULL = -1009,
    TUXRT_ERR_WAL_CORRUPT       = -1010,
    TUXRT_ERR_I18N_MODEL_MISSING = -1011,
    TUXRT_ERR_MTLS_HANDSHAKE    = -1012,
    TUXRT_ERR_INTERRUPTED      = -1013,
    TUXRT_ERR_FILE_NOT_FOUND    = -1014,  // File not found
    TUXRT_ERR_ECC_UNCORRECTABLE = -1015,  // Unrecoverable ECC error
    TUXRT_ERR_FILE_READ         = -1016,  // Failed to read file
    TUXRT_ERR_INVALID_MAGIC     = -1017,  // Invalid format magic / header
    TUXRT_ERR_CRC_MISMATCH      = -1018,  // CRC / integrity mismatch
    TUXRT_ERR_FILE_OPEN         = -1019,  // Failed to open file
    TUXRT_ERR_FILE_WRITE        = -1020,  // Failed to write file
    TUXRT_ERR_FILE_SYNC         = -1021,  // Failed to sync/flush file
    TUXRT_ERR_FILE_SIZE         = -1022,  // Unexpected file size
    TUXRT_ERR_VERSION_MISMATCH  = -1023,  // Checkpoint/format version mismatch
    TUXRT_ERR_INVALID_STATE     = -1024,  // Operation invalid for current state
    TUXRT_ERR_RETRY             = -1025,  // Transient failure, retry advised
    TUXRT_ERR_TEMP_DRIFT        = -1026,  // Thermal drift beyond threshold
    // Dependencia opcional ausente (p. ej. ChronosRT para el transporte EtherCAT).
    // Valor explícito: se añade sin renumerar los existentes (ABI intacta).
    TUXRT_ERR_DEPENDENCY_MISSING = -1027, // Optional dependency not installed
    // EtherTux: control de admisión / política best-effort (se añaden al final
    // para no renumerar los códigos existentes; ABI intacta).
    TUXRT_ERR_BACKPRESSURE      = -1028,  // Producer must reduce its rate
    TUXRT_ERR_DROPPED           = -1029,  // Frame dropped by best-effort policy
    TUXRT_ERR_NOT_SUPPORTED     = -1030,  // Operation not supported

    // FFI / panic protection
    TUXRT_ERR_PANIC             = -2000,  // C-side panic caught via setjmp
    TUXRT_ERR_CRITICAL          = -2001,  // Critical failure requiring Safe Mode
    TUXRT_ERR_SIGNAL            = -2002,  // Caught signal (SIGSEGV, SIGBUS, etc.)
    TUXRT_ERR_STACK_OVERFLOW    = -2003,  // Stack overflow detected
} tuxrt_err_t;

// Helpers
static inline const char* tuxrt_strerror(tuxrt_err_t err) {
    switch (err) {
        case TUXRT_OK: return "Success";
        case TUXRT_ERR_NOMEM: return "Out of memory";
        case TUXRT_ERR_INVAL: return "Invalid argument";
        case TUXRT_ERR_NOENT: return "Not found";
        case TUXRT_ERR_EXIST: return "Already exists";
        case TUXRT_ERR_BUSY: return "Resource busy";
        case TUXRT_ERR_AGAIN: return "Try again";
        case TUXRT_ERR_NOSYS: return "Not implemented";
        case TUXRT_ERR_PERM: return "Permission denied";
        case TUXRT_ERR_IO: return "I/O error";
        case TUXRT_ERR_TIMEDOUT: return "Timeout";
        case TUXRT_ERR_PROTO: return "Protocol error";
        case TUXRT_ERR_OVERFLOW: return "Overflow";
        case TUXRT_ERR_NOT_INITIALIZED: return "Not initialized";
        case TUXRT_ERR_ALREADY_RUNNING: return "Already running";
        case TUXRT_ERR_CONFIG_INVALID: return "Invalid configuration";
        case TUXRT_ERR_HARDWARE_FAULT: return "Hardware fault";
        case TUXRT_ERR_RT_VIOLATION: return "Real-time violation";
        case TUXRT_ERR_SYNC_LOST: return "Sync lost";
        case TUXRT_ERR_PLUGIN_LOAD: return "Plugin load failed";
        case TUXRT_ERR_PLUGIN_API_MISMATCH: return "Plugin API mismatch";
        case TUXRT_ERR_KINEMATICS_SINGULAR: return "Kinematics singularity";
        case TUXRT_ERR_MOTION_QUEUE_FULL: return "Motion queue full";
        case TUXRT_ERR_WAL_CORRUPT: return "WAL corruption";
        case TUXRT_ERR_I18N_MODEL_MISSING: return "i18n model missing";
        case TUXRT_ERR_MTLS_HANDSHAKE: return "mTLS handshake failed";
    case TUXRT_ERR_INTERRUPTED: return "Operation interrupted";
    case TUXRT_ERR_FILE_NOT_FOUND: return "File not found";
    case TUXRT_ERR_ECC_UNCORRECTABLE: return "ECC uncorrectable error";
    case TUXRT_ERR_FILE_READ: return "File read failed";
    case TUXRT_ERR_INVALID_MAGIC: return "Invalid magic/header";
    case TUXRT_ERR_CRC_MISMATCH: return "CRC mismatch";
    case TUXRT_ERR_FILE_OPEN: return "File open failed";
    case TUXRT_ERR_FILE_WRITE: return "File write failed";
    case TUXRT_ERR_FILE_SYNC: return "File sync failed";
    case TUXRT_ERR_FILE_SIZE: return "Unexpected file size";
    case TUXRT_ERR_VERSION_MISMATCH: return "Version mismatch";
    case TUXRT_ERR_INVALID_STATE: return "Invalid state";
    case TUXRT_ERR_RETRY: return "Retry advised";
    case TUXRT_ERR_TEMP_DRIFT: return "Thermal drift";
    case TUXRT_ERR_DEPENDENCY_MISSING: return "Required dependency missing";
    case TUXRT_ERR_BACKPRESSURE: return "Backpressure: reduce producer rate";
    case TUXRT_ERR_DROPPED: return "Frame dropped (best-effort)";
    case TUXRT_ERR_NOT_SUPPORTED: return "Operation not supported";
    case TUXRT_ERR_PANIC: return "C panic caught";
        case TUXRT_ERR_CRITICAL: return "Critical failure";
        case TUXRT_ERR_SIGNAL: return "Signal caught";
        case TUXRT_ERR_STACK_OVERFLOW: return "Stack overflow";
        default: return "Unknown error";
    }
}

// =============================================================================
// Error Boundary / FFI Protection (M3b)
// =============================================================================
// Thread-local jump buffer for setjmp/longjmp panic recovery
// Each FFI entry point MUST wrap its body with TUXRT_TRY / TUXRT_CATCH

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for opaque types used in struct tuxrt_error_context_t (must be before it)
typedef struct tuxrt_trapq_t tuxrt_trapq_t;
typedef struct tuxrt_input_shaper_t tuxrt_input_shaper_t;
typedef struct tuxrt_stepcompress_t tuxrt_stepcompress_t;
typedef struct tuxrt_steppersync_t tuxrt_steppersync_t;

// Forward declaration for signal handler
typedef struct tuxrt_error_context_t tuxrt_error_context_t;

struct tuxrt_error_context_t {
    jmp_buf jump_buffer;
    volatile int caught_signal;
    volatile tuxrt_err_t error_code;
    char error_message[256];
    void* fault_address;
    uint64_t timestamp_ns;
};

// Thread-local error context (one per thread)
TUXRT_SDK_API tuxrt_error_context_t* tuxrt_error_context_get(void);

// Initialize error context for current thread (call once per thread at startup)
TUXRT_SDK_API tuxrt_err_t tuxrt_error_context_init(void);

// Cleanup error context
TUXRT_SDK_API void tuxrt_error_context_fini(void);

// Install signal handlers for SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT
TUXRT_SDK_API tuxrt_err_t tuxrt_install_signal_handlers(void);

// Remove signal handlers (for cleanup)
TUXRT_SDK_API void tuxrt_remove_signal_handlers(void);

#ifdef __cplusplus
}
#endif

// =============================================================================
// Tipos fundamentales (alineados a 64 bytes para hot-path)
// =============================================================================

// Vector 3D (posición, velocidad, aceleración)
typedef struct __attribute__((aligned(64))) {
    double x, y, z;
} tuxrt_vec3_t;

// Cuaternión (orientación)
typedef struct __attribute__((aligned(64))) {
    double w, x, y, z;
} tuxrt_quat_t;

// Pose completa (posición + orientación)
typedef struct __attribute__((aligned(64))) {
    tuxrt_vec3_t pos;
    tuxrt_quat_t rot;
} tuxrt_pose_t;

// Joint state (para cinemática multi-eje)
typedef struct __attribute__((aligned(64))) {
    double position[12];  // max 12 ejes (6+6 dual)
    double velocity[12];
    double effort[12];
    uint8_t axis_count;
    uint8_t reserved[7];
} tuxrt_joint_state_t;

// Timestamp nanosegundos (monotonic, RT-safe)
typedef int64_t tuxrt_timestamp_ns_t;

// =============================================================================
// Configuración: opaque types + accessors
// =============================================================================
TUXRT_SDK_API tuxrt_err_t tuxrt_config_load(const char* path, tuxrt_config_t** out_config);

// Crear config vacía
TUXRT_SDK_API tuxrt_err_t tuxrt_config_create(tuxrt_config_t** out_config);

// Clonar config
TUXRT_SDK_API tuxrt_err_t tuxrt_config_clone(const tuxrt_config_t* src, tuxrt_config_t** out_config);

// Liberar
TUXRT_SDK_API void tuxrt_config_destroy(tuxrt_config_t* config);

// Getters (thread-safe, config es inmutable tras load/create)
TUXRT_SDK_API tuxrt_err_t tuxrt_config_get_int(const tuxrt_config_t* cfg, const char* key, int64_t* out);
TUXRT_SDK_API tuxrt_err_t tuxrt_config_get_double(const tuxrt_config_t* cfg, const char* key, double* out);
TUXRT_SDK_API tuxrt_err_t tuxrt_config_get_bool(const tuxrt_config_t* cfg, const char* key, bool* out);
TUXRT_SDK_API tuxrt_err_t tuxrt_config_get_string(const tuxrt_config_t* cfg, const char* key, const char** out);
TUXRT_SDK_API tuxrt_err_t tuxrt_config_get_vec3(const tuxrt_config_t* cfg, const char* key, tuxrt_vec3_t* out);

// Setters (solo en config creada con create, no en loaded)
TUXRT_SDK_API tuxrt_err_t tuxrt_config_set_int(tuxrt_config_t* cfg, const char* key, int64_t val);
TUXRT_SDK_API tuxrt_err_t tuxrt_config_set_double(tuxrt_config_t* cfg, const char* key, double val);
TUXRT_SDK_API tuxrt_err_t tuxrt_config_set_bool(tuxrt_config_t* cfg, const char* key, bool val);
TUXRT_SDK_API tuxrt_err_t tuxrt_config_set_string(tuxrt_config_t* cfg, const char* key, const char* val);

// =============================================================================
// Scheduler RT (CORE)
// =============================================================================

typedef struct tuxrt_scheduler_t tuxrt_scheduler_t;

typedef enum {
    TUXRT_SCHED_POLICY_FIFO = 0,
    TUXRT_SCHED_POLICY_RR   = 1,
    TUXRT_SCHED_POLICY_DEADLINE = 2,  // SCHED_DEADLINE (Linux 3.14+)
} tuxrt_sched_policy_t;

typedef struct {
    tuxrt_sched_policy_t policy;
    int priority;              // 1-99 (RT), 0 (normal)
    uint64_t runtime_ns;       // Para DEADLINE
    uint64_t deadline_ns;      // Para DEADLINE
    uint64_t period_ns;        // Para DEADLINE
    int cpu_affinity;          // -1 = any, >=0 = CPU específica
} tuxrt_thread_attr_t;

typedef void (*tuxrt_task_fn_t)(void* arg);

// Crear scheduler (inicializa timerfd, cpu_set_t, etc.)
TUXRT_SDK_API tuxrt_err_t tuxrt_scheduler_create(const tuxrt_config_t* cfg, tuxrt_scheduler_t** out_sched);

// Iniciar hilo RT con atributos
TUXRT_SDK_API tuxrt_err_t tuxrt_scheduler_spawn(tuxrt_scheduler_t* sched,
                                                  tuxrt_task_fn_t fn,
                                                  void* arg,
                                                  const tuxrt_thread_attr_t* attr,
                                                  uint64_t* out_thread_id);

// Programar tarea periódica (tick rate Hz)
TUXRT_SDK_API tuxrt_err_t tuxrt_scheduler_periodic(tuxrt_scheduler_t* sched,
                                                      tuxrt_task_fn_t fn,
                                                      void* arg,
                                                      uint32_t hz,
                                                      const tuxrt_thread_attr_t* attr,
                                                      uint64_t* out_timer_id);

// Cancelar timer periódico
TUXRT_SDK_API tuxrt_err_t tuxrt_scheduler_cancel(tuxrt_scheduler_t* sched, uint64_t timer_id);

// Obtener timestamp actual (CLOCK_MONOTONIC_RAW)
TUXRT_SDK_API tuxrt_timestamp_ns_t tuxrt_scheduler_now(const tuxrt_scheduler_t* sched);

// Dormir hasta timestamp absoluto (RT-safe)
TUXRT_SDK_API tuxrt_err_t tuxrt_scheduler_sleep_until(tuxrt_scheduler_t* sched, tuxrt_timestamp_ns_t deadline);

// Métricas del scheduler
typedef struct {
    uint64_t ticks_total;
    uint64_t ticks_missed;
    uint64_t max_latency_ns;
    uint64_t avg_latency_ns;
    uint64_t context_switches;
} tuxrt_scheduler_stats_t;

TUXRT_SDK_API tuxrt_err_t tuxrt_scheduler_stats(const tuxrt_scheduler_t* sched, tuxrt_scheduler_stats_t* out);

TUXRT_SDK_API void tuxrt_scheduler_destroy(tuxrt_scheduler_t* sched);

// =============================================================================
// Cinemática (CORE)
// =============================================================================

typedef struct tuxrt_kinematics_t tuxrt_kinematics_t;

typedef enum {
    TUXRT_KIN_CARTESIAN = 0,
    TUXRT_KIN_COREXY    = 1,
    TUXRT_KIN_DELTA     = 2,
    TUXRT_KIN_POLAR     = 3,
    TUXRT_KIN_SCARA     = 4,
    TUXRT_KIN_CUSTOM    = 0xFF,  // Plugin-provided
} tuxrt_kin_type_t;

typedef struct {
    tuxrt_kin_type_t type;
    // Parámetros específicos por tipo (union-like via config)
    double params[32];  // Generic params, interpretados por tipo
    uint8_t param_count;
} tuxrt_kin_config_t;

// Forward/inverse kinematics
TUXRT_SDK_API tuxrt_err_t tuxrt_kinematics_create(const tuxrt_kin_config_t* cfg, tuxrt_kinematics_t** out_kin);
TUXRT_SDK_API tuxrt_err_t tuxrt_kinematics_forward(const tuxrt_kinematics_t* kin,
                                                      const tuxrt_joint_state_t* joints,
                                                      tuxrt_pose_t* out_pose);
TUXRT_SDK_API tuxrt_err_t tuxrt_kinematics_inverse(const tuxrt_kinematics_t* kin,
                                                      const tuxrt_pose_t* pose,
                                                      tuxrt_joint_state_t* out_joints);
// Jacobiano (para control de fuerza, singularidades)
TUXRT_SDK_API tuxrt_err_t tuxrt_kinematics_jacobian(const tuxrt_kinematics_t* kin,
                                                       const tuxrt_joint_state_t* joints,
                                                       double* out_jacobian,  // 6 x N matrix row-major
                                                       size_t* out_rows,
                                                       size_t* out_cols);
// Verificar singularidad
TUXRT_SDK_API bool tuxrt_kinematics_is_singular(const tuxrt_kinematics_t* kin, const tuxrt_joint_state_t* joints, double threshold);

TUXRT_SDK_API void tuxrt_kinematics_destroy(tuxrt_kinematics_t* kin);

// =============================================================================
// Motion Planning (CORE)
// =============================================================================

typedef struct tuxrt_motion_t tuxrt_motion_t;
typedef struct tuxrt_steppersync_t tuxrt_steppersync_t;

typedef enum {
    TUXRT_MOTION_TRAPEZOIDAL = 0,
    TUXRT_MOTION_S_CURVE     = 1,
    TUXRT_MOTION_JERK_LIMITED = 2,
} tuxrt_motion_profile_t;

typedef struct {
    tuxrt_motion_profile_t profile;
    double max_velocity;      // mm/s
    double max_acceleration;  // mm/s^2
    double max_jerk;          // mm/s^3 (para S-curve)
    double junction_deviation; // mm
    uint32_t lookahead;       // segmentos
} tuxrt_motion_config_t;

typedef struct {
    tuxrt_pose_t target;
    tuxrt_vec3_t velocity;    // feedrate override
    bool relative;            // G91 vs G90
    uint64_t timestamp_ns;    // desired start time (0 = ASAP)
} tuxrt_motion_cmd_t;

typedef enum {
    TUXRT_MOTION_STATUS_IDLE = 0,
    TUXRT_MOTION_STATUS_RUNNING = 1,
    TUXRT_MOTION_STATUS_PAUSED = 2,
    TUXRT_MOTION_STATUS_ERROR = 3,
} tuxrt_motion_status_t;

TUXRT_SDK_API tuxrt_err_t tuxrt_motion_create(const tuxrt_config_t* cfg, const tuxrt_kinematics_t* kin, tuxrt_motion_t** out_motion);
TUXRT_SDK_API tuxrt_err_t tuxrt_motion_enqueue(tuxrt_motion_t* motion, const tuxrt_motion_cmd_t* cmd);
TUXRT_SDK_API tuxrt_err_t tuxrt_motion_step(tuxrt_motion_t* motion, tuxrt_timestamp_ns_t now, tuxrt_joint_state_t* out_joints);
TUXRT_SDK_API tuxrt_err_t tuxrt_motion_status(const tuxrt_motion_t* motion, tuxrt_motion_status_t* out_status, size_t* out_queued);
TUXRT_SDK_API tuxrt_err_t tuxrt_motion_pause(tuxrt_motion_t* motion);
TUXRT_SDK_API tuxrt_err_t tuxrt_motion_resume(tuxrt_motion_t* motion);
TUXRT_SDK_API tuxrt_err_t tuxrt_motion_emergency_stop(tuxrt_motion_t* motion);
TUXRT_SDK_API void tuxrt_motion_destroy(tuxrt_motion_t* motion);

// =============================================================================
// KARES — Kinematic Adaptive Resonance Suppression (CORE)
// =============================================================================

typedef struct tuxrt_kares_t tuxrt_kares_t;

typedef struct {
    double frequency_hz;      // Frecuencia de resonancia detectada
    double amplitude;         // Amplitud (mm)
    double damping_ratio;     // 0-1
    uint32_t axis_mask;       // Qué ejes afecta
} tuxrt_resonance_t;

typedef void (*tuxrt_kares_callback_t)(const tuxrt_resonance_t* res, void* user_data);

TUXRT_SDK_API tuxrt_err_t tuxrt_kares_create(const tuxrt_config_t* cfg, tuxrt_kares_t** out_kares);
TUXRT_SDK_API tuxrt_err_t tuxrt_kares_analyze(tuxrt_kares_t* kares, const tuxrt_joint_state_t* measured, const tuxrt_joint_state_t* commanded);
TUXRT_SDK_API tuxrt_err_t tuxrt_kares_get_shaper(tuxrt_kares_t* kares, double* out_shaper_coeffs, size_t* out_len);
TUXRT_SDK_API tuxrt_err_t tuxrt_kares_set_callback(tuxrt_kares_t* kares, tuxrt_kares_callback_t cb, void* user_data);
TUXRT_SDK_API void tuxrt_kares_destroy(tuxrt_kares_t* kares);

// =============================================================================
// SIRC — Safety Integrity Runtime Controller (CORE) — F1-F10
// =============================================================================

typedef struct tuxrt_sirc_t tuxrt_sirc_t;

// ---- Safe Mode States N1-N6 (per IEC 61800-5-2 / ISO 13849) ----
typedef enum {
    TUXRT_SAFE_MODE_N1_NORMAL      = 0,  // Operación normal
    TUXRT_SAFE_MODE_N2_DEGRADED    = 1,  // Degradado: funcionalidad reducida
    TUXRT_SAFE_MODE_N3_SAFE_STOP   = 2,  // Parada segura Cat.1 (controlado)
    TUXRT_SAFE_MODE_N4_EMERGENCY   = 3,  // Parada emergencia Cat.0 (inmediata)
    TUXRT_SAFE_MODE_N5_RECOVERY    = 4,  // Recuperación: validando condiciones
    TUXRT_SAFE_MODE_N6_QUARANTINE  = 5,  // Cuarentena: aislamiento total
} tuxrt_safe_mode_t;

// ---- Fault Levels ----
typedef enum {
    TUXRT_SIRC_LEVEL_NONE = 0,
    TUXRT_SIRC_LEVEL_WARN = 1,
    TUXRT_SIRC_LEVEL_SAFE_STOP = 2,    // Category 1 stop
    TUXRT_SIRC_LEVEL_EMERGENCY = 3,    // Category 0 stop
} tuxrt_sirc_level_t;

typedef struct {
    tuxrt_sirc_level_t level;
    const char* message;
    uint64_t timestamp_ns;
    uint32_t fault_code;  // Código específico del fallo
} tuxrt_sirc_event_t;

typedef void (*tuxrt_sirc_handler_t)(const tuxrt_sirc_event_t* event, void* user_data);

// ---- Circuit Breaker States ----
typedef enum {
    TUXRT_CB_CLOSED     = 0,  // Normal: requests pass through
    TUXRT_CB_OPEN       = 1,  // Open: requests fail fast
    TUXRT_CB_HALF_OPEN  = 2,  // Half-open: testing recovery
} tuxrt_cb_state_t;

// ---- Exponential Backoff Config ----
typedef struct {
    uint64_t base_delay_ns;      // Base delay (e.g., 10ms)
    uint64_t max_delay_ns;       // Max delay (e.g., 5s)
    double multiplier;           // Exponential multiplier (e.g., 2.0)
    double jitter_factor;        // Jitter 0.0-1.0 (e.g., 0.1)
    uint32_t max_retries;        // Max retry attempts
} tuxrt_backoff_config_t;

// ---- Transport Adapter Abstraction ----
typedef enum {
    TUXRT_TRANSPORT_USB      = 0,
    TUXRT_TRANSPORT_UART     = 1,
    TUXRT_TRANSPORT_CAN      = 2,
    TUXRT_TRANSPORT_ETHERCAT = 3,
    TUXRT_TRANSPORT_SPI      = 4,
    TUXRT_TRANSPORT_I2C      = 5,
    // EtherTux: Ethernet comercial determinista (trama 0x88B5). Se añade al
    // final para no alterar los valores publicados.
    TUXRT_TRANSPORT_ETHERTUX = 6,
} tuxrt_transport_type_t;

typedef struct tuxrt_transport_adapter_t tuxrt_transport_adapter_t;

typedef struct {
    tuxrt_transport_type_t type;
    const char* device_path;     // /dev/ttyACM0, can0, etc.
    uint32_t baudrate;           // Para UART
    uint32_t bitrate;            // Para CAN/EtherCAT
    bool flow_control;
    // Failover config
    bool enable_failover;
    tuxrt_transport_type_t fallback_type;
    const char* fallback_device;
} tuxrt_transport_adapter_config_t;

// Transport adapter VTable (for plugin transport drivers)
typedef struct {
    uint32_t abi_version;
    tuxrt_err_t (*open)(tuxrt_transport_adapter_t* adapter);
    tuxrt_err_t (*close)(tuxrt_transport_adapter_t* adapter);
    tuxrt_err_t (*send)(tuxrt_transport_adapter_t* adapter, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline);
    tuxrt_err_t (*recv)(tuxrt_transport_adapter_t* adapter, uint8_t* buf, size_t* len, tuxrt_timestamp_ns_t deadline);
    tuxrt_err_t (*flush)(tuxrt_transport_adapter_t* adapter);
    tuxrt_err_t (*get_stats)(tuxrt_transport_adapter_t* adapter, void* stats_out);
    void (*destroy)(tuxrt_transport_adapter_t* adapter);
} tuxrt_transport_vtable_t;

typedef struct {
    tuxrt_transport_type_t type;
    const char* device_path;
    uint32_t baudrate;
    uint32_t bitrate;
    bool flow_control;
} tuxrt_transport_config_t;

// ---- Policy Engine for Failover ----
typedef enum {
    TUXRT_POLICY_ACTION_NONE        = 0,
    TUXRT_POLICY_ACTION_RETRY       = 1,
    TUXRT_POLICY_ACTION_FAILOVER    = 2,
    TUXRT_POLICY_ACTION_SAFE_STOP   = 3,
    TUXRT_POLICY_ACTION_EMERGENCY   = 4,
    TUXRT_POLICY_ACTION_QUARANTINE  = 5,
} tuxrt_policy_action_t;

typedef struct {
    tuxrt_policy_action_t action;
    tuxrt_safe_mode_t target_safe_mode;
    uint64_t retry_delay_ns;
    const char* reason;
} tuxrt_policy_decision_t;

typedef tuxrt_policy_decision_t (*tuxrt_policy_fn_t)(const tuxrt_sirc_event_t* event,
                                                      const void* context,
                                                      void* user_data);

// ---- Health Monitor ----
typedef struct {
    uint64_t last_success_ns;
    uint64_t last_failure_ns;
    uint32_t consecutive_failures;
    uint32_t consecutive_successes;
    tuxrt_cb_state_t cb_state;
    double error_rate;  // 0.0-1.0
} tuxrt_health_stats_t;

// ---- Complete SIRC Config ----
typedef struct {
    // Safety limits
    double max_position_error;     // mm
    double max_velocity_error;     // mm/s
    double max_temperature;        // °C
    double min_temperature;        // °C
    uint64_t watchdog_timeout_ns;  // Timeout watchdog
    bool enable_position_check;
    bool enable_thermal_check;
    bool enable_watchdog;

    // Circuit Breaker
    tuxrt_backoff_config_t backoff;
    uint32_t cb_failure_threshold;   // Failures before OPEN
    uint32_t cb_success_threshold;   // Successes before CLOSED from HALF_OPEN
    uint64_t cb_timeout_ns;          // Time in OPEN before HALF_OPEN

    // Transport failover
    tuxrt_transport_adapter_config_t primary_transport;
    tuxrt_transport_adapter_config_t fallback_transport;
    bool enable_transport_failover;
    uint64_t failover_timeout_ns;  // Timeout for failover to fallback adapter

    // Policy
    tuxrt_policy_fn_t policy_fn;
    void* policy_user_data;

    // Handler
    tuxrt_sirc_handler_t handler;
    void* handler_user_data;
} tuxrt_sirc_config_t;

// ---- SIRC API ----
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_create(const tuxrt_sirc_config_t* cfg, tuxrt_sirc_t** out_sirc);

// Core safety check (called from motion loop, ~1kHz)
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_check(tuxrt_sirc_t* sirc,
                                            const tuxrt_joint_state_t* cmd,
                                            const tuxrt_joint_state_t* fb,
                                            const double* temperatures,
                                            size_t temp_count);

// Watchdog
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_kick_watchdog(tuxrt_sirc_t* sirc);

// State queries
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_get_safe_mode(const tuxrt_sirc_t* sirc, tuxrt_safe_mode_t* out_mode);
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_get_health(const tuxrt_sirc_t* sirc, tuxrt_health_stats_t* out_health);
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_get_transport_stats(const tuxrt_sirc_t* sirc, void* stats_out);

// Transport adapter management
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_register_transport(tuxrt_sirc_t* sirc,
                                                          const tuxrt_transport_adapter_config_t* cfg,
                                                          const tuxrt_transport_vtable_t* vtable);
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_set_active_transport(tuxrt_sirc_t* sirc, tuxrt_transport_type_t type);
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_trigger_failover(tuxrt_sirc_t* sirc, tuxrt_transport_type_t target);

// EtherTux Transport registration helper
// Externally provided by transport_ethertux.c
extern const tuxrt_transport_vtable_t tuxrt_transport_ethertux_vtable;

// Policy
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_set_policy(tuxrt_sirc_t* sirc, tuxrt_policy_fn_t fn, void* user_data);

// Handler
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_set_handler(tuxrt_sirc_t* sirc, tuxrt_sirc_handler_t h, void* user_data);

// Exponential backoff helper
TUXRT_SDK_API uint64_t tuxrt_backoff_compute(const tuxrt_backoff_config_t* cfg, uint32_t attempt);

// Deterministic sleep using clock_nanosleep with absolute time
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_deterministic_sleep(tuxrt_timestamp_ns_t abs_deadline_ns);

// Periodic SIRC monitoring: watchdog, failover, circuit breaker, recovery
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_monitor(tuxrt_sirc_t* sirc);

// Destroy
TUXRT_SDK_API void tuxrt_sirc_destroy(tuxrt_sirc_t* sirc);

// EtherTux Transport registration helper (convenience wrapper)
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_register_ethertux(tuxrt_sirc_t* sirc,
                                                       const tuxrt_transport_adapter_config_t* cfg);

// EtherCAT Transport registration helper (convenience wrapper)
TUXRT_SDK_API tuxrt_err_t tuxrt_sirc_register_ethercat(tuxrt_sirc_t* sirc,
                                                        const tuxrt_transport_adapter_config_t* cfg);

// =============================================================================
// Transport vtable symbols (extern declarations for plugin transport drivers)
// =============================================================================

// EtherCAT Transport vtable (externally provided by transport_ethercat.c)
extern const tuxrt_transport_vtable_t tuxrt_transport_ethercat_vtable;

// Transport adapter instance (defined here after all dependent types)
struct tuxrt_transport_adapter_t {
    const tuxrt_transport_vtable_t* vtable;
    void* impl;
    tuxrt_transport_adapter_config_t config;
    bool active;
    tuxrt_health_stats_t health;
    pthread_mutex_t mutex;
};

// Internal helpers for transport drivers
TUXRT_SDK_API void tuxrt_sirc_update_health_success(tuxrt_transport_adapter_t* adapter);
TUXRT_SDK_API void tuxrt_sirc_update_health_failure(tuxrt_transport_adapter_t* adapter);
TUXRT_SDK_API tuxrt_timestamp_ns_t tuxrt_now_ns(void);

// =============================================================================
// AUTOCFG — Auto-configuration System
// =============================================================================

// Configuration validation result
typedef enum {
    TUXRT_CFGVAL_OK = 0,
    TUXRT_CFGVAL_WARN = 1,
    TUXRT_CFGVAL_ERROR = 2,
    TUXRT_CFGVAL_FATAL = 3,
} tuxrt_cfgval_result_t;

// Configuration validation rule
typedef struct {
    const char* key;
    const char* min_value;
    const char* max_value;
    const char* allowed_values;
    bool required;
} tuxrt_cfgval_rule_t;

// Configuration trace entry
typedef struct {
    tuxrt_timestamp_ns_t timestamp;
    const char* key;
    const char* source; // "default", "file", "env", "cli", "auto"
    const char* old_value;
    const char* new_value;
    tuxrt_cfgval_result_t validation;
    const char* message;
} tuxrt_cfgtrc_entry_t;

// Configuration result
typedef struct {
    char* data;
    size_t size;
    tuxrt_cfgval_result_t result;
    const char* error_message;
} tuxrt_cfgres_t;

// Auto-config manager
typedef struct tuxrt_autocfg_t tuxrt_autocfg_t;

// AUTOCFG API
TUXRT_SDK_API tuxrt_err_t tuxrt_autocfg_create(tuxrt_autocfg_t** out_cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_autocfg_destroy(tuxrt_autocfg_t* cfg);

// Config validation
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgval_add_rule(tuxrt_autocfg_t* cfg, const tuxrt_cfgval_rule_t* rule);
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgval_validate_key(tuxrt_autocfg_t* cfg, const char* key, const char* value, tuxrt_cfgval_result_t* out_result);
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgval_validate_all(tuxrt_autocfg_t* cfg, tuxrt_cfgval_result_t* out_result);

// Config loading
TUXRT_SDK_API tuxrt_err_t tuxrt_autocfg_load_defaults(tuxrt_autocfg_t* cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_autocfg_load_file(tuxrt_autocfg_t* cfg, const char* path);
TUXRT_SDK_API tuxrt_err_t tuxrt_autocfg_load_env(tuxrt_autocfg_t* cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_autocfg_load_cli(tuxrt_autocfg_t* cfg, int argc, char** argv);

// Config tracing
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgtrc_enable(tuxrt_autocfg_t* cfg, bool enable);
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgtrc_get_entries(tuxrt_autocfg_t* cfg, tuxrt_cfgtrc_entry_t** out_entries, size_t* out_count);
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgtrc_clear(tuxrt_autocfg_t* cfg);

// Config access
TUXRT_SDK_API const char* tuxrt_autocfg_get(const tuxrt_autocfg_t* cfg, const char* key);
TUXRT_SDK_API tuxrt_err_t tuxrt_autocfg_set(tuxrt_autocfg_t* cfg, const char* key, const char* value);

// Config result
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgres_create(tuxrt_cfgres_t** out_res);
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgres_destroy(tuxrt_cfgres_t* res);
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgres_set_data(tuxrt_cfgres_t* res, const void* data, size_t size);
TUXRT_SDK_API tuxrt_err_t tuxrt_cfgres_set_result(tuxrt_cfgres_t* res, tuxrt_cfgval_result_t result, const char* message);

// =============================================================================
// WAL — Write-Ahead Log Persistencia (CORE)
// =============================================================================

typedef struct tuxrt_wal_t tuxrt_wal_t;

typedef enum {
    TUXRT_WAL_OP_SET_CONFIG = 1,
    TUXRT_WAL_OP_CALIBRATE = 2,
    TUXRT_WAL_OP_FIRMWARE_UPDATE = 3,
    TUXRT_WAL_OP_USER_ACTION = 4,
} tuxrt_wal_op_type_t;

typedef struct {
    tuxrt_wal_op_type_t type;
    uint64_t sequence;
    tuxrt_timestamp_ns_t timestamp;
    const char* key;      // Clave de config (ej: "printer.extruder.steps_mm")
    const char* value;    // Valor serializado (JSON/msgpack)
    uint32_t crc32;       // Checksum
} tuxrt_wal_entry_t;

typedef void (*tuxrt_wal_replay_fn_t)(const tuxrt_wal_entry_t* entry, void* user_data);

TUXRT_SDK_API tuxrt_err_t tuxrt_wal_create(const char* path, size_t max_segment_mb, tuxrt_wal_t** out_wal);
TUXRT_SDK_API tuxrt_err_t tuxrt_wal_append(tuxrt_wal_t* wal, tuxrt_wal_op_type_t type, const char* key, const char* value);
TUXRT_SDK_API tuxrt_err_t tuxrt_wal_replay(tuxrt_wal_t* wal, tuxrt_wal_replay_fn_t fn, void* user_data);
TUXRT_SDK_API tuxrt_err_t tuxrt_wal_checkpoint(tuxrt_wal_t* wal);
TUXRT_SDK_API tuxrt_err_t tuxrt_wal_verify(tuxrt_wal_t* wal, bool* out_ok);
TUXRT_SDK_API void tuxrt_wal_destroy(tuxrt_wal_t* wal);
TUXRT_SDK_API tuxrt_err_t tuxrt_wal_set_group_commit(tuxrt_wal_t* wal, bool enabled);

// Define legacy transport struct for backward compatibility
typedef struct {
    tuxrt_transport_config_t cfg;
    int fd;
    bool open;
    pthread_mutex_t mutex;
    // Ring buffers
    uint8_t* rx_buf;
    size_t rx_cap;
    size_t rx_head, rx_tail;
    uint8_t* tx_buf;
    size_t tx_cap;
    size_t tx_head, tx_tail;
} tuxrt_legacy_transport_t;

typedef struct {
    uint8_t* data;
    size_t len;
    size_t capacity;
} tuxrt_buffer_t;

// =============================================================================
// Transport / MCU Communication (CORE) — Uses SIRC transport types
// =============================================================================

// Legacy simple transport API (deprecated, use SIRC transport adapters)
// Maps to tuxrt_legacy_transport_t in SDK

TUXRT_SDK_API tuxrt_err_t tuxrt_transport_create(const tuxrt_transport_config_t* cfg, tuxrt_legacy_transport_t** out_transport);
TUXRT_SDK_API tuxrt_err_t tuxrt_transport_open(tuxrt_legacy_transport_t* transport);
TUXRT_SDK_API tuxrt_err_t tuxrt_transport_close(tuxrt_legacy_transport_t* transport);
TUXRT_SDK_API tuxrt_err_t tuxrt_transport_send(tuxrt_legacy_transport_t* transport, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline);
TUXRT_SDK_API tuxrt_err_t tuxrt_transport_recv(tuxrt_legacy_transport_t* transport, tuxrt_buffer_t* buf, tuxrt_timestamp_ns_t deadline);
TUXRT_SDK_API tuxrt_err_t tuxrt_transport_flush(tuxrt_legacy_transport_t* transport);
TUXRT_SDK_API void tuxrt_transport_destroy(tuxrt_legacy_transport_t* transport);

// =============================================================================
// Dependencias opcionales (resolución diferida, sin enlazado duro)
// =============================================================================
// libtuxrt_core NO enlaza contra ChronosRT en tiempo de compilación y NO falla
// ni se rompe si `libchronosrt.so` no está instalada: las dependencias
// opcionales se resuelven con dlopen() en tiempo de ejecución y su ausencia se
// reporta de forma estructurada. Así la CLI/TUI/FFI (tuxrt) puede informar al
// sistema de que debe instalarse (p. ej. ChronosRT >= 0.1.0 para EtherCAT).

#define TUXRT_DEP_NAME_MAX    32
#define TUXRT_DEP_VERSION_MAX 16
#define TUXRT_DEP_SONAME_MAX  64
#define TUXRT_DEP_HINT_MAX    256

typedef struct {
    char name[TUXRT_DEP_NAME_MAX];                // p. ej. "ChronosRT"
    char soname[TUXRT_DEP_SONAME_MAX];            // p. ej. "libchronosrt.so"
    char required_version[TUXRT_DEP_VERSION_MAX]; // versión mínima requerida
    char installed_version[TUXRT_DEP_VERSION_MAX];// "" si no está instalada
    bool available;                               // true si el soname es cargable
    char hint[TUXRT_DEP_HINT_MAX];                // mensaje accionable (instalación)
} tuxrt_dependency_t;

// Consulta el estado de una dependencia opcional por nombre ("ChronosRT").
// @return TUXRT_OK                  si está disponible (out->available = true)
//         TUXRT_ERR_DEPENDENCY_MISSING si falta (out->hint describe la acción)
//         TUXRT_ERR_INVAL           si los argumentos o el nombre son inválidos
// Nunca aborta ni falla el proceso si la librería no está presente.
TUXRT_SDK_API tuxrt_err_t tuxrt_dependency_query(const char* name, tuxrt_dependency_t* out);

// Escribe en `buf` (NUL-terminado) un informe legible del estado de todas las
// dependencias opcionales conocidas. Devuelve TUXRT_OK si `buf` es válido; el
// texto indica qué falta. Pensado para CLI/TUI y registro del sistema.
TUXRT_SDK_API tuxrt_err_t tuxrt_dependency_report(char* buf, size_t len);

// =============================================================================
// Cargador universal de proveedores (TuxRT Provider ABI)
// =============================================================================
// Contrato universal: todo proyecto que ofrezca capacidades al núcleo
// (ChronosRT o cualquier otro) exporta el símbolo `tuxrt_provider_descriptor`
// descrito en `tuxrt_provider.h`. Este cargador genérico lo descubre y enlaza
// con dlopen: la dependencia es UNIDIRECCIONAL (consumidor -> proveedor) y la
// ausencia del proveedor NUNCA rompe el proceso.
//
// Para añadir un proveedor nuevo basta con exportar el símbolo (o invocar la
// función CMake `tuxrt_provide()` del módulo TuxRTProvider.cmake); no requiere
// cambios en libtuxrt_core.

#define TUXRT_PROVIDER_NAME_MAX    32
#define TUXRT_PROVIDER_VER_MAX     16
#define TUXRT_PROVIDER_CAPS_MAX   160
#define TUXRT_PROVIDER_SONAME_MAX  64

typedef struct {
    char     name[TUXRT_PROVIDER_NAME_MAX];         // p. ej. "ChronosRT"
    char     version[TUXRT_PROVIDER_VER_MAX];       // p. ej. "0.1.0"
    char     capabilities[TUXRT_PROVIDER_CAPS_MAX]; // "ethercat,rt-scheduler,..."
    char     soname[TUXRT_PROVIDER_SONAME_MAX];     // "libchronosrt.so"
    uint32_t abi_version;                           // TUXRT_PROVIDER_ABI_VERSION
    bool     available;                             // true si se pudo enlazar
} tuxrt_provider_info_t;

// Abre un proveedor por soname (dlopen) y lo deja ENLAZADO: el handle queda
// abierto para resolver símbolos con tuxrt_provider_symbol().
// @param soname  "libchronosrt.so" (o ruta); se usa la búsqueda del loader.
// @param handle  [out] handle para symbol()/close(); puede ser NULL si sólo se
//                desea la info (en ese caso se cierra inmediatamente).
// @param out     [out, opcional] info normalizada del descriptor.
// @return TUXRT_OK                     si se abrió y el descriptor es válido
//         TUXRT_ERR_DEPENDENCY_MISSING si el soname no es cargable
//         TUXRT_ERR_NOENT              si falta el símbolo descriptor
//         TUXRT_ERR_VERSION_MISMATCH   si el ABI del descriptor es incompatible
//         TUXRT_ERR_INVAL              si soname es NULL
TUXRT_SDK_API tuxrt_err_t tuxrt_provider_open(const char* soname, void** handle,
                                              tuxrt_provider_info_t* out);

// Cierra un handle devuelto por tuxrt_provider_open(). NULL-safe.
TUXRT_SDK_API void tuxrt_provider_close(void* handle);

// Resuelve un símbolo del proveedor abierto (dlsym). El puntero devuelto es
// válido mientras el handle siga abierto.
TUXRT_SDK_API tuxrt_err_t tuxrt_provider_symbol(void* handle, const char* symbol,
                                                void** out);

// Sonda un proveedor por soname SIN dejarlo enlazado (open + close). Rellena
// `out` (si no es NULL) y devuelve TUXRT_OK si está disponible.
TUXRT_SDK_API tuxrt_err_t tuxrt_provider_probe(const char* soname,
                                               tuxrt_provider_info_t* out);

// Localiza un proveedor por NOMBRE aplicando la convención de soname
// "lib<name en minúsculas>.so" (p. ej. "ChronosRT" -> "libchronosrt.so"). Si
// `name` ya parece un soname (contiene ".so") se usa tal cual.
TUXRT_SDK_API tuxrt_err_t tuxrt_provider_query(const char* name,
                                               tuxrt_provider_info_t* out);

// =============================================================================
// PHSYNC — Phase Synchronization (Mojo ↔ CORE)
// =============================================================================

typedef struct tuxrt_phsync_t tuxrt_phsync_t;

typedef enum {
    TUXRT_PHSYNC_MODE_MASTER = 0,
    TUXRT_PHSYNC_MODE_SLAVE  = 1,
    TUXRT_PHSYNC_MODE_STANDALONE = 2,
} tuxrt_phsync_mode_t;

typedef struct {
    tuxrt_phsync_mode_t mode;
    uint32_t sync_interval_ms;      // Intervalo de re-sync
    uint64_t drift_tolerance_ns;    // Tolerancia deriva
    int cpu_affinity;               // CPU para hilo PHSYNC
} tuxrt_phsync_config_t;

typedef struct {
    int64_t offset_ns;      // Offset local vs master
    int64_t drift_ns;       // Deriva acumulada
    uint64_t last_sync_ns;  // Timestamp último sync
    bool synchronized;      // Estado
} tuxrt_phsync_status_t;

TUXRT_SDK_API tuxrt_err_t tuxrt_phsync_create(const tuxrt_phsync_config_t* cfg, tuxrt_phsync_t** out_phsync);
TUXRT_SDK_API tuxrt_err_t tuxrt_phsync_start(tuxrt_phsync_t* phsync);
TUXRT_SDK_API tuxrt_err_t tuxrt_phsync_stop(tuxrt_phsync_t* phsync);
TUXRT_SDK_API tuxrt_err_t tuxrt_phsync_status(const tuxrt_phsync_t* phsync, tuxrt_phsync_status_t* out);
TUXRT_SDK_API tuxrt_err_t tuxrt_phsync_adjust(tuxrt_phsync_t* phsync, int64_t correction_ns);
TUXRT_SDK_API void tuxrt_phsync_destroy(tuxrt_phsync_t* phsync);

// =============================================================================
// Mojo Orchestrator (Mojo FFI opaque)
// =============================================================================

typedef struct tuxrt_t tuxrt_t;

typedef struct {
    const char* config_path;
    const char* printer_data_root;
    int log_level;  // 0=trace, 1=debug, 2=info, 3=warn, 4=error
    bool daemonize;
} tuxrt_config_t;

typedef enum {
    TUXRT_STATE_STOPPED = 0,
    TUXRT_STATE_STARTING = 1,
    TUXRT_STATE_RUNNING = 2,
    TUXRT_STATE_DEGRADED = 3,
    TUXRT_STATE_STOPPING = 4,
    TUXRT_STATE_ERROR = 5,
} tuxrt_state_t;

typedef void (*tuxrt_state_cb_t)(tuxrt_state_t old_state, tuxrt_state_t new_state, void* user_data);

TUXRT_SDK_API tuxrt_err_t tuxrt_create(const tuxrt_config_t* cfg, tuxrt_t** out_ctx);
TUXRT_SDK_API tuxrt_err_t tuxrt_start(tuxrt_t* ctx);
TUXRT_SDK_API tuxrt_err_t tuxrt_stop(tuxrt_t* ctx, bool graceful);
TUXRT_SDK_API tuxrt_err_t tuxrt_state(const tuxrt_t* ctx, tuxrt_state_t* out);
TUXRT_SDK_API tuxrt_err_t tuxrt_set_state_cb(tuxrt_t* ctx, tuxrt_state_cb_t cb, void* user_data);
TUXRT_SDK_API void tuxrt_destroy(tuxrt_t* ctx);

// =============================================================================
// i18n Router (Mojo ↔ i18n_worker)
// =============================================================================

typedef struct tuxrt_i18n_t tuxrt_i18n_t;

typedef struct {
    const char* model_dir;      // Directorio modelos CT2
    const char* fallback_lang;  // "en"
    uint32_t timeout_ms;        // Timeout por request
    int worker_port;            // gRPC port (default 50051)
} tuxrt_i18n_config_t;

TUXRT_SDK_API tuxrt_err_t tuxrt_i18n_create(const tuxrt_i18n_config_t* cfg, tuxrt_i18n_t** out_i18n);
TUXRT_SDK_API tuxrt_err_t tuxrt_i18n_translate(tuxrt_i18n_t* i18n,
                                                  const char* src_lang,  // ISO 639-1, "auto" para detect
                                                  const char* dst_lang,
                                                  const char* text,
                                                  char** out_translated);  // caller frees
TUXRT_SDK_API tuxrt_err_t tuxrt_i18n_detect_lang(tuxrt_i18n_t* i18n, const char* text, char** out_lang);
TUXRT_SDK_API tuxrt_err_t tuxrt_i18n_list_models(tuxrt_i18n_t* i18n, const char*** out_langs, size_t* out_count);
TUXRT_SDK_API void tuxrt_i18n_destroy(tuxrt_i18n_t* i18n);

// =============================================================================
// REST mTLS API (Mojo)
// =============================================================================

typedef struct tuxrt_rest_t tuxrt_rest_t;

typedef struct {
    const char* bind_addr;      // "0.0.0.0"
    uint16_t port;              // 4433
    const char* cert_file;      // PEM
    const char* key_file;       // PEM
    const char* ca_file;        // PEM (para client auth)
    uint32_t rate_limit_rps;
    uint32_t rate_limit_burst;
} tuxrt_rest_config_t;

TUXRT_SDK_API tuxrt_err_t tuxrt_rest_create(const tuxrt_rest_config_t* cfg, tuxrt_rest_t** out_rest);
TUXRT_SDK_API tuxrt_err_t tuxrt_rest_start(tuxrt_rest_t* rest);
TUXRT_SDK_API tuxrt_err_t tuxrt_rest_stop(tuxrt_rest_t* rest);
TUXRT_SDK_API void tuxrt_rest_destroy(tuxrt_rest_t* rest);

// =============================================================================
// G-code Parser SIMD (Mojo)
// =============================================================================

typedef struct tuxrt_gcode_t tuxrt_gcode_t;

typedef enum {
    TUXRT_GCODE_RESULT_OK = 0,
    TUXRT_GCODE_RESULT_INCOMPLETE = 1,  // Necesita más datos
    TUXRT_GCODE_RESULT_ERROR = -1,
} tuxrt_gcode_result_t;

typedef struct {
    char command;           // 'G', 'M', 'T'
    uint32_t number;        // 0, 1, 28, 104, etc.
    double params[26];      // X, Y, Z, E, F, S, P, etc. (NaN = no presente)
    bool has_param[26];
    char comment[128];      // Comentario si existe
    uint32_t line_number;
} tuxrt_gcode_parsed_t;

TUXRT_SDK_API tuxrt_err_t tuxrt_gcode_create(tuxrt_gcode_t** out_parser);
TUXRT_SDK_API tuxrt_gcode_result_t tuxrt_gcode_parse(tuxrt_gcode_t* parser,
                                                        const uint8_t* data,
                                                        size_t len,
                                                        tuxrt_gcode_parsed_t* out_parsed,
                                                        size_t* out_consumed);
TUXRT_SDK_API tuxrt_err_t tuxrt_gcode_reset(tuxrt_gcode_t* parser);
TUXRT_SDK_API void tuxrt_gcode_destroy(tuxrt_gcode_t* parser);

// =============================================================================
// Observabilidad (Prometheus + OpenTelemetry)
// =============================================================================

typedef struct tuxrt_metrics_t tuxrt_metrics_t;

typedef enum {
    TUXRT_METRIC_COUNTER = 0,
    TUXRT_METRIC_GAUGE = 1,
    TUXRT_METRIC_HISTOGRAM = 2,
    TUXRT_METRIC_SUMMARY = 3,
} tuxrt_metric_type_t;

TUXRT_SDK_API tuxrt_err_t tuxrt_metrics_create(const char* service_name, const char* otel_endpoint, tuxrt_metrics_t** out_metrics);
TUXRT_SDK_API tuxrt_err_t tuxrt_metrics_register(tuxrt_metrics_t* m, const char* name, tuxrt_metric_type_t type, const char* help, const char* unit);
TUXRT_SDK_API tuxrt_err_t tuxrt_metrics_inc(tuxrt_metrics_t* m, const char* name, double value, const char** labels, size_t label_count);
TUXRT_SDK_API tuxrt_err_t tuxrt_metrics_set(tuxrt_metrics_t* m, const char* name, double value, const char** labels, size_t label_count);
TUXRT_SDK_API tuxrt_err_t tuxrt_metrics_observe(tuxrt_metrics_t* m, const char* name, double value, const char** labels, size_t label_count);
TUXRT_SDK_API void tuxrt_metrics_destroy(tuxrt_metrics_t* m);

// =============================================================================
// Ultracrc — Integridad CRC-32C (Castagnoli) acelerada por hardware/SIMD
// =============================================================================

/**
 * Calcular CRC-32C (Castagnoli) de un buffer.
 *
 * Algoritmo: CRC-32C, poly 0x1EDC6F41 (reflejado 0x82F63B78),
 * init 0xFFFFFFFF, xorout 0xFFFFFFFF, refin/refout = true.
 * Valor de chequeo de "123456789" = 0xE3069283.
 *
 * El backend (SSE4.2 / ARMv8-CRC / escalar) se selecciona automáticamente
 * en la primera llamada según las capacidades detectadas por `tuxrt_hwdet`.
 *
 * Thread-safe. Con `len == 0` devuelve 0.
 *
 * @param data  Puntero a los datos (puede ser NULL si len == 0)
 * @param len   Longitud en bytes
 * @return CRC-32C del buffer
 */
TUXRT_SDK_API uint32_t tuxrt_ultracrc32(const void* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // TUXRT_SDK_H