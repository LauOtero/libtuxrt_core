/**
 * kares_checkpoint.h — KARES Checkpoint con CRC32 + ECC BCH(1024,996)
 *
 * Sistema de checkpoint protegido para recuperación tras fallo:
 *   - CRC32 para detección de errores
 *   - ECC BCH(1024,996) para corrección de hasta 3 bits
 *   - Checkpoint periódico del estado de la impresión
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_KARES_CHECKPOINT_H
#define TUXRT_KARES_CHECKPOINT_H

#include "../../sdk/include/tuxrt_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Tamaño de datos útiles en checkpoint (bytes)
 * BCH(1024,996): 996 bits de datos = 124 bytes.
 * Debe coincidir con KARES_ECC_DATA_SIZE de kares_ecc.h.
 */
#define KARES_CHECKPOINT_DATA_SIZE 124
#define KARES_CHECKPOINT_CRC_SIZE 4
#define KARES_CHECKPOINT_ECC_SIZE 28  /* 1024 - 996 = 28 bits de paridad */
#define KARES_CHECKPOINT_TOTAL_SIZE (KARES_CHECKPOINT_DATA_SIZE + KARES_CHECKPOINT_CRC_SIZE + KARES_CHECKPOINT_ECC_SIZE)

/**
 * Estructura de checkpoint (sin ECC)
 */
typedef struct {
    uint32_t magic;           /* 0xDEADBEEF para validación */
    uint32_t version;         /* Versión de estructura */
    uint32_t line_number;     /* Línea actual del G-code */
    uint32_t layer_number;    /* Capa actual */
    uint32_t job_id;          /* ID del trabajo */
    
    /* Posición actual */
    double position_x;
    double position_y;
    double position_z;
    double position_e;
    
    /* Temperaturas */
    double hotend_target;
    double bed_target;
    
    /* Timestamp */
    uint64_t timestamp_ns;
    
    /* Flags */
    uint32_t flags;
    
    /* CRC32 de los datos anteriores */
    uint32_t crc32;
    
    /* Relleno para completar KARES_CHECKPOINT_DATA_SIZE bytes */
    uint8_t reserved[KARES_CHECKPOINT_DATA_SIZE - (4*9 + 8*6)];
} __attribute__((packed)) kares_checkpoint_data_t;

/* El layout serializado debe coincidir con el tamaño que protege la ECC */
_Static_assert(sizeof(kares_checkpoint_data_t) == KARES_CHECKPOINT_DATA_SIZE,
               "kares_checkpoint_data_t debe medir KARES_CHECKPOINT_DATA_SIZE bytes");

/**
 * Checkpoint completo con ECC
 */
typedef struct {
    uint8_t data[KARES_CHECKPOINT_DATA_SIZE];  /* Datos + CRC32 */
    uint8_t ecc[KARES_CHECKPOINT_ECC_SIZE];    /* Paridad BCH */
} kares_checkpoint_with_ecc_t;

/**
 * Contexto de gestión de checkpoints
 */
typedef struct kares_checkpoint_ctx {
    char* checkpoint_path;      /* Ruta al archivo de checkpoint */
    uint32_t interval_secs;     /* Intervalo entre checkpoints */
    uint64_t last_checkpoint_ns;/* Timestamp del último checkpoint */
    uint32_t checkpoint_count;  /* Número de checkpoints creados */
    bool initialized;
} kares_checkpoint_ctx_t;

/**
 * Crear contexto de checkpoint
 *
 * @param checkpoint_path Ruta al archivo de checkpoint
 * @param interval_secs Intervalo entre checkpoints (segundos)
 * @param out_ctx Contexto resultante
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_checkpoint_create(
    const char* checkpoint_path,
    uint32_t interval_secs,
    kares_checkpoint_ctx_t** out_ctx
);

/**
 * Guardar checkpoint protegido con CRC32 + ECC
 *
 * @param ctx Contexto de checkpoint
 * @param data Datos a guardar
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_checkpoint_save(
    kares_checkpoint_ctx_t* ctx,
    const kares_checkpoint_data_t* data
);

/**
 * Cargar checkpoint y validar CRC32 + ECC
 *
 * @param ctx Contexto de checkpoint
 * @param out_data Datos cargados
 * @param out_corrected Salida: true si se corrigieron errores con ECC
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_checkpoint_load(
    kares_checkpoint_ctx_t* ctx,
    kares_checkpoint_data_t* out_data,
    bool* out_corrected
);

/**
 * Verificar si hay un checkpoint disponible
 *
 * @param ctx Contexto de checkpoint
 * @param out_available Salida: true si hay checkpoint
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_checkpoint_exists(
    kares_checkpoint_ctx_t* ctx,
    bool* out_available
);

/**
 * Eliminar checkpoint
 *
 * @param ctx Contexto de checkpoint
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_checkpoint_delete(kares_checkpoint_ctx_t* ctx);

/**
 * Obtener información del último checkpoint
 *
 * @param ctx Contexto de checkpoint
 * @param out_timestamp Timestamp del último checkpoint
 * @param out_count Número de checkpoints
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_checkpoint_get_info(
    const kares_checkpoint_ctx_t* ctx,
    uint64_t* out_timestamp_ns,
    uint32_t* out_count
);

/**
 * Destruir contexto de checkpoint
 *
 * @param ctx Contexto de checkpoint
 */
void tuxrt_kares_checkpoint_destroy(kares_checkpoint_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_KARES_CHECKPOINT_H */
