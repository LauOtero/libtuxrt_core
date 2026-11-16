/*
 * et_topology.c — Registro de nodos, ET-WKC, descubrimiento y Hot Connect
 *                 (§6.2–6.5).
 *
 * El registro es estático (64 entradas, sin asignación dinámica) y guarda por
 * nodo: NodeID, MAC, capacidades, slot TDMA, estado ET-ESM, último ET-WKC,
 * contador de discrepancias, offset/estado de sincronización y temporizador de
 * heartbeat.
 *
 * Verificación pasiva (§6.2): cada nodo que procesa una trama incrementa el
 * ET-WKC del campo de cabecera. El maestro compara el valor recibido con el
 * esperado según el registro; N discrepancias consecutivas (3 por defecto)
 * disparan un `TopoCheck` unicast (§6.3), sin broadcast.
 *
 * Hot Connect (§6.4): cada 100 superframes se sondea una dirección libre con
 * `Discovery` unicast; nunca se usa broadcast en operación.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

struct tuxrt_et_topology {
    tuxrt_et_topology_config_t cfg;
    tuxrt_et_node_info_t       nodes[TUXRT_ET_MAX_NODES];
    uint32_t                       count;
    /* Caché directa {hash(MAC) -> índice} validada en cada acierto: el camino
     * por trama pasa de O(N) `memcmp` a un solo acceso. Al validar la MAC no
     * necesita invalidación tras `remove`/`upsert` (reordenamientos incluidos). */
    uint16_t                       mac_cache[TUXRT_ET_MAX_NODES * 4u];
};

/* Hash de 6 B barato (multiplicación-xor): no hace falta CRC aquí. */
static inline uint32_t topo_mac_hash(const uint8_t mac[6]) {
    uint32_t h = 0x9E3779B9u;
    h ^= (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    h *= 0x85EBCA6Bu;
    h ^= (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
    h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

static inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void tuxrt_et_topology_default_config(tuxrt_et_topology_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->wkc_mismatch_threshold = 3u;          /* 3 superframes  */
    cfg->hot_connect_period_frames = 100u;     /* 100 ms         */
    cfg->heartbeat_timeout_frames = 3u;        /* 3 superframes  */
}

tuxrt_et_topology_t* tuxrt_et_topology_create(void) {
    tuxrt_et_topology_t* t = (tuxrt_et_topology_t*)calloc(1u, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    tuxrt_et_topology_default_config(&t->cfg);
    for (uint32_t i = 0; i < TUXRT_ET_MAX_NODES; i++) {
        t->nodes[i].slot = TUXRT_ET_SLOT_FREE;
    }
    return t;
}

void tuxrt_et_topology_destroy(tuxrt_et_topology_t* topo) {
    free(topo);
}

tuxrt_err_t tuxrt_et_topology_configure(tuxrt_et_topology_t* topo,
                                                   const tuxrt_et_topology_config_t* cfg) {
    if (topo == NULL || cfg == NULL) {
        return TUXRT_ERR_INVAL;
    }
    if (cfg->wkc_mismatch_threshold == 0u || cfg->heartbeat_timeout_frames == 0u) {
        return TUXRT_ERR_INVAL;
    }
    topo->cfg = *cfg;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_et_topology_upsert(tuxrt_et_topology_t* topo,
                                                const tuxrt_et_node_info_t* info) {
    if (topo == NULL || info == NULL || info->id == 0u) {
        return TUXRT_ERR_INVAL;
    }

    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == info->id) {
            uint16_t slot = topo->nodes[i].slot;   /* conserva el slot asignado */
            topo->nodes[i] = *info;
            if (info->slot >= (uint16_t)TUXRT_ET_MAX_NODES) {
                topo->nodes[i].slot = slot;
            }
            return TUXRT_OK;
        }
    }

    if (topo->count >= TUXRT_ET_MAX_NODES) {
        return TUXRT_ERR_OVERFLOW;
    }
    tuxrt_et_node_info_t* n = &topo->nodes[topo->count++];
    *n = *info;
    if (n->slot >= (uint16_t)TUXRT_ET_MAX_NODES) {
        n->slot = TUXRT_ET_SLOT_FREE;
    }
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_et_topology_remove(tuxrt_et_topology_t* topo,
                                                uint16_t node_id) {
    if (topo == NULL || node_id == 0u) {
        return TUXRT_ERR_INVAL;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == node_id) {
            for (uint32_t j = i + 1u; j < topo->count; j++) {
                topo->nodes[j - 1u] = topo->nodes[j];
            }
            topo->count--;
            return TUXRT_OK;
        }
    }
    return TUXRT_ERR_NOENT;
}

const tuxrt_et_node_info_t* tuxrt_et_topology_find(const tuxrt_et_topology_t* topo,
                                                           uint16_t node_id) {
    if (topo == NULL || node_id == 0u) {
        return NULL;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == node_id) {
            return &topo->nodes[i];
        }
    }
    return NULL;
}

uint32_t tuxrt_et_topology_count(const tuxrt_et_topology_t* topo) {
    return (topo != NULL) ? topo->count : 0u;
}

const tuxrt_et_node_info_t* tuxrt_et_topology_at(const tuxrt_et_topology_t* topo,
                                                         uint32_t index) {
    if (topo == NULL || index >= topo->count) {
        return NULL;
    }
    return &topo->nodes[index];
}

uint16_t tuxrt_et_topology_next_free_slot(const tuxrt_et_topology_t* topo) {
    if (topo == NULL) {
        return TUXRT_ET_SLOT_FREE;
    }
    for (uint16_t slot = 0u; slot < (uint16_t)TUXRT_ET_MAX_NODES; slot++) {
        bool used = false;
        for (uint32_t i = 0; i < topo->count; i++) {
            if (topo->nodes[i].slot == slot) {
                used = true;
                break;
            }
        }
        if (!used) {
            return slot;
        }
    }
    return TUXRT_ET_SLOT_FREE;
}

tuxrt_err_t tuxrt_et_topology_assign_slot(tuxrt_et_topology_t* topo,
                                                     uint16_t node_id,
                                                     uint16_t slot) {
    if (topo == NULL || node_id == 0u) {
        return TUXRT_ERR_INVAL;
    }
    if (slot != TUXRT_ET_SLOT_FREE && slot >= (uint16_t)TUXRT_ET_MAX_NODES) {
        return TUXRT_ERR_INVAL;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == node_id) {
            topo->nodes[i].slot = slot;
            return TUXRT_OK;
        }
    }
    return TUXRT_ERR_NOENT;
}

tuxrt_err_t tuxrt_et_topology_on_frame(tuxrt_et_topology_t* topo,
                                                  const uint8_t mac[6],
                                                  const tuxrt_et_header_t* h,
                                                  uint64_t now_ns) {
    if (topo == NULL || mac == NULL || h == NULL) {
        return TUXRT_ERR_INVAL;
    }

    /* Camino rápido: caché validada por MAC (1 acceso, sin barrido). */
    const uint32_t ch = topo_mac_hash(mac) & (uint32_t)(sizeof(topo->mac_cache) / sizeof(topo->mac_cache[0]) - 1u);
    const uint32_t ci = topo->mac_cache[ch];
    if (ci < topo->count && memcmp(topo->nodes[ci].mac, mac, 6u) == 0) {
        tuxrt_et_node_info_t* n = &topo->nodes[ci];
        n->last_seen_ns = now_ns;
        tuxrt_et_state_t st;
        if (tuxrt_et_esm_state_from_wire(h->state, &st)) {
            n->state = st;
        }
        n->last_wkc = h->wkc;
        if (n->expected_wkc != 0u && h->wkc != n->expected_wkc) {
            if (n->wkc_mismatch_count < UINT8_MAX) {
                n->wkc_mismatch_count++;
            }
            return TUXRT_ERR_PROTO;
        }
        n->wkc_mismatch_count = 0u;
        return TUXRT_OK;
    }

    for (uint32_t i = 0; i < topo->count; i++) {
        tuxrt_et_node_info_t* n = &topo->nodes[i];
        if (memcmp(n->mac, mac, 6u) != 0) {
            continue;
        }
        topo->mac_cache[ch] = (uint16_t)i;   /* refresca la caché */

        n->last_seen_ns = now_ns;
        tuxrt_et_state_t st;
        if (tuxrt_et_esm_state_from_wire(h->state, &st)) {
            n->state = st;
        }
        n->last_wkc = h->wkc;

        /* ET-WKC esperado: 0 desactiva la verificación de ese nodo. */
        if (n->expected_wkc != 0u && h->wkc != n->expected_wkc) {
            if (n->wkc_mismatch_count < UINT8_MAX) {
                n->wkc_mismatch_count++;
            }
            return TUXRT_ERR_PROTO;
        }
        n->wkc_mismatch_count = 0u;
        return TUXRT_OK;
    }
    return TUXRT_ERR_NOENT;
}

bool tuxrt_et_topology_needs_topo_check(const tuxrt_et_topology_t* topo,
                                            uint16_t* out_node_id) {
    if (topo == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        const tuxrt_et_node_info_t* n = &topo->nodes[i];
        if (n->wkc_mismatch_count >= topo->cfg.wkc_mismatch_threshold) {
            if (out_node_id != NULL) {
                *out_node_id = n->id;
            }
            return true;
        }
    }
    return false;
}

uint32_t tuxrt_et_topology_tick(tuxrt_et_topology_t* topo,
                                    uint64_t now_ns, uint32_t cycle_ns) {
    if (topo == NULL || cycle_ns == 0u) {
        return 0u;
    }
    uint64_t limit = (uint64_t)cycle_ns * (uint64_t)topo->cfg.heartbeat_timeout_frames;
    uint32_t faulted = 0u;

    for (uint32_t i = 0; i < topo->count; i++) {
        tuxrt_et_node_info_t* n = &topo->nodes[i];
        if (n->state == TUXRT_ET_STATE_FAULT || n->last_seen_ns == 0u) {
            continue;
        }
        if (now_ns > n->last_seen_ns && (now_ns - n->last_seen_ns) > limit) {
            n->state = TUXRT_ET_STATE_FAULT;
            n->slot = TUXRT_ET_SLOT_FREE;   /* se libera para reasignación */
            faulted++;
        }
    }
    return faulted;
}

bool tuxrt_et_topology_hot_connect_due(const tuxrt_et_topology_t* topo,
                                            uint64_t frame_index,
                                            uint16_t* out_node_addr) {
    if (topo == NULL || topo->cfg.hot_connect_period_frames == 0u) {
        return false;
    }
    if (frame_index % (uint64_t)topo->cfg.hot_connect_period_frames != 0u) {
        return false;
    }
    /* Dirección libre siguiente (NodeID consecutivo al último registrado). */
    if (out_node_addr != NULL) {
        *out_node_addr = (uint16_t)(topo->count + 1u);
    }
    return true;
}

uint32_t tuxrt_et_topology_build_reply(const tuxrt_et_node_info_t* info,
                                            uint8_t out[TUXRT_ET_DISCOVERY_REPLY_SIZE]) {
    if (info == NULL || out == NULL || info->id == 0u) {
        return 0u;
    }
    put_le16(out + 0u, info->id);
    out[2] = (uint8_t)info->state;
    out[3] = info->fec_supported ? 0x01u : 0x00u;
    put_le16(out + 4u, info->link_mbps);
    put_le16(out + 6u, info->buffer_bytes);
    return TUXRT_ET_DISCOVERY_REPLY_SIZE;
}

tuxrt_err_t tuxrt_et_topology_parse_reply(const uint8_t* in, uint32_t len,
                                                      tuxrt_et_node_info_t* out) {
    if (in == NULL || out == NULL || len < TUXRT_ET_DISCOVERY_REPLY_SIZE) {
        return TUXRT_ERR_INVAL;
    }
    uint16_t id = get_le16(in + 0u);
    if (id == 0u) {
        return TUXRT_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->state = (in[2] <= (uint8_t)TUXRT_ET_STATE_FAULT)
                     ? (tuxrt_et_state_t)in[2]
                     : TUXRT_ET_STATE_INIT;
    out->fec_supported = (in[3] & 0x01u) != 0u;
    out->link_mbps = get_le16(in + 4u);
    out->buffer_bytes = get_le16(in + 6u);
    out->slot = TUXRT_ET_SLOT_FREE;
    return TUXRT_OK;
}
