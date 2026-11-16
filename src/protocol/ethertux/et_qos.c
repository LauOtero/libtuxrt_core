/*
 * et_qos.c — QoS jerárquico de EtherTux: token buckets, backpressure y LAPM.
 *
 * Modelo de servicio (P0 la clase más crítica):
 *
 *   P0  express        nunca se descarta; si no hay tokens => backpressure
 *   P1  express        degrada a P2 si su bucket se agota
 *   P2  time-triggered degrada a P4
 *   P3  rate-limited   degrada a P4
 *   P4  rate-limited   degrada a P5
 *   P5  best-effort    se descarta (drop-tail) si no hay tokens
 *
 * La suma de las tasas garantizadas por defecto (5+5+50+30+20+15 MB/s) es
 * exactamente 125 MB/s = 1 Gbps: el control de admisión garantiza por diseño
 * que la suma de lo reservado nunca excede la capacidad del enlace (principio
 * DetNet de "cero congestión en el puerto de salida").
 *
 * LAPM: cuando la ocupación de una clase supera `lapm_high_percent` (80 %), sus
 * flujos migran a la clase inferior; no vuelven hasta que la ocupación baja de
 * `lapm_low_percent` (60 %). La histéresis evita oscilaciones.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

/* Cadena de degradación ante agotamiento de tokens (-1 = no degradable). */
static const int8_t s_degrade[TUXRT_ET_PRIORITIES] = {
    -1,                                  /* P0: backpressure */
    TUXRT_ET_P2,                     /* P1 -> P2         */
    TUXRT_ET_P4,                     /* P2 -> P4         */
    TUXRT_ET_P4,                     /* P3 -> P4         */
    TUXRT_ET_P5,                     /* P4 -> P5         */
    -1                                   /* P5: descarte     */
};

static int64_t clamp_tokens(int64_t tokens, uint32_t burst) {
    if (tokens > (int64_t)burst) {
        return (int64_t)burst;
    }
    return tokens;
}

static void bucket_refill(et_bucket_t* b, const tuxrt_et_bucket_cfg_t* cfg,
                          uint64_t rate_q32, uint64_t now_ns) {
    if (cfg->rate_bytes_per_sec == 0u || cfg->burst_bytes == 0u) {
        b->tokens = 0;
        b->last_ns = now_ns;
        return;
    }
    if (b->last_ns == 0u) {
        b->last_ns = now_ns;
        b->tokens = (int64_t)cfg->burst_bytes;
        return;
    }
    if (now_ns <= b->last_ns) {
        return;
    }
    uint64_t dt = now_ns - b->last_ns;
    /* Acota el intervalo para que rate*dt no desborde. */
    if (dt > 1000000000ull) {
        dt = 1000000000ull;
    }
    b->last_ns = now_ns;
    /* bytes = (dt_ns · rate_q32) >> 32, sin división de 64 bits. */
    int64_t add = (int64_t)(((__uint128_t)dt * rate_q32) >> 32);
    b->tokens = clamp_tokens(b->tokens + add, cfg->burst_bytes);
}

static void qos_refill_all(tuxrt_et_qos_t* q, uint64_t now_ns) {
    for (unsigned i = 0; i < TUXRT_ET_PRIORITIES; i++) {
        bucket_refill(&q->tb[i], &q->cfg.bucket[i], q->rate_q32[i], now_ns);
    }
}

/* Ocupación del bucket en porcentaje (100 % = vacío), sin división. */
static uint32_t bucket_occupancy(const tuxrt_et_qos_t* q, unsigned cls) {
    uint32_t burst = q->cfg.bucket[cls].burst_bytes;
    if (burst == 0u) {
        return 100u;
    }
    if (q->tb[cls].tokens <= 0) {
        return 100u;
    }
    int64_t used = (int64_t)burst - q->tb[cls].tokens;
    if (used <= 0) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)used * q->inv_burst_q32[cls]) >> 32);
}

static void qos_update_lapm(tuxrt_et_qos_t* q) {
    if (!q->cfg.lapm_enabled) {
        for (unsigned i = 0; i < TUXRT_ET_PRIORITIES; i++) {
            q->eff[i] = (uint8_t)i;
        }
        q->lapm_active = false;
        return;
    }

    bool active = false;
    for (unsigned cls = TUXRT_ET_P3; cls <= TUXRT_ET_P4; cls++) {
        uint32_t occ = bucket_occupancy(q, cls);
        if (q->eff[cls] == (uint8_t)cls) {
            if (occ >= q->cfg.lapm_high_percent) {
                q->eff[cls] = (uint8_t)((cls == TUXRT_ET_P3) ? TUXRT_ET_P4
                                                                 : TUXRT_ET_P5);
                q->stats.degraded++;
            }
        } else if (occ <= q->cfg.lapm_low_percent) {
            q->eff[cls] = (uint8_t)cls;
        }
        if (q->eff[cls] != (uint8_t)cls) {
            active = true;
        }
    }
    q->lapm_active = active;
}

/* ------------------------------------------------------------------ */
/* API pública                                                         */
/* ------------------------------------------------------------------ */

void tuxrt_et_qos_default_config(tuxrt_et_qos_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* Reparto para un enlace de 1 Gbps (125 MB/s) sin sobre-suscripción. */
    cfg->bucket[TUXRT_ET_P0].rate_bytes_per_sec =   5u * 1000u * 1000u;
    cfg->bucket[TUXRT_ET_P0].burst_bytes        =   4096u;
    cfg->bucket[TUXRT_ET_P1].rate_bytes_per_sec =   5u * 1000u * 1000u;
    cfg->bucket[TUXRT_ET_P1].burst_bytes        =   8192u;
    cfg->bucket[TUXRT_ET_P2].rate_bytes_per_sec =  50u * 1000u * 1000u;
    cfg->bucket[TUXRT_ET_P2].burst_bytes        =  65536u;
    cfg->bucket[TUXRT_ET_P3].rate_bytes_per_sec =  30u * 1000u * 1000u;
    cfg->bucket[TUXRT_ET_P3].burst_bytes        = 131072u;
    cfg->bucket[TUXRT_ET_P4].rate_bytes_per_sec =  20u * 1000u * 1000u;
    cfg->bucket[TUXRT_ET_P4].burst_bytes        = 131072u;
    cfg->bucket[TUXRT_ET_P5].rate_bytes_per_sec =  15u * 1000u * 1000u;
    cfg->bucket[TUXRT_ET_P5].burst_bytes        = 262144u;

    cfg->pause_percent     = 80u;
    cfg->lapm_enabled      = true;
    cfg->lapm_high_percent = 80u;
    cfg->lapm_low_percent  = 60u;
}

tuxrt_et_qos_t* tuxrt_et_qos_create(void) {
    tuxrt_et_qos_t* q = (tuxrt_et_qos_t*)calloc(1u, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    tuxrt_et_qos_config_t cfg;
    tuxrt_et_qos_default_config(&cfg);
    if (tuxrt_et_qos_configure(q, &cfg) != TUXRT_OK) {
        free(q);
        return NULL;
    }
    return q;
}

void tuxrt_et_qos_destroy(tuxrt_et_qos_t* qos) {
    free(qos);
}

tuxrt_err_t tuxrt_et_qos_configure(tuxrt_et_qos_t* qos,
                                              const tuxrt_et_qos_config_t* cfg) {
    if (qos == NULL || cfg == NULL) {
        return TUXRT_ERR_INVAL;
    }
    if (cfg->pause_percent < 1u || cfg->pause_percent > 100u) {
        return TUXRT_ERR_INVAL;
    }
    if (cfg->lapm_high_percent <= cfg->lapm_low_percent ||
        cfg->lapm_high_percent > 100u) {
        return TUXRT_ERR_INVAL;
    }
    for (unsigned i = 0; i < TUXRT_ET_PRIORITIES; i++) {
        if (cfg->bucket[i].burst_bytes == 0u || cfg->bucket[i].rate_bytes_per_sec == 0u) {
            return TUXRT_ERR_INVAL;
        }
    }

    qos->cfg = *cfg;
    for (unsigned i = 0; i < TUXRT_ET_PRIORITIES; i++) {
        qos->tb[i].tokens = (int64_t)cfg->bucket[i].burst_bytes;
        qos->tb[i].last_ns = 0u;
        qos->eff[i] = (uint8_t)i;
        /* Q32: bytes por ns y 100/burst, ambos con recíproco fijo. */
        qos->rate_q32[i] = (uint64_t)(((__uint128_t)cfg->bucket[i].rate_bytes_per_sec << 32)
                                      / 1000000000ull);
        qos->inv_burst_q32[i] = (uint64_t)(((__uint128_t)100u << 32)
                                           / cfg->bucket[i].burst_bytes);
    }
    qos->lapm_active = false;
    qos->last_refill_ns = 0u;
    qos->last_lazy_ns = 0u;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_et_qos_admit(tuxrt_et_qos_t* qos,
                                          uint8_t priority, uint32_t bytes,
                                          uint64_t now_ns) {
    if (qos == NULL || priority >= TUXRT_ET_PRIORITIES) {
        return TUXRT_ERR_INVAL;
    }

    uint64_t now = (now_ns != 0u) ? now_ns : tuxrt_now_ns();

    /* Lazy refill: solo refill completo si han pasado > 1 ms desde el último,
     * si no refillamos solo el bucket de la clase efectiva. */
    if (now - qos->last_refill_ns > 1000000ull) {
        qos_refill_all(qos, now);
    }
    /* LAPM: dos lecturas y una multiplicación-shift por clase (sin división). */
    qos_update_lapm(qos);

    int cls = (int)qos->eff[priority];
    for (;;) {
        if (now - qos->last_lazy_ns > 500000ull) {
            bucket_refill(&qos->tb[cls], &qos->cfg.bucket[cls], qos->rate_q32[cls], now);
            qos->last_lazy_ns = now;
        }
        if (qos->tb[cls].tokens >= (int64_t)bytes) {
            qos->tb[cls].tokens -= (int64_t)bytes;
            qos->stats.admitted++;
            qos->stats.bytes_admitted += bytes;
            return TUXRT_OK;
        }
        int next = (int)s_degrade[cls];
        if (next < 0 || next == cls) {
            break;
        }
        cls = next;
        qos->stats.degraded++;
    }

    if (priority == TUXRT_ET_P5) {
        qos->stats.dropped++;
        return TUXRT_ERR_DROPPED;
    }
    qos->stats.backpressure++;
    return TUXRT_ERR_BACKPRESSURE;
}

bool tuxrt_et_qos_backpressure(const tuxrt_et_qos_t* qos, uint8_t priority) {
    if (qos == NULL || priority >= TUXRT_ET_PRIORITIES) {
        return true;
    }
    /* P0 y P1 nunca se pausan por sí mismos: son la clase crítica. */
    if (priority <= TUXRT_ET_P1) {
        return false;
    }
    if (priority >= TUXRT_ET_P2 &&
        bucket_occupancy(qos, TUXRT_ET_P0) >= qos->cfg.pause_percent) {
        return true;
    }
    if (priority >= TUXRT_ET_P3 &&
        bucket_occupancy(qos, TUXRT_ET_P1) >= ET_PAUSE_P1_AT) {
        return true;
    }
    if (priority >= TUXRT_ET_P4 &&
        bucket_occupancy(qos, TUXRT_ET_P2) >= ET_PAUSE_P2_AT) {
        return true;
    }
    return false;
}

uint8_t tuxrt_et_qos_effective_priority(const tuxrt_et_qos_t* qos,
                                            uint8_t priority) {
    if (qos == NULL || priority >= TUXRT_ET_PRIORITIES) {
        return priority;
    }
    return qos->eff[priority];
}

tuxrt_err_t tuxrt_et_qos_stats(const tuxrt_et_qos_t* qos,
                                          tuxrt_et_qos_stats_t* out) {
    if (qos == NULL || out == NULL) {
        return TUXRT_ERR_INVAL;
    }
    *out = qos->stats;
    return TUXRT_OK;
}

void tuxrt_et_qos_reset_stats(tuxrt_et_qos_t* qos) {
    if (qos != NULL) {
        memset(&qos->stats, 0, sizeof(qos->stats));
    }
}
