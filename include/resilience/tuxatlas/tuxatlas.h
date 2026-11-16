/**
 * tuxatlas.h — TuxAtlas Rollback con Golden Image
 *
 * Sistema de rollback atómico con:
 *   - Golden Image de configuración
 *   - Escritura atómica con fsync
 *   - Health check con timeout
 *   - Rollback automático ante fallo
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_TUXATLAS_H
#define TUXRT_TUXATLAS_H

#include "../../sdk/include/tuxrt_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Estados de TuxAtlas
 */
typedef enum {
    TUXATLAS_STATE_HEALTHY = 0,
    TUXATLAS_STATE_DEGRADED = 1,
    TUXATLAS_STATE_FAILED = 2,
    TUXATLAS_STATE_ROLLING_BACK = 3,
    TUXATLAS_STATE_ROLLBACK_COMPLETE = 4
} tuxatlas_state_t;

/**
 * Resultado de health check
 */
typedef struct {
    bool healthy;
    uint32_t checks_passed;
    uint32_t checks_failed;
    char error_message[256];
    uint64_t timestamp_ns;
} tuxatlas_health_result_t;

/**
 * Contexto TuxAtlas
 */
typedef struct tuxatlas_ctx {
    char* golden_image_path;
    char* current_config_path;
    char* backup_path;
    uint32_t health_check_interval_ms;
    uint32_t rollback_timeout_ms;
    tuxatlas_state_t state;
    bool initialized;
    bool health_check_running;
} tuxatlas_ctx_t;

/* ============================================================================
 *  API Pública
 * ============================================================================ */

/**
 * Crear contexto TuxAtlas
 *
 * @param golden_image_path Ruta al Golden Image
 * @param current_config_path Ruta a configuración actual
 * @param backup_path Ruta a backup
 * @param out_ctx Contexto resultante
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_create(
    const char* golden_image_path,
    const char* current_config_path,
    const char* backup_path,
    tuxatlas_ctx_t** out_ctx
);

/**
 * Inicializar TuxAtlas
 *
 * @param ctx Contexto TuxAtlas
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_init(tuxatlas_ctx_t* ctx);

/**
 * Crear Golden Image
 *
 * @param ctx Contexto TuxAtlas
 * @param config_data Datos de configuración
 * @param data_len Longitud de datos
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_create_golden_image(
    tuxatlas_ctx_t* ctx,
    const void* config_data,
    size_t data_len
);

/**
 * Guardar configuración actual (escritura atómica)
 *
 * @param ctx Contexto TuxAtlas
 * @param config_data Datos de configuración
 * @param data_len Longitud de datos
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_save_config(
    tuxatlas_ctx_t* ctx,
    const void* config_data,
    size_t data_len
);

/**
 * Cargar configuración actual
 *
 * @param ctx Contexto TuxAtlas
 * @param out_data Buffer de salida
 * @param out_data_len Longitud de datos
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_load_config(
    tuxatlas_ctx_t* ctx,
    void* out_data,
    size_t* out_data_len
);

/**
 * Health check
 *
 * @param ctx Contexto TuxAtlas
 * @param result Resultado del check
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_health_check(
    tuxatlas_ctx_t* ctx,
    tuxatlas_health_result_t* result
);

/**
 * Iniciar rollback a Golden Image
 *
 * @param ctx Contexto TuxAtlas
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_rollback(tuxatlas_ctx_t* ctx);

/**
 * Verificar si se requiere rollback
 *
 * @param ctx Contexto TuxAtlas
 * @param out_required Salida: true si se requiere rollback
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_check_rollback_needed(
    tuxatlas_ctx_t* ctx,
    bool* out_required
);

/**
 * Obtener estado actual
 *
 * @param ctx Contexto TuxAtlas
 * @return Estado actual
 */
tuxatlas_state_t tuxatlas_get_state(const tuxatlas_ctx_t* ctx);

/**
 * Obtener información de última verificación
 *
 * @param ctx Contexto TuxAtlas
 * @param out_timestamp_ns Timestamp
 * @param out_healthy Healthy
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxatlas_get_last_check_info(
    const tuxatlas_ctx_t* ctx,
    uint64_t* out_timestamp_ns,
    bool* out_healthy
);

/**
 * Destruir contexto TuxAtlas
 *
 * @param ctx Contexto TuxAtlas
 */
void tuxatlas_destroy(tuxatlas_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_TUXATLAS_H */
