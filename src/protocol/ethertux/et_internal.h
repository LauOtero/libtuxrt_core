#ifndef TUXRT_ET_INTERNAL_H
#define TUXRT_ET_INTERNAL_H

/*
 * et_internal.h — Definiciones internas del protocolo EtherTux.
 *
 * NO es una cabecera pública: solo la usan los módulos de `src/protocol/ethertux/`.
 *
 * Organización de módulos (v0.1.0):
 *   et_frame.c    — serialización/parseo de la cabecera v0.1.0 (26 B) y la PDU (CRC-32C).
 *   et_fec.c      — gestión de bloques FEC sobre la HAL de Reed-Solomon.
 *   et_qos.c      — token buckets jerárquicos, backpressure y LAPM.
 *   et_sched.c    — planificador time-triggered con ventanas dedicadas.
 *   et_esm.c      — máquina de estados ET-ESM del nodo.
 *   et_sync.c     — ET-Sync (offset/drift) y Monitor de Ventana.
 *   et_topology.c — registro de nodos, ET-WKC, descubrimiento y Hot Connect.
 *   et_mbx.c      — mailbox ET-MBX (segmentación, ACK/NACK, reintentos).
 *   et_flowagg.c  — Flow Aggregator (varios flujos por trama).
 *   et_frm.c      — redundancia lógica de flujo (FRM) y deduplicación.
 *   et_socket.c   — socket AF_PACKET (0x88B5), pacing SO_TXTIME y recepción.
 *   et_master.c   — ciclo de vida, envío/recepción, nodos y heartbeat.
 */

/* Si el bridge necesita status_t, SUCCESS, ERROR_* definidos aquí, */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <net/if.h>
#include <tuxrt_ethertux.h>

/* ------------------------------------------------------------------ */
/* Número máximo de ranuras del superframe.                            */
/* ------------------------------------------------------------------ */
#define ET_MAX_SLOTS 64u

/* ------------------------------------------------------------------ */
/* Índice hash de nodos del maestro (sondeo lineal acotado).           */
/* ------------------------------------------------------------------ */
#define ET_NODE_HASH_SLOTS  256u
#define ET_NODE_HASH_PROBES 8u

/* ------------------------------------------------------------------ */
/* Ventanas del superframe (índices).                                   */
/* ------------------------------------------------------------------ */
#define ET_WINDOW_CONTROL       0u
#define ET_WINDOW_DETERMINISTIC 1u
#define ET_WINDOW_RATE          2u
#define ET_WINDOW_BEST_EFFORT   3u
#define ET_WINDOW_COUNT         4u

/* ------------------------------------------------------------------ */
/* Umbrales fijos de backpressure consciente de prioridad (§5.3).         */
/* ------------------------------------------------------------------ */
#define ET_PAUSE_P1_AT 90u
#define ET_PAUSE_P2_AT 95u

/* ------------------------------------------------------------------ */
/* Puerto de red                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int      fd;
    uint8_t  mac[6];
    int      ifindex;
    char     ifname[IF_NAMESIZE];
    bool     txtime_enabled;      /* SO_TXTIME aceptado por la NIC       */
    uint64_t frames_tx;
    uint64_t frames_rx;
    uint64_t bytes_tx;
    uint64_t bytes_rx;

    /* Anillo de transmisión PACKET_TX_RING (0 si no está activo). */
    uint8_t* tx_ring;             /* mmap del anillo                     */
    size_t   tx_ring_bytes;
    uint32_t tx_frame_size;       /* tp_frame_size                       */
    uint32_t tx_frame_nr;         /* tramas en el anillo (potencia de 2) */
    uint32_t tx_frame_mask;       /* tx_frame_nr - 1: sin módulo          */
    uint32_t tx_head;             /* siguiente ranura candidata          */
    bool     tx_ring_active;
    bool     tx_reserved;         /* ranura reservada pendiente de commit */
} et_port_t;

/* ------------------------------------------------------------------ */
/* QoS                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t  tokens;      /* bytes disponibles                               */
    uint64_t last_ns;     /* instante del último reabastecimiento            */
} et_bucket_t;

struct tuxrt_et_qos {
    tuxrt_et_qos_config_t cfg;
    et_bucket_t               tb[TUXRT_ET_PRIORITIES];
    uint8_t                   eff[TUXRT_ET_PRIORITIES];  /* LAPM         */
    bool                      lapm_active;
    uint64_t                  last_refill_ns;    /* lazy refill: último refill completo */
    uint64_t                  last_lazy_ns;      /* ultimo refill lazy individual */
    /* Coeficientes fijos (Q32) para no dividir en el camino caliente. */
    uint64_t                  rate_q32[TUXRT_ET_PRIORITIES];      /* B/ns × 2^32 */
    uint64_t                  inv_burst_q32[TUXRT_ET_PRIORITIES]; /* 100/burst × 2^32 */
    tuxrt_et_qos_stats_t  stats;
};

/* ------------------------------------------------------------------ */
/* Planificador                                                        */
/* ------------------------------------------------------------------ */
struct tuxrt_et_sched {
    uint32_t           cycle_ns;
    uint64_t           link_bps;
    uint64_t           cycle_recip;   /* magia para `now % cycle_ns` sin idiv */
    uint32_t           window_ns[ET_WINDOW_COUNT];
    tuxrt_et_slot_t slots[ET_MAX_SLOTS];
    uint32_t           slot_count;
};

/* ------------------------------------------------------------------ */
/* FEC                                                                 */
/* ------------------------------------------------------------------ */
struct tuxrt_et_fec {
    uint8_t k;
    uint8_t n;
};

/* ------------------------------------------------------------------ */
/* Maestro                                                             */
/* ------------------------------------------------------------------ */
struct tuxrt_et_master {
    et_port_t port;
    bool      is_open;

    uint64_t  link_bps;
    uint32_t  cycle_ns;

    tuxrt_et_qos_t*   qos;    /* propio                              */
    tuxrt_et_fec_t*   fec;    /* propio; NULL = sin FEC              */
    tuxrt_et_sched_t* sched;  /* NO propio                           */

    /* Módulos v0.1.0 asociados (NO propios; NULL = no integrado). */
    tuxrt_et_topology_t* topo;
    tuxrt_et_sync_t*     sync;
    tuxrt_et_esm_t*      esm;
    tuxrt_et_mbx_t*      mbx;
    tuxrt_et_frm_t*      frm;

    tuxrt_et_node_t nodes[TUXRT_ET_MAX_NODES];
    uint32_t            node_count;
    uint8_t             node_hash[ET_NODE_HASH_SLOTS];  /* índice+1, 0 = vacío */

    /* ID local determinista (Nivel 2): NULL = esquema desactivado. */
    tuxrt_et_aalid_t* aalid;
    uint16_t          local_id;      /* TUXRT_ET_LOCAL_ID_NONE si no hay */

    uint32_t seq[TUXRT_ET_MAX_FLOWS + 1u];
    uint16_t block_seq;
    uint16_t last_wkc;

    uint8_t  tx_frame[TUXRT_ET_MAX_FRAME];
    uint8_t  rx_frame[TUXRT_ET_MAX_FRAME];
    uint8_t  aux[TUXRT_ET_MAX_PAYLOAD];   /* cargas agregadas / mailbox */
    uint8_t  fec_parity[TUXRT_ET_MAX_PAYLOAD * (TUXRT_RS_MAX_N - 1)]; /* buffer FEC pre-asignado */

    tuxrt_et_master_stats_t stats;
};

/* ------------------------------------------------------------------ */
/* et_socket.c                                                         */
/* ------------------------------------------------------------------ */
tuxrt_err_t et_socket_open(et_port_t* port, const char* ifname);
void     et_socket_close(et_port_t* port);
int      et_socket_send(et_port_t* port, const uint8_t* frame, uint32_t len);
int      et_socket_send_iov(et_port_t* port, const void* hdr, uint32_t hdr_len,
                            const void* payload, uint32_t payload_len);
int      et_socket_send_batch(et_port_t* port, struct mmsghdr* msgs, uint32_t count);
int      et_socket_send_at(et_port_t* port, const uint8_t* frame, uint32_t len,
                           uint64_t deadline_ns);
int      et_socket_recv(et_port_t* port, uint8_t* frame, uint32_t bufsize,
                        uint32_t timeout_us);
bool     et_socket_txtime_enabled(const et_port_t* port);
/** Velocidad del enlace en bps desde sysfs; 0 si no se puede leer. */
uint64_t et_socket_link_bps(const char* ifname);

/* Anillo de transmisión PACKET_TX_RING */
bool     et_socket_tx_ring_active(const et_port_t* port);
bool     et_socket_tx_ring_enable(et_port_t* port, uint32_t frames);
void     et_socket_tx_ring_disable(et_port_t* port);
int      et_socket_tx_ring_kick(et_port_t* port);
int      et_socket_tx_ring_put(et_port_t* port, const uint8_t* frame, uint32_t len);
int      et_socket_tx_ring_space(const et_port_t* port);

/* ------------------------------------------------------------------ */
/* et_master.c                                                         */
/* ------------------------------------------------------------------ */

/** Registra/refresca el nodo identificado por su MAC. Devuelve su índice. */
int et_master_touch_node(tuxrt_et_master_t* master, const uint8_t mac[6],
                         uint64_t now_ns);

/* ------------------------------------------------------------------ */
/* ET-ESM slave transition (used by bridge)                            */
/* ------------------------------------------------------------------ */

/** Convierte un status_t interno en un error tuxrt_err_t. */
typedef uint32_t status_t;

/* Éxito y códigos de error internos */
#define SUCCESS           ((status_t)0)
#define ERROR_INVALID     ((status_t)1)
#define ERROR_STATE       ((status_t)2)
#define ERROR_IO          ((status_t)3)
#define ERROR_TIMEOUT     ((status_t)4)

/** Convierte un status_t interno en un error tuxrt_err_t. */
status_t tuxrt_err_from_status(status_t st);

/** Estado ET-ESM del nodo (compatibilidad con et_esm_slave_transition). */
typedef enum {
    ET_STATE_INIT   = 0,   /* solo Discovery                     */
    ET_STATE_CONFIG = 1,   /* Discovery + mailbox                */
    ET_STATE_SAFE   = 2,   /* P0, P1 y mailbox                   */
    ET_STATE_OP     = 3,   /* P0–P5                              */
    ET_STATE_FAULT  = 4    /* cuarentena: solo Discovery         */
} et_state_t;

/* Estructura opaca del esclavo (forward declaration para el bridge). */
typedef struct {
    uint8_t esm_state;
} et_slave_t;

/** Obtener el estado ESM actual del esclavo (acceso seguro/opaco). */
static inline uint8_t et_slave_esm_state(const et_slave_t* slave) {
    return slave->esm_state;
}

/** Estadísticas del modo esclavo. */
typedef struct {
    uint64_t frames_tx;      /* tramas transmitidas exitosas   */
    uint64_t frames_rx;      /* tramas recibidas (después de filtro) */
    uint64_t bytes_tx;       /* bytes transmitidos             */
    uint64_t bytes_rx;       /* bytes recibidos (después de filtro) */
    uint64_t dropped;        /* tramas descartadas (filtro destino) */
    uint64_t frame_errors;   /* errores de formato de trama    */
} et_slave_stats_t;

/* Declaraciones de funciones et_slave_* (implementadas en et_slave.c). */
et_slave_t* et_slave_create(const char* ifname,
                            uint64_t link_bps,
                            uint32_t cycle_ns);

void et_slave_destroy(et_slave_t* slave);

status_t et_slave_open(et_slave_t* slave, const char* ifname);

void et_slave_close(et_slave_t* slave);

uint8_t et_slave_mode(const et_slave_t* slave);

status_t et_slave_stats(const et_slave_t* slave, et_slave_stats_t* out);

status_t et_slave_send(et_slave_t* slave,
                       uint8_t priority,
                       tuxrt_et_type_t type,
                       uint8_t flow_id,
                       const void* payload,
                       uint16_t len);

status_t et_slave_send_at(et_slave_t* slave,
                          uint8_t priority,
                          tuxrt_et_type_t type,
                          uint8_t flow_id,
                          const void* payload,
                          uint16_t len,
                          uint64_t tx_deadline_ns);

status_t et_slave_send_to(et_slave_t* slave,
                          uint16_t dest_id,
                          uint8_t priority,
                          tuxrt_et_type_t type,
                          uint8_t flow_id,
                          const void* payload,
                          uint16_t len);

int et_slave_recv(et_slave_t* slave,
                  tuxrt_et_header_t* hdr,
                  void* payload,
                  uint32_t bufsize,
                  uint32_t timeout_us);

status_t et_esm_slave_transition(et_slave_t* slave, et_state_t next);

/* ------------------------------------------------------------------ */
/* Reserved space for future internal functions                       */
/* ------------------------------------------------------------------ */

#endif /* TUXRT_ET_INTERNAL_H */