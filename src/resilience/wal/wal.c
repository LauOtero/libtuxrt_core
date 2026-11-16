/**
 * wal.c — Write-Ahead Log (WAL) Persistencia
 * 
 * Log de operaciones para recuperación atómica ante fallos
 * Implementación M11: Log con segmentación, verificación CRC y replay
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <zlib.h>  // Para crc32
#include <limits.h>

// Binary WAL entry header (must match on-disk format exactly)
#pragma pack(push, 1)
/**
 * Tipos de entrada WAL según especificación KARES
 * 11 tipos definidos en el informe completo
 */
enum wal_entry_type {
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
};

struct wal_entry_header {
    uint64_t sequence;
    uint32_t type;
    tuxrt_timestamp_ns_t timestamp;
    uint32_t key_len;
    uint32_t val_len;
    uint32_t crc32;
};
#pragma pack(pop)

struct tuxrt_wal_t {
    int fd;
    char* path;
    uint64_t sequence;
    pthread_mutex_t mutex;
    size_t max_segment_bytes;
    char* segment_path;
    uint64_t entries_written;
    uint64_t entries_replayed;
    bool group_commit_enabled;  // Si true, append() no llama fsync; se usa tuxrt_wal_checkpoint()
    bool immediate_fsync;       // Si true, llama fsync en cada append() para durabilidad inmediata
};

tuxrt_err_t tuxrt_wal_create(const char* path, size_t max_segment_mb, tuxrt_wal_t** out_wal) {
    if (!path || !out_wal) return TUXRT_ERR_INVAL;

    if (max_segment_mb == 0 || max_segment_mb > 1024) {
        return TUXRT_ERR_INVAL;
    }

    tuxrt_wal_t* w = calloc(1, sizeof(tuxrt_wal_t));
    if (!w) return TUXRT_ERR_NOMEM;

    w->path = strdup(path);
    if (!w->path) { free(w); return TUXRT_ERR_NOMEM; }

    // Create segment path
    char seg_path[PATH_MAX];
    snprintf(seg_path, sizeof(seg_path), "%s.segment.000000", path);
    w->segment_path = strdup(seg_path);
    if (!w->segment_path) {
        free(w->path); free(w);
        return TUXRT_ERR_NOMEM;
    }

    w->fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (w->fd < 0) {
        free(w->segment_path);
        free(w->path);
        free(w);
        return TUXRT_ERR_IO;
    }

    w->sequence = 0;
    w->max_segment_bytes = max_segment_mb * 1024 * 1024;
    w->entries_written = 0;
    w->entries_replayed = 0;
    w->group_commit_enabled = true; // Por defecto, habilitar group commit para los objetivos de almacenamiento
    pthread_mutex_init(&w->mutex, NULL);

    // Recover sequence from file leyendo entradas binarias reales
    struct stat st;
    if (fstat(w->fd, &st) == 0 && st.st_size > 0) {
        // Repasar entradas binarias para obtener secuencia y entries_written
        off_t pos = 0;
        uint64_t written_count = 0;
        while (pos < st.st_size) {
            if (lseek(w->fd, pos, SEEK_SET) == -1) break;
            struct wal_entry_header header;
            ssize_t n = read(w->fd, &header, sizeof(header));
            if (n != sizeof(header)) break; // Truncado o EOF
            // Verificar CRC (opcional, se validará en replay)
            pos += sizeof(header) + header.key_len + header.val_len;
            written_count++;
        }
        w->entries_written = written_count;
        w->sequence = written_count;
    }

    *out_wal = w;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_wal_append(tuxrt_wal_t* wal, tuxrt_wal_op_type_t type, const char* key, const char* value) {
    if (!wal || !key || !value) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&wal->mutex);

    // Check size limit
    struct stat st;
    if (fstat(wal->fd, &st) == 0 && (size_t)st.st_size >= wal->max_segment_bytes) {
        // Rotate segment
        char new_path[PATH_MAX];
        snprintf(new_path, sizeof(new_path), "%s.segment.%06llu",
                 wal->path, (unsigned long long)wal->entries_written);
        rename(wal->path, new_path);

        // Recreate file
        close(wal->fd);
        wal->fd = open(wal->path, O_RDWR | O_CREAT | O_APPEND, 0644);
        if (wal->fd < 0) {
            pthread_mutex_unlock(&wal->mutex);
            return TUXRT_ERR_IO;
        }
    }

    wal->sequence++;
    wal->entries_written++;

    // Build entry with timestamp
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    tuxrt_timestamp_ns_t timestamp = (tuxrt_timestamp_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    // Calculate CRC32
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef*)key, strlen(key));
    crc = crc32(crc, (const Bytef*)value, strlen(value));
    crc = crc32(crc, (const Bytef*)&timestamp, sizeof(timestamp));

    // Write as binary format for efficiency
    struct wal_entry_header header = {
        .sequence = wal->sequence,
        .type = type,
        .timestamp = timestamp,
        .key_len = strlen(key),
        .val_len = strlen(value),
        .crc32 = crc
    };

    // Write header
    if (write(wal->fd, &header, sizeof(header)) != sizeof(header)) {
        pthread_mutex_unlock(&wal->mutex);
        return TUXRT_ERR_IO;
    }

    // Write key and value
    if (write(wal->fd, key, header.key_len) != header.key_len ||
        write(wal->fd, value, header.val_len) != header.val_len) {
        pthread_mutex_unlock(&wal->mutex);
        return TUXRT_ERR_IO;
    }

    // Durabilidad: si group_commit está habilitado, NO hacer fsync aquí.
    // Si immediate_fsync está habilitado, hacer fsync inmediato para durabilidad garantizada.
    // El usuario debe llamar a tuxrt_wal_checkpoint() para persistencia completa (durabilidad + metadatos).
    if (wal->group_commit_enabled) {
        if (wal->immediate_fsync) {
            fsync(wal->fd);
        }
    } else {
        // group_commit deshabilitado: fsync siempre
        fsync(wal->fd);
    }

    pthread_mutex_unlock(&wal->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_wal_replay(tuxrt_wal_t* wal, tuxrt_wal_replay_fn_t fn, void* user_data) {
    if (!wal || !fn) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&wal->mutex);

    // Seek to start
    if (lseek(wal->fd, 0, SEEK_SET) == (off_t)-1) {
        pthread_mutex_unlock(&wal->mutex);
        return TUXRT_ERR_IO;
    }

    // Repasar entradas binarias secuenciales
    uint64_t processed = 0;
    while (1) {
        struct wal_entry_header header;
        ssize_t n = read(wal->fd, &header, sizeof(header));
        if (n != sizeof(header)) {
            // EOF o archivo truncado; detener replay
            break;
        }

        // Crear entrada con buffer para key y value
        tuxrt_wal_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.sequence = header.sequence;
        entry.type = header.type;
        entry.timestamp = header.timestamp;
        entry.crc32 = header.crc32;

        // Leer key
        if (header.key_len > 0) {
            char* key_buf = malloc(header.key_len + 1);
            if (!key_buf) {
                // Memory error: stop replay but log processed
                break;
            }
            if (read(wal->fd, key_buf, header.key_len) != (ssize_t)header.key_len) {
                free(key_buf);
                break;
            }
            key_buf[header.key_len] = '\0';
            entry.key = key_buf;
        }

        // Leer value
        if (header.val_len > 0) {
            char* val_buf = malloc(header.val_len + 1);
            if (!val_buf) {
                free((void*)entry.key);
                break;
            }
            if (read(wal->fd, val_buf, header.val_len) != (ssize_t)header.val_len) {
                free((void*)entry.key);
                free(val_buf);
                break;
            }
            val_buf[header.val_len] = '\0';
            entry.value = val_buf;
        }

        // Verificar CRC32
        uint32_t crc = crc32(0L, Z_NULL, 0);
        const char* k = entry.key ? entry.key : "";
        const char* v = entry.value ? entry.value : "";
        crc = crc32(crc, (const Bytef*)k, strlen(k));
        crc = crc32(crc, (const Bytef*)v, strlen(v));
        crc = crc32(crc, (const Bytef*)&header.timestamp, sizeof(header.timestamp));

        if (crc != header.crc32) {
            // CRC mismatch: este entry está corrupto. Puedes decidir si detenerte
            // o continuar. Aquí decidimos detener el replay para evitar daños.
            free((void*)entry.key);
            free((void*)entry.value);
            break;
        }

        // Llamar al callback con la entrada
        fn(&entry, user_data);
        processed++;

        // Liberar buffers de entrada
        free((void*)entry.key);
        free((void*)entry.value);
    }

    // Actualizar contador de entries_replayed (fuera del mutex para evitar contention extrema,
    // o mantenerlo dentro si prefieres exactitud bajo lock)
    wal->entries_replayed = processed;

    pthread_mutex_unlock(&wal->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_wal_checkpoint(tuxrt_wal_t* wal) {
    if (!wal) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&wal->mutex);
    // fsync del archivo principal
    int ret = fsync(wal->fd);
    pthread_mutex_unlock(&wal->mutex);

    if (ret != 0) return TUXRT_ERR_IO;
    return TUXRT_OK;
}

// Función para habilitar/deshabilitar group commit
tuxrt_err_t tuxrt_wal_set_group_commit(tuxrt_wal_t* wal, bool enabled) {
    if (!wal) return TUXRT_ERR_INVAL;
    pthread_mutex_lock(&wal->mutex);
    wal->group_commit_enabled = enabled;
    pthread_mutex_unlock(&wal->mutex);
    return TUXRT_OK;
}

// Función para habilitar/deshabilitar immediate fsync (durabilidad por cada append)
tuxrt_err_t tuxrt_wal_set_immediate_fsync(tuxrt_wal_t* wal, bool enabled) {
    if (!wal) return TUXRT_ERR_INVAL;
    pthread_mutex_lock(&wal->mutex);
    wal->immediate_fsync = enabled;
    pthread_mutex_unlock(&wal->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_wal_verify(tuxrt_wal_t* wal, bool* out_ok) {
    if (!wal || !out_ok) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&wal->mutex);

    // Seek to start
    if (lseek(wal->fd, 0, SEEK_SET) == (off_t)-1) {
        pthread_mutex_unlock(&wal->mutex);
        *out_ok = false;
        return TUXRT_ERR_IO;
    }

    bool ok = true;
    struct stat st;
    if (fstat(wal->fd, &st) == 0 && st.st_size > 0) {
        off_t pos = 0;
        while (pos < st.st_size) {
            if (lseek(wal->fd, pos, SEEK_SET) == -1) {
                ok = false;
                break;
            }
            struct wal_entry_header header;
            ssize_t n = read(wal->fd, &header, sizeof(header));
            if (n != sizeof(header)) {
                ok = false;
                break;
            }
            pos += sizeof(header) + header.key_len + header.val_len;
            
            // Skip key/value data for CRC check
            if (lseek(wal->fd, pos, SEEK_SET) == -1) {
                ok = false;
                break;
            }
        }
    }

    *out_ok = ok;
    pthread_mutex_unlock(&wal->mutex);
    return TUXRT_OK;
}

void tuxrt_wal_destroy(tuxrt_wal_t* wal) {
    if (!wal) return;
    pthread_mutex_destroy(&wal->mutex);
    if (wal->fd >= 0) close(wal->fd);
    free(wal->path);
    free(wal->segment_path);
    free(wal);
}