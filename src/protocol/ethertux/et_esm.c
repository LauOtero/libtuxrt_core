/*
 * et_esm.c — Máquina de estados del nodo EtherTux (ET-ESM, §6.1).
 *
 *   INIT → CONFIG → SAFE → OP
 *     └──────┴───────┴──────┴──→ FAULT (cuarentena)
 *   FAULT → INIT (reintroducción tras reparación, vía Discovery)
 *
 * El estado del emisor viaja en el campo `State` de cada trama y determina qué
 * tráfico puede generarse: un nodo en FAULT solo responde a Discovery, de modo
 * que un nodo defectuoso no consume ancho de banda ni provoca bloqueos.
 */
#include "et_internal.h"

#include <stdlib.h>

struct tuxrt_et_esm {
    tuxrt_et_state_t state;
};

tuxrt_et_esm_t* tuxrt_et_esm_create(void) {
    tuxrt_et_esm_t* esm = (tuxrt_et_esm_t*)calloc(1u, sizeof(*esm));
    if (esm != NULL) {
        esm->state = TUXRT_ET_STATE_INIT;
    }
    return esm;
}

void tuxrt_et_esm_destroy(tuxrt_et_esm_t* esm) {
    free(esm);
}

void tuxrt_et_esm_reset(tuxrt_et_esm_t* esm) {
    if (esm != NULL) {
        esm->state = TUXRT_ET_STATE_INIT;
    }
}

tuxrt_et_state_t tuxrt_et_esm_state(const tuxrt_et_esm_t* esm) {
    return (esm != NULL) ? esm->state : TUXRT_ET_STATE_FAULT;
}

bool tuxrt_et_esm_can_transition(tuxrt_et_state_t from,
                                     tuxrt_et_state_t to) {
    switch (from) {
        case TUXRT_ET_STATE_INIT:
            return to == TUXRT_ET_STATE_CONFIG || to == TUXRT_ET_STATE_FAULT;
        case TUXRT_ET_STATE_CONFIG:
            return to == TUXRT_ET_STATE_SAFE || to == TUXRT_ET_STATE_FAULT ||
                   to == TUXRT_ET_STATE_INIT;
        case TUXRT_ET_STATE_SAFE:
            return to == TUXRT_ET_STATE_OP || to == TUXRT_ET_STATE_FAULT ||
                   to == TUXRT_ET_STATE_CONFIG;
        case TUXRT_ET_STATE_OP:
            return to == TUXRT_ET_STATE_SAFE || to == TUXRT_ET_STATE_FAULT;
        case TUXRT_ET_STATE_FAULT:
            return to == TUXRT_ET_STATE_INIT;
        default:
            return false;
    }
}

tuxrt_err_t tuxrt_et_esm_request(tuxrt_et_esm_t* esm,
                                            tuxrt_et_state_t next) {
    if (esm == NULL) {
        return TUXRT_ERR_INVAL;
    }
    if (!tuxrt_et_esm_can_transition(esm->state, next)) {
        return TUXRT_ERR_INVALID_STATE;
    }
    esm->state = next;
    return TUXRT_OK;
}

bool tuxrt_et_esm_allows_traffic(tuxrt_et_state_t state,
                                     uint8_t type, uint8_t priority) {
    if (priority >= TUXRT_ET_PRIORITIES) {
        return false;
    }
    const bool mailbox = (type == (uint8_t)TUXRT_ET_TYPE_MAILBOX_DATA ||
                          type == (uint8_t)TUXRT_ET_TYPE_MAILBOX_ACK);
    const bool discovery = (type == (uint8_t)TUXRT_ET_TYPE_DISCOVERY);

    switch (state) {
        case TUXRT_ET_STATE_INIT:
        case TUXRT_ET_STATE_FAULT:
            return discovery;
        case TUXRT_ET_STATE_CONFIG:
            return discovery || mailbox;
        case TUXRT_ET_STATE_SAFE:
            return mailbox || priority == (uint8_t)TUXRT_ET_P0 ||
                   priority == (uint8_t)TUXRT_ET_P1;
        case TUXRT_ET_STATE_OP:
            return true;
        default:
            return false;
    }
}

uint32_t tuxrt_et_esm_build_state_change(tuxrt_et_state_t target,
                                             uint8_t reason,
                                             uint8_t out[2]) {
    if (out == NULL || (uint8_t)target > (uint8_t)TUXRT_ET_STATE_FAULT) {
        return 0u;
    }
    out[0] = (uint8_t)target;
    out[1] = reason;
    return 2u;
}

tuxrt_err_t tuxrt_et_esm_parse_state_change(const uint8_t* in, uint32_t len,
                                                       tuxrt_et_state_t* target,
                                                       uint8_t* reason) {
    if (in == NULL || len < 2u) {
        return TUXRT_ERR_INVAL;
    }
    if (in[0] > (uint8_t)TUXRT_ET_STATE_FAULT) {
        return TUXRT_ERR_INVAL;
    }
    if (target != NULL) {
        *target = (tuxrt_et_state_t)in[0];
    }
    if (reason != NULL) {
        *reason = in[1];
    }
    return TUXRT_OK;
}

bool tuxrt_et_esm_state_from_wire(uint8_t raw, tuxrt_et_state_t* out) {
    if (raw > (uint8_t)TUXRT_ET_STATE_FAULT) {
        return false;
    }
    if (out != NULL) {
        *out = (tuxrt_et_state_t)raw;
    }
    return true;
}
