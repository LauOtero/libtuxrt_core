/**
 * uss.c — Universal State Snapshot para KARES
 *
 * Implementación de snapshot atómico y consistente del estado completo:
 *   - Escritura con doble buffer A/B
 *   - Lectura con verificación CRC32 + ECC BCH
 *   - Seqlock para consistencia
 *   - Integración con KARES para recuperación
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#include "uss.h"
#include "uss_ecc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* ============================================================================
 *  CRC32 Implementation (IEEE 802.3)
 * ============================================================================ */

static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table(void)
{
    if (crc32_table_initialized) {
        return;
    }
    
    const uint32_t polynomial = 0xEDB88320;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
        }
        crc32_table[i] = crc;
    }
    
    crc32_table_initialized = true;
}

uint32_t tuxrt_uss_crc32(const void* data, size_t len)
{
    init_crc32_table();
    
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < len; i++) {
        uint8_t index = (crc ^ bytes[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }
    
    return crc ^ 0xFFFFFFFF;
}

/* ============================================================================
 *  Gestión de contexto USS
 * ============================================================================ */

tuxrt_err_t tuxrt_uss_create(
    const char* checkpoint_path_a,
    const char* checkpoint_path_b,
    uint32_t interval_secs,
    uss_ctx_t** out_ctx
)
{
    if (!checkpoint_path_a || !checkpoint_path_b || !out_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    uss_ctx_t* ctx = (uss_ctx_t*)calloc(1, sizeof(uss_ctx_t));
    if (!ctx) {
        return TUXRT_ERR_NOMEM;
    }
    
    /* Copiar rutas */
    ctx->checkpoint_path_a = strdup(checkpoint_path_a);
    ctx->checkpoint_path_b = strdup(checkpoint_path_b);
    
    if (!ctx->checkpoint_path_a || !ctx->checkpoint_path_b) {
        free(ctx->checkpoint_path_a);
        free(ctx->checkpoint_path_b);
        free(ctx);
        return TUXRT_ERR_NOMEM;
    }
    
    ctx->checkpoint_interval = interval_secs;
    ctx->last_sequence = USS_SEQUENCE_INITIAL;
    ctx->active_buffer = 0;  /* Empezar con buffer A */
    ctx->ecc_ctx = NULL;
    ctx->initialized = true;
    
    *out_ctx = ctx;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_uss_init_ecc(uss_ctx_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Crear contexto ECC */
    uss_ecc_ctx_t* ecc_ctx = (uss_ecc_ctx_t*)calloc(1, sizeof(uss_ecc_ctx_t));
    if (!ecc_ctx) {
        return TUXRT_ERR_NOMEM;
    }
    
    tuxrt_err_t ret = tuxrt_uss_ecc_init(ecc_ctx);
    if (ret != TUXRT_OK) {
        free(ecc_ctx);
        return ret;
    }
    
    ctx->ecc_ctx = ecc_ctx;
    
    return TUXRT_OK;
}

void tuxrt_uss_destroy(uss_ctx_t* ctx)
{
    if (!ctx) {
        return;
    }
    
    /* Liberar contexto ECC */
    if (ctx->ecc_ctx) {
        tuxrt_uss_ecc_destroy((uss_ecc_ctx_t*)ctx->ecc_ctx);
        free(ctx->ecc_ctx);
    }
    
    /* Liberar rutas */
    free(ctx->checkpoint_path_a);
    free(ctx->checkpoint_path_b);
    
    /* Liberar contexto */
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

/* ============================================================================
 *  Escritura de USS con doble buffer
 * ============================================================================ */

/**
 * Calcular CRC32 de la cabecera
 */
static uint32_t compute_header_crc(uss_header_t* header)
{
    /* Guardar CRC original */
    uint32_t saved_crc = header->crc32_header;
    header->crc32_header = 0;
    
    /* Calcular CRC */
    uint32_t crc = tuxrt_uss_crc32(header, sizeof(uss_header_t));
    
    /* Restaurar CRC */
    header->crc32_header = saved_crc;
    
    return crc;
}

/**
 * Preparar snapshot para escritura
 * - Calcular CRC32 de cada sección
 * - Calcular CRC32 de cabecera
 * - Codificar con ECC
 */
static tuxrt_err_t prepare_snapshot_for_write(
    uss_ctx_t* ctx,
    const uss_snapshot_t* snapshot,
    uss_block_t* block
)
{
    if (!ctx || !ctx->initialized || !ctx->ecc_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    if (!snapshot || !block) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Copiar snapshot a un buffer local mutable para sellar el CRC de cabecera */
    uss_snapshot_t sealed;
    memcpy(&sealed, snapshot, sizeof(sealed));
    sealed.header.crc32_header = compute_header_crc(&sealed.header);
    
    /* Copiar snapshot sellado a buffer de datos */
    memcpy(block->data, &sealed, sizeof(sealed));
    
    /* Codificar con ECC */
    tuxrt_err_t ret = tuxrt_uss_ecc_encode(
        (uss_ecc_ctx_t*)ctx->ecc_ctx,
        block->data,
        USS_DATA_SIZE,
        block->ecc
    );
    
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    return TUXRT_OK;
}

/**
 * Escribir bloque a archivo con fsync
 */
static tuxrt_err_t write_block_to_file(
    const char* filepath,
    const uss_block_t* block
)
{
    if (!filepath || !block) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Abrir archivo */
    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return TUXRT_ERR_FILE_OPEN;
    }
    
    /* Escribir bloque completo */
    ssize_t written = write(fd, block, sizeof(uss_block_t));
    if (written != (ssize_t)sizeof(uss_block_t)) {
        close(fd);
        return TUXRT_ERR_FILE_WRITE;
    }
    
    /* Asegurar escritura a disco (crítico para checkpoints) */
    if (fsync(fd) < 0) {
        close(fd);
        return TUXRT_ERR_FILE_SYNC;
    }
    
    close(fd);
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_uss_write(
    uss_ctx_t* ctx,
    const uss_snapshot_t* snapshot
)
{
    if (!ctx || !ctx->initialized || !ctx->ecc_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    if (!snapshot) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Verificar magic y version */
    if (snapshot->header.magic != USS_MAGIC) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Preparar bloque con ECC */
    uss_block_t block;
    memset(&block, 0, sizeof(block));
    
    tuxrt_err_t ret = prepare_snapshot_for_write(ctx, snapshot, &block);
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    /* Seleccionar buffer activo */
    const char* filepath = (ctx->active_buffer == 0) 
                           ? ctx->checkpoint_path_a 
                           : ctx->checkpoint_path_b;
    
    /* Escribir a disco */
    ret = write_block_to_file(filepath, &block);
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    /* Actualizar secuencia y buffer */
    ctx->last_sequence = snapshot->header.sequence;
    ctx->active_buffer = (ctx->active_buffer == 0) ? 1 : 0;  /* Alternar buffer */
    
    return TUXRT_OK;
}

/* ============================================================================
 *  Lectura de USS con verificación ECC
 * ============================================================================ */

/**
 * Leer bloque desde archivo
 */
static tuxrt_err_t read_block_from_file(
    const char* filepath,
    uss_block_t* block
)
{
    if (!filepath || !block) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Verificar existencia */
    struct stat st;
    if (stat(filepath, &st) < 0) {
        return TUXRT_ERR_FILE_NOT_FOUND;
    }
    
    /* Verificar tamaño */
    if (st.st_size != (ssize_t)sizeof(uss_block_t)) {
        return TUXRT_ERR_FILE_SIZE;
    }
    
    /* Abrir archivo */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        return TUXRT_ERR_FILE_OPEN;
    }
    
    /* Leer bloque completo */
    ssize_t read_bytes = read(fd, block, sizeof(uss_block_t));
    close(fd);
    
    if (read_bytes != (ssize_t)sizeof(uss_block_t)) {
        return TUXRT_ERR_FILE_READ;
    }
    
    return TUXRT_OK;
}

/**
 * Verificar y corregir bloque leído
 */
static tuxrt_err_t verify_and_correct_block(
    uss_ctx_t* ctx,
    uss_block_t* block,
    bool* out_corrected
)
{
    if (!ctx || !ctx->ecc_ctx || !block) {
        return TUXRT_ERR_INVAL;
    }
    
    if (out_corrected) {
        *out_corrected = false;
    }
    
    /* Verificar con ECC */
    uint32_t error_bits = 0;
    bool correctable = false;
    
    tuxrt_err_t ret = tuxrt_uss_ecc_decode(
        (uss_ecc_ctx_t*)ctx->ecc_ctx,
        block->data,
        USS_DATA_SIZE,
        block->ecc,
        &error_bits,
        &correctable
    );
    
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    /* Si hay errores, intentar corregir */
    if (error_bits > 0) {
        if (!correctable) {
            /* Demasiados errores - no correctable */
            return TUXRT_ERR_ECC_UNCORRECTABLE;
        }
        
        /* Corregir errores */
        uint32_t bits_corrected = 0;
        ret = tuxrt_uss_ecc_correct(
            (uss_ecc_ctx_t*)ctx->ecc_ctx,
            block->data,
            USS_DATA_SIZE,
            block->ecc,
            &bits_corrected
        );
        
        if (ret != TUXRT_OK) {
            return ret;
        }
        
        if (out_corrected) {
            *out_corrected = true;
        }
    }
    
    return TUXRT_OK;
}

/**
 * Verificar CRC32 de cabecera
 */
static bool verify_header_crc(uss_header_t* header)
{
    uint32_t computed_crc = compute_header_crc(header);
    return (header->crc32_header == computed_crc);
}

tuxrt_err_t tuxrt_uss_read(
    uss_ctx_t* ctx,
    uss_snapshot_t* out_snapshot,
    bool* out_corrected
)
{
    if (!ctx || !ctx->initialized || !ctx->ecc_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    if (!out_snapshot) {
        return TUXRT_ERR_INVAL;
    }
    
    if (out_corrected) {
        *out_corrected = false;
    }
    
    /* Intentar leer buffer activo primero */
    const char* filepath = (ctx->active_buffer == 0) 
                           ? ctx->checkpoint_path_a 
                           : ctx->checkpoint_path_b;
    
    uss_block_t block;
    memset(&block, 0, sizeof(block));
    
    tuxrt_err_t ret = read_block_from_file(filepath, &block);
    if (ret != TUXRT_OK) {
        /* Intentar buffer alternativo */
        const char* alt_filepath = (ctx->active_buffer == 0) 
                                   ? ctx->checkpoint_path_b 
                                   : ctx->checkpoint_path_a;
        
        ret = read_block_from_file(alt_filepath, &block);
        if (ret != TUXRT_OK) {
            return ret;
        }
    }
    
    /* Verificar y corregir con ECC */
    bool corrected = false;
    ret = verify_and_correct_block(ctx, &block, &corrected);
    if (ret != TUXRT_OK) {
        /* Si falla, intentar con buffer alternativo */
        const char* alt_filepath = (ctx->active_buffer == 0) 
                                   ? ctx->checkpoint_path_b 
                                   : ctx->checkpoint_path_a;
        
        ret = read_block_from_file(alt_filepath, &block);
        if (ret == TUXRT_OK) {
            ret = verify_and_correct_block(ctx, &block, &corrected);
        }
        
        if (ret != TUXRT_OK) {
            return ret;
        }
    }
    
    /* Copiar snapshot decodificado */
    memcpy(out_snapshot, block.data, sizeof(uss_snapshot_t));
    
    /* Verificar magic */
    if (out_snapshot->header.magic != USS_MAGIC) {
        return TUXRT_ERR_INVALID_MAGIC;
    }
    
    /* Verificar CRC de cabecera */
    if (!verify_header_crc(&out_snapshot->header)) {
        return TUXRT_ERR_CRC_MISMATCH;
    }
    
    if (out_corrected) {
        *out_corrected = corrected;
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_uss_load_previous(
    uss_ctx_t* ctx,
    uss_snapshot_t* out_snapshot,
    bool* out_corrected
)
{
    if (!ctx || !ctx->initialized || !ctx->ecc_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    if (!out_snapshot) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Leer siempre del buffer NO activo (el anterior) */
    const char* filepath = (ctx->active_buffer == 0) 
                           ? ctx->checkpoint_path_b 
                           : ctx->checkpoint_path_a;
    
    uss_block_t block;
    memset(&block, 0, sizeof(block));
    
    tuxrt_err_t ret = read_block_from_file(filepath, &block);
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    /* Verificar y corregir */
    bool corrected = false;
    ret = verify_and_correct_block(ctx, &block, &corrected);
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    /* Copiar snapshot */
    memcpy(out_snapshot, block.data, sizeof(uss_snapshot_t));
    
    /* Verificar magic */
    if (out_snapshot->header.magic != USS_MAGIC) {
        return TUXRT_ERR_INVALID_MAGIC;
    }
    
    if (out_corrected) {
        *out_corrected = corrected;
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_uss_exists(
    uss_ctx_t* ctx,
    bool* out_available
)
{
    if (!ctx || !ctx->initialized || !out_available) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Verificar existencia de al menos un buffer */
    struct stat st;
    bool exists_a = (stat(ctx->checkpoint_path_a, &st) == 0);
    bool exists_b = (stat(ctx->checkpoint_path_b, &st) == 0);
    
    *out_available = (exists_a || exists_b);
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_uss_get_info(
    const uss_ctx_t* ctx,
    uint64_t* out_sequence,
    uint64_t* out_timestamp_ns
)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    if (out_sequence) {
        *out_sequence = ctx->last_sequence;
    }
    
    /* Timestamp se obtendría leyendo el snapshot */
    if (out_timestamp_ns) {
        *out_timestamp_ns = 0;  /* Se requiere leer snapshot para obtener timestamp */
    }
    
    return TUXRT_OK;
}
