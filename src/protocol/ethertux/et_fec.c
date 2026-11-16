/*
 * et_fec.c — Gestión de bloques FEC Reed-Solomon de EtherTux.
 *
 * Envuelve la HAL de Reed-Solomon (`tuxrt_hwaccel.h`) y resuelve los modos
 * predefinidos del protocolo:
 *
 *   LIGHT  -> RS(10,8): 8 datos + 2 paridad  (25 % de overhead)
 *   ROBUST -> RS(16,8): 8 datos + 8 paridad  (100 % de overhead)
 *
 * La decodificación es de BORRADOS: el receptor conoce qué bloques faltan (por
 * BlockID/SeqNum) y los reconstruye sin retransmisión ni RTT. Es la propiedad
 * que sostiene el requisito de "cero pérdida sin dependencia del RTT".
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

tuxrt_et_fec_t* tuxrt_et_fec_create(uint8_t k, uint8_t n) {
    if (k == 0u || n <= k || n > TUXRT_RS_MAX_N) {
        return NULL;
    }
    tuxrt_et_fec_t* fec = (tuxrt_et_fec_t*)calloc(1u, sizeof(*fec));
    if (fec == NULL) {
        return NULL;
    }
    fec->k = k;
    fec->n = n;
    return fec;
}

tuxrt_et_fec_t* tuxrt_et_fec_create_mode(tuxrt_et_fec_mode_t mode) {
    switch (mode) {
        case TUXRT_ET_FEC_LIGHT:  return tuxrt_et_fec_create(8u, 10u);
        case TUXRT_ET_FEC_ROBUST: return tuxrt_et_fec_create(8u, 16u);
        case TUXRT_ET_FEC_OFF:
        default:                      return NULL;
    }
}

void tuxrt_et_fec_destroy(tuxrt_et_fec_t* fec) {
    free(fec);
}

uint8_t tuxrt_et_fec_k(const tuxrt_et_fec_t* fec) {
    return (fec != NULL) ? fec->k : 0u;
}

uint8_t tuxrt_et_fec_n(const tuxrt_et_fec_t* fec) {
    return (fec != NULL) ? fec->n : 0u;
}

uint32_t tuxrt_et_fec_overhead_percent(const tuxrt_et_fec_t* fec) {
    if (fec == NULL || fec->k == 0u) {
        return 0u;
    }
    return ((uint32_t)(fec->n - fec->k) * 100u) / (uint32_t)fec->k;
}

tuxrt_err_t tuxrt_et_fec_encode(const tuxrt_et_fec_t* fec,
                                           const uint8_t* const* data,
                                           uint8_t* const* parity,
                                           uint32_t len) {
    if (fec == NULL) {
        return TUXRT_ERR_INVAL;
    }
    return tuxrt_rs_encode(fec->k, fec->n, data, parity, len);
}

tuxrt_err_t tuxrt_et_fec_decode(const tuxrt_et_fec_t* fec,
                                           uint8_t* const* blocks,
                                           const bool* present,
                                           uint32_t len) {
    if (fec == NULL) {
        return TUXRT_ERR_INVAL;
    }
    return tuxrt_rs_decode(fec->k, fec->n, blocks, present, len);
}
