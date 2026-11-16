/**
 * tuxatlas.c — Implementación de TuxAtlas Rollback con Golden Image
 *
 * Características:
 *   - Golden Image inmutable
 *   - Escritura atómica (write-rename)
 *   - Health check con timeout
 *   - Rollback automático
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#include "tuxatlas.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================================
 *  Utilidades
 * ============================================================================ */

static uint64_t get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int64_t current_time_seconds(void)
{
    return time(NULL);
}

/* ============================================================================
 *  API Implementación
 * ============================================================================ */

tuxrt_err_t tuxatlas_create(
    const char* golden_image_path,
    const char* current_config_path,
    const char* backup_path,
    tuxatlas_ctx_t** out_ctx
)
{
    if (!golden_image_path || !current_config_path || !backup_path || !out_ctx) {
        return TUXRT_ERR_INVAL;
    }
    
    tuxatlas_ctx_t* ctx = (tuxatlas_ctx_t*)calloc(1, sizeof(tuxatlas_ctx_t));
    if (!ctx) {
        return TUXRT_ERR_NOMEM;
    }
    
    ctx->golden_image_path = strdup(golden_image_path);
    ctx->current_config_path = strdup(current_config_path);
    ctx->backup_path = strdup(backup_path);
    
    if (!ctx->golden_image_path || !ctx->current_config_path || !ctx->backup_path) {
        free(ctx->golden_image_path);
        free(ctx->current_config_path);
        free(ctx->backup_path);
        free(ctx);
        return TUXRT_ERR_NOMEM;
    }
    
    ctx->health_check_interval_ms = 5000;
    ctx->rollback_timeout_ms = 10000;
    ctx->state = TUXATLAS_STATE_HEALTHY;
    ctx->initialized = true;
    
    *out_ctx = ctx;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxatlas_init(tuxatlas_ctx_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Verificar existencia de Golden Image */
    struct stat st;
    if (stat(ctx->golden_image_path, &st) != 0) {
        /* Crear Golden Image vacío si no existe */
        int fd = open(ctx->golden_image_path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            return TUXRT_ERR_IO;
        }
        close(fd);
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxatlas_create_golden_image(
    tuxatlas_ctx_t* ctx,
    const void* config_data,
    size_t data_len
)
{
    if (!ctx || !ctx->initialized || !config_data) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Escritura atómica: escribir a archivo temporal y renombrar */
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d", ctx->golden_image_path, getpid());
    
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return TUXRT_ERR_IO;
    }
    
    ssize_t written = write(fd, config_data, data_len);
    close(fd);
    
    if (written != (ssize_t)data_len) {
        unlink(temp_path);
        return TUXRT_ERR_IO;
    }
    
    /* Asegurar escritura a disco */
    fd = open(temp_path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    
    /* Renombrar atómicamente */
    if (rename(temp_path, ctx->golden_image_path) != 0) {
        unlink(temp_path);
        return TUXRT_ERR_IO;
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxatlas_save_config(
    tuxatlas_ctx_t* ctx,
    const void* config_data,
    size_t data_len
)
{
    if (!ctx || !ctx->initialized || !config_data) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Hacer backup de configuración actual */
    if (access(ctx->current_config_path, F_OK) == 0) {
        if (rename(ctx->current_config_path, ctx->backup_path) != 0) {
            /* Backup falló pero continuamos */
        }
    }
    
    /* Escritura atómica */
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d", ctx->current_config_path, getpid());
    
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return TUXRT_ERR_IO;
    }
    
    ssize_t written = write(fd, config_data, data_len);
    close(fd);
    
    if (written != (ssize_t)data_len) {
        unlink(temp_path);
        return TUXRT_ERR_IO;
    }
    
    /* fsync */
    fd = open(temp_path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    
    /* Renombrar atómicamente */
    if (rename(temp_path, ctx->current_config_path) != 0) {
        return TUXRT_ERR_IO;
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxatlas_load_config(
    tuxatlas_ctx_t* ctx,
    void* out_data,
    size_t* out_data_len
)
{
    if (!ctx || !ctx->initialized || !out_data || !out_data_len) {
        return TUXRT_ERR_INVAL;
    }
    
    int fd = open(ctx->current_config_path, O_RDONLY);
    if (fd < 0) {
        return TUXRT_ERR_FILE_NOT_FOUND;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return TUXRT_ERR_IO;
    }
    
    size_t len = (size_t)st.st_size;
    if (len > *out_data_len) {
        close(fd);
        return TUXRT_ERR_NOMEM;
    }
    
    ssize_t read_bytes = read(fd, out_data, len);
    close(fd);
    
    if (read_bytes != (ssize_t)len) {
        return TUXRT_ERR_IO;
    }
    
    *out_data_len = len;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxatlas_health_check(
    tuxatlas_ctx_t* ctx,
    tuxatlas_health_result_t* result
)
{
    if (!ctx || !ctx->initialized || !result) {
        return TUXRT_ERR_INVAL;
    }
    
    memset(result, 0, sizeof(*result));
    result->timestamp_ns = get_timestamp_ns();
    
    /* Check 1: Golden Image existe */
    struct stat st;
    bool golden_exists = (stat(ctx->golden_image_path, &st) == 0);
    result->checks_passed += golden_exists ? 1 : 0;
    result->checks_failed += golden_exists ? 0 : 1;
    
    /* Check 2: Config actual existe */
    bool config_exists = (stat(ctx->current_config_path, &st) == 0);
    result->checks_passed += config_exists ? 1 : 0;
    result->checks_failed += config_exists ? 0 : 1;
    
    /* Check 3: Config es legible */
    bool config_readable = (access(ctx->current_config_path, R_OK) == 0);
    result->checks_passed += config_readable ? 1 : 0;
    result->checks_failed += config_readable ? 0 : 1;
    
    /* Check 4: Config no está vacía */
    bool config_not_empty = false;
    if (config_exists) {
        if (stat(ctx->current_config_path, &st) == 0 && st.st_size > 0) {
            config_not_empty = true;
        }
    }
    result->checks_passed += config_not_empty ? 1 : 0;
    result->checks_failed += config_not_empty ? 0 : 1;
    
    /* Resultado general */
    result->healthy = (result->checks_failed == 0);
    
    if (!result->healthy) {
        snprintf(result->error_message, sizeof(result->error_message),
                "Health check falló: %u fallos", result->checks_failed);
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxatlas_rollback(tuxatlas_ctx_t* ctx)
{
    if (!ctx || !ctx->initialized) {
        return TUXRT_ERR_INVAL;
    }
    
    ctx->state = TUXATLAS_STATE_ROLLING_BACK;
    
    /* Copiar Golden Image a configuración actual */
    int src_fd = open(ctx->golden_image_path, O_RDONLY);
    if (src_fd < 0) {
        ctx->state = TUXATLAS_STATE_FAILED;
        return TUXRT_ERR_FILE_NOT_FOUND;
    }
    
    /* Hacer backup de configuración actual */
    if (access(ctx->current_config_path, F_OK) == 0) {
        rename(ctx->current_config_path, ctx->backup_path);
    }
    
    /* Copiar Golden Image */
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d", ctx->current_config_path, getpid());
    
    int dst_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        close(src_fd);
        ctx->state = TUXATLAS_STATE_FAILED;
        return TUXRT_ERR_IO;
    }
    
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_written = write(dst_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            close(src_fd);
            close(dst_fd);
            unlink(temp_path);
            ctx->state = TUXATLAS_STATE_FAILED;
            return TUXRT_ERR_IO;
        }
    }
    
    close(src_fd);
    close(dst_fd);
    
    /* fsync */
    dst_fd = open(temp_path, O_RDONLY);
    if (dst_fd >= 0) {
        fsync(dst_fd);
        close(dst_fd);
    }
    
    /* Renombrar atómicamente */
    if (rename(temp_path, ctx->current_config_path) != 0) {
        ctx->state = TUXATLAS_STATE_FAILED;
        return TUXRT_ERR_IO;
    }
    
    ctx->state = TUXATLAS_STATE_ROLLBACK_COMPLETE;
    
    /* Volver a estado saludable */
    ctx->state = TUXATLAS_STATE_HEALTHY;
    
    return TUXRT_OK;
}

tuxrt_err_t tuxatlas_check_rollback_needed(
    tuxatlas_ctx_t* ctx,
    bool* out_required
)
{
    if (!ctx || !ctx->initialized || !out_required) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Hacer health check */
    tuxatlas_health_result_t result;
    tuxrt_err_t ret = tuxatlas_health_check(ctx, &result);
    if (ret != TUXRT_OK) {
        return ret;
    }
    
    *out_required = !result.healthy;
    
    return TUXRT_OK;
}

tuxatlas_state_t tuxatlas_get_state(const tuxatlas_ctx_t* ctx)
{
    if (!ctx) {
        return TUXATLAS_STATE_FAILED;
    }
    return ctx->state;
}

tuxrt_err_t tuxatlas_get_last_check_info(
    const tuxatlas_ctx_t* ctx,
    uint64_t* out_timestamp_ns,
    bool* out_healthy
)
{
    if (!ctx || !out_timestamp_ns || !out_healthy) {
        return TUXRT_ERR_INVAL;
    }
    
    *out_timestamp_ns = get_timestamp_ns();
    *out_healthy = (ctx->state == TUXATLAS_STATE_HEALTHY);
    
    return TUXRT_OK;
}

void tuxatlas_destroy(tuxatlas_ctx_t* ctx)
{
    if (!ctx) {
        return;
    }
    
    free(ctx->golden_image_path);
    free(ctx->current_config_path);
    free(ctx->backup_path);
    
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}
