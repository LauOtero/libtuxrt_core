/**
 * kares_checkpoint.c — Checkpoint con CRC32 + ECC BCH(1024,996)
 *
 * Implementación de checkpoint periódico del estado de la impresión:
 *   - Guarda posición, temperaturas, línea actual en un archivo
 *   - Usa CRC32 para detección de errores
 *   - Aplica ECC BCH(1024,996) para corrección de hasta 3 bits
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#define TUXRT_CORE_EXPORTS
#include "kares_checkpoint.h"
#include "kares_ecc.h"
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ============================================================================
 * Constantes de checkpoint
 * ============================================================================ */

#define KARES_CHECKPOINT_FILE "kares_checkpoint.bin"
#define KARES_CHECKPOINT_MAGIC 0xDEADBEEF
#define KARES_CHECKPOINT_VERSION 1

/* ============================================================================
 * Funciones auxiliares
 * ============================================================================ */

static uint32_t kares_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        crc ^= (uint32_t)byte << 24;
        
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ 0x04C11DB7;
            } else {
                crc <<= 1;
            }
        }
    }
    
    return ~crc;
}

static void kares_write_header(int fd, uint32_t magic, uint32_t version) {
    uint32_t header[2];
    header[0] = magic;
    header[1] = version;

    write(fd, header, sizeof(header));
}

static void kares_read_data(int fd, kares_checkpoint_data_t* data) {
    /* El struct está empaquetado y mide KARES_CHECKPOINT_DATA_SIZE bytes,
     * por lo que se serializa en una sola operación (incluye el relleno). */
    read(fd, data, sizeof(*data));
}

static void kares_write_data(int fd, const kares_checkpoint_data_t* data) {
    write(fd, data, sizeof(*data));
}

static int kares_check_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Obtener timestamp actual en nanosegundos */
static inline int64_t kares_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ============================================================================
 * Implementación de API
 * ============================================================================ */

tuxrt_err_t tuxrt_kares_checkpoint_create(
    const char* checkpoint_path,
    uint32_t interval_secs,
    kares_checkpoint_ctx_t** out_ctx
) {
    if (!out_ctx) return TUXRT_ERR_INVAL;
    
    kares_checkpoint_ctx_t* ctx = calloc(1, sizeof(kares_checkpoint_ctx_t));
    if (!ctx) return TUXRT_ERR_NOMEM;
    
    ctx->checkpoint_path = strdup(checkpoint_path);
    if (!ctx->checkpoint_path) {
        free(ctx);
        return TUXRT_ERR_NOMEM;
    }
    
    ctx->interval_secs = interval_secs;
    ctx->last_checkpoint_ns = 0;
    ctx->checkpoint_count = 0;
    ctx->initialized = true;
    
    *out_ctx = ctx;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_checkpoint_save(
    kares_checkpoint_ctx_t* ctx,
    const kares_checkpoint_data_t* data
) {
    if (!ctx || !data) return TUXRT_ERR_INVAL;
    
    /* Verificar si es hora de guardar */
    uint64_t now = kares_get_time_ns();
    if (now - ctx->last_checkpoint_ns < ctx->interval_secs * 1000000000LL) {
        return TUXRT_OK;  /* No es tiempo de guardar todavía */
    }
    
    /* Crear archivo temporal */
    char temp_path[256];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", ctx->checkpoint_path);
    
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return TUXRT_ERR_IO;
    }
    
    /* Escribir encabezado */
    kares_write_header(fd, KARES_CHECKPOINT_MAGIC, KARES_CHECKPOINT_VERSION);
    
    /* Calcular ECC sobre el payload completo (struct packed = KARES_ECC_DATA_SIZE) */
    uint8_t ecc[KARES_ECC_PARITY_SIZE];
    tuxrt_err_t err = tuxrt_kares_ecc_encode((const uint8_t*)data, sizeof(*data), ecc);
    if (err != TUXRT_OK) {
        close(fd);
        remove(temp_path);
        return err;
    }

    /* Escribir datos protegidos (KARES_CHECKPOINT_DATA_SIZE) + paridad ECC */
    kares_write_data(fd, data);
    write(fd, ecc, sizeof(ecc));
    
    /* Cerrar y renombrar */
    close(fd);
    
    if (rename(temp_path, ctx->checkpoint_path) < 0) {
        remove(temp_path);
        return TUXRT_ERR_IO;
    }
    
    ctx->last_checkpoint_ns = now;
    ctx->checkpoint_count++;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_checkpoint_load(
    kares_checkpoint_ctx_t* ctx,
    kares_checkpoint_data_t* out_data,
    bool* out_corrected
) {
    if (!ctx || !out_data || !out_corrected) return TUXRT_ERR_INVAL;
    
    /* Verificar si existe el archivo */
    if (!kares_check_file_exists(ctx->checkpoint_path)) {
        *out_corrected = false;
        return TUXRT_ERR_FILE_NOT_FOUND;
    }
    
    int fd = open(ctx->checkpoint_path, O_RDONLY);
    if (fd < 0) {
        return TUXRT_ERR_IO;
    }
    
    /* Leer encabezado */
    uint32_t magic, version;
    read(fd, &magic, sizeof(magic));
    read(fd, &version, sizeof(version));
    
    if (magic != KARES_CHECKPOINT_MAGIC || version != KARES_CHECKPOINT_VERSION) {
        close(fd);
        return TUXRT_ERR_WAL_CORRUPT;
    }
    
    /* Leer datos protegidos */
    kares_checkpoint_data_t data;
    kares_read_data(fd, &data);
    
    /* Leer ECC */
    uint8_t ecc[KARES_ECC_PARITY_SIZE];
    if (read(fd, ecc, sizeof(ecc)) != (ssize_t)sizeof(ecc)) {
        close(fd);
        return TUXRT_ERR_IO;
    }
    
    /* Validar ECC */
    uint32_t error_bits = 0;
    bool correctable = false;
    tuxrt_err_t err = tuxrt_kares_ecc_decode((const uint8_t*)&data, sizeof(data), ecc,
                                             &error_bits, &correctable);
    if (err != TUXRT_OK) {
        close(fd);
        return err;
    }

    *out_corrected = false;
    if (error_bits > 0) {
        if (!correctable) {
            close(fd);
            return TUXRT_ERR_ECC_UNCORRECTABLE;
        }
        err = tuxrt_kares_ecc_correct((uint8_t*)&data, sizeof(data), ecc);
        if (err != TUXRT_OK) {
            close(fd);
            return err;
        }
        *out_corrected = true;
    }

    *out_data = data;
    close(fd);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_checkpoint_exists(
    kares_checkpoint_ctx_t* ctx,
    bool* out_available
) {
    if (!ctx || !out_available) return TUXRT_ERR_INVAL;
    
    *out_available = kares_check_file_exists(ctx->checkpoint_path);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_checkpoint_delete(kares_checkpoint_ctx_t* ctx) {
    if (!ctx) return TUXRT_ERR_INVAL;
    
    if (remove(ctx->checkpoint_path) < 0) {
        return TUXRT_ERR_IO;
    }
    
    free(ctx->checkpoint_path);
    free(ctx);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_checkpoint_get_info(
    const kares_checkpoint_ctx_t* ctx,
    uint64_t* out_timestamp_ns,
    uint32_t* out_count
) {
    if (!ctx || !out_timestamp_ns || !out_count) return TUXRT_ERR_INVAL;

    struct stat st;
    if (stat(ctx->checkpoint_path, &st) != 0) {
        return TUXRT_ERR_IO;
    }

    *out_timestamp_ns = (uint64_t)st.st_mtime;
    *out_count = ctx->checkpoint_count;
    return TUXRT_OK;
}

void tuxrt_kares_checkpoint_destroy(kares_checkpoint_ctx_t* ctx) {
    if (ctx) {
        free(ctx->checkpoint_path);
        free(ctx);
    }
}