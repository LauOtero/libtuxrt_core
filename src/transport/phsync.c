/**
 * phsync.c — Phase Synchronization (CORE ↔ Mojo)
 * 
 * Sincronización de fase entre CORE (RT) y Mojo
 * Implementación M9: Sincronización con detección de deriva y corrección gradual
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

struct tuxrt_phsync_t {
    tuxrt_phsync_config_t cfg;
    tuxrt_phsync_status_t status;
    pthread_t thread;
    bool running;
    pthread_mutex_t mutex;
    tuxrt_timestamp_ns_t last_check_ns;
    uint64_t sync_count;
    uint64_t error_count;
};

static tuxrt_timestamp_ns_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (tuxrt_timestamp_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void* phsync_thread(void* arg) {
    struct tuxrt_phsync_t* p = (struct tuxrt_phsync_t*)arg;
    struct timespec interval;
    interval.tv_sec = p->cfg.sync_interval_ms / 1000;
    interval.tv_nsec = (p->cfg.sync_interval_ms % 1000) * 1000000;
    
    tuxrt_timestamp_ns_t prev_sync = 0;
    int64_t cumulative_drift = 0;
    
    while (p->running) {
        nanosleep(&interval, NULL);
        
        tuxrt_timestamp_ns_t now = now_ns();
        
        pthread_mutex_lock(&p->mutex);
        if (prev_sync == 0) {
            prev_sync = now;
            p->status.last_sync_ns = now;
            p->status.synchronized = true;
            p->sync_count++;
        } else {
            // Calcular drift real basado en intervalo esperado vs real
            tuxrt_timestamp_ns_t expected_interval = (tuxrt_timestamp_ns_t)p->cfg.sync_interval_ms * 1000000LL;
            tuxrt_timestamp_ns_t actual_interval = now - prev_sync;
            int64_t interval_drift = (int64_t)actual_interval - (int64_t)expected_interval;
            
            cumulative_drift += interval_drift;
            
            // Limitar corrección para evitar saltos bruscos (slew rate limiting)
            int64_t max_correction = p->cfg.drift_tolerance_ns / 10;
            int64_t correction = interval_drift / 10; // Factor de corrección suave
            
            if (correction > max_correction) correction = max_correction;
            if (correction < -max_correction) correction = -max_correction;
            
            p->status.offset_ns += correction;
            p->status.drift_ns = cumulative_drift;
            
            // Verificar si sincronización está dentro de tolerancia
            if (llabs(p->status.drift_ns) <= (int64_t)p->cfg.drift_tolerance_ns) {
                p->status.synchronized = true;
            } else {
                p->status.synchronized = false;
                p->error_count++;
            }
            
            p->status.last_sync_ns = now;
            p->sync_count++;
            prev_sync = now;
        }
        pthread_mutex_unlock(&p->mutex);
    }
    return NULL;
}

tuxrt_err_t tuxrt_phsync_create(const tuxrt_phsync_config_t* cfg, tuxrt_phsync_t** out_phsync) {
    if (!cfg || !out_phsync) return TUXRT_ERR_INVAL;
    
    // Validar configuración
    if (cfg->sync_interval_ms == 0 || cfg->drift_tolerance_ns == 0) {
        return TUXRT_ERR_INVAL;
    }
    
    tuxrt_phsync_t* p = calloc(1, sizeof(tuxrt_phsync_t));
    if (!p) return TUXRT_ERR_NOMEM;
    
    p->cfg = *cfg;
    p->status = (tuxrt_phsync_status_t){
        .offset_ns = 0,
        .drift_ns = 0,
        .last_sync_ns = 0,
        .synchronized = false
    };
    p->last_check_ns = 0;
    p->sync_count = 0;
    p->error_count = 0;
    pthread_mutex_init(&p->mutex, NULL);
    
    *out_phsync = p;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_phsync_start(tuxrt_phsync_t* phsync) {
    if (!phsync) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&phsync->mutex);
    if (phsync->running) {
        pthread_mutex_unlock(&phsync->mutex);
        return TUXRT_OK;
    }
    phsync->running = true;
    pthread_mutex_unlock(&phsync->mutex);
    
    if (pthread_create(&phsync->thread, NULL, phsync_thread, phsync) != 0) {
        phsync->running = false;
        return TUXRT_ERR_IO;
    }
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_phsync_stop(tuxrt_phsync_t* phsync) {
    if (!phsync) return TUXRT_ERR_INVAL;
    
    phsync->running = false;
    if (phsync->thread) pthread_join(phsync->thread, NULL);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_phsync_status(const tuxrt_phsync_t* phsync, tuxrt_phsync_status_t* out) {
    if (!phsync || !out) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock((pthread_mutex_t*)&phsync->mutex); // const cast for stub
    *out = phsync->status;
    pthread_mutex_unlock((pthread_mutex_t*)&phsync->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_phsync_adjust(tuxrt_phsync_t* phsync, int64_t correction_ns) {
    if (!phsync) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&phsync->mutex);
    phsync->status.offset_ns += correction_ns;
    phsync->status.drift_ns += correction_ns;
    pthread_mutex_unlock(&phsync->mutex);
    return TUXRT_OK;
}

void tuxrt_phsync_destroy(tuxrt_phsync_t* phsync) {
    if (!phsync) return;
    tuxrt_phsync_stop(phsync);
    pthread_mutex_destroy(&phsync->mutex);
    free(phsync);
}