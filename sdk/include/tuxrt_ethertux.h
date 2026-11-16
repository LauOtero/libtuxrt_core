/*
 * tuxrt_ethertux.h — Protocolo EtherTux de TuxRT (API pública, C11).
 *
 * EtherTux es el tejido de comunicación determinista para Ethernet COMERCIAL
 * (NICs y switches estándar, SBC heterogéneas y MCUs con controlador Ethernet
 * SPI/QSPI). Convive con el maestro EtherCAT: el operador elige uno u otro en
 * tiempo de configuración (`chronosrt_protocol.h`), nunca ambos a la vez sobre
 * la misma interfaz.
 *
 * Principios de diseño (blindaje absoluto):
 *
 *   1. Time-triggered para P0–P2: cada trama crítica tiene su ranura en un
 *      superframe; no hay contención.
 *   2. Antibloqueo: P0 es "express"; el planificador no encola tráfico de baja
 *      prioridad si hay una P0 pendiente (y la NIC puede preemptar si lo
 *      soporta).
 *   3. FEC Reed-Solomon: recuperación de paquetes perdidos SIN retransmisión.
 *   4. Cero congestión por diseño: control de admisión + token bucket
 *      jerárquico por clase + backpressure consciente de prioridad.
 *   5. LAPM: migración dinámica de prioridad con histéresis bajo carga.
 *   6. CRC-32C por instrucción en todas las SBCs (ARMv8.1+ / SSE4.2).
 *
 * Trama: Ethernet II con EtherType 0x88B5 y cabecera EtherTux de 22 bytes.
 * (El informe indica "20 bytes", pero la suma de los campos de su tabla es 22;
 * se implementa la tabla, que es la fuente de verdad de los campos.)
 */
#ifndef TUXRT_ETHERTUX_H
#define TUXRT_ETHERTUX_H

#include "tuxrt_sdk.h"
#include "tuxrt_hwaccel.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EtherType propio de EtherTux. */
#define TUXRT_ET_ETHERTYPE 0x88B5

/* Geometría (cabecera v0.1.0: 26 B en el cable). */
#define TUXRT_ET_HEADER_SIZE  26u    /* cabecera EtherTux en el cable   */
#define TUXRT_ET_ETH_HDR_SIZE 14u    /* cabecera Ethernet II            */
#define TUXRT_ET_MAX_FRAME    1518u  /* MTU Ethernet estándar           */
#define TUXRT_ET_MAX_PAYLOAD  1478u  /* MAX_FRAME - ETH_HDR - HEADER    */
#define TUXRT_ET_MAX_NODES    64u
#define TUXRT_ET_MAX_FLOWS    255u
#define TUXRT_ET_PRIORITIES   6u
#define TUXRT_ET_P0_MAX_FRAME 64u    /* las P0 se limitan a 64 B (§4.2) */

/* Tipos de trama (campo `Type`, §4.3). */
typedef enum {
    TUXRT_ET_TYPE_DATA            = 0x01,  /* datos / setpoints        */
    TUXRT_ET_TYPE_FEC             = 0x02,  /* paridad Reed-Solomon     */
    TUXRT_ET_TYPE_CONTROL         = 0x03,  /* emergencia, ACK/NACK     */
    TUXRT_ET_TYPE_SYNC            = 0x04,  /* sincronización básica    */
    TUXRT_ET_TYPE_LAPM            = 0x05,  /* migración de prioridad   */
    TUXRT_ET_TYPE_DISCOVERY       = 0x06,  /* Discovery / DiscoveryReply */
    TUXRT_ET_TYPE_SYNC_CALIBRATED = 0x07,  /* ET-Sync calibrado        */
    TUXRT_ET_TYPE_STATE_CHANGE    = 0x08,  /* transición ET-ESM        */
    TUXRT_ET_TYPE_MAILBOX_DATA    = 0x09,  /* ET-MBX: datos            */
    TUXRT_ET_TYPE_MAILBOX_ACK     = 0x0A,  /* ET-MBX: ACK/NACK         */
    TUXRT_ET_TYPE_SYNC_REPORT     = 0x0B,  /* Monitor de sincronización */
    TUXRT_ET_TYPE_TOPO_CHECK      = 0x0C   /* verificación topológica  */
} tuxrt_et_type_t;

/* Niveles de prioridad / clase de servicio. */
typedef enum {
    TUXRT_ET_P0 = 0,   /* express: emergencia, ACK/NACK crítico  */
    TUXRT_ET_P1 = 1,   /* express: sync, heartbeat, LAPM         */
    TUXRT_ET_P2 = 2,   /* time-triggered: setpoints de control   */
    TUXRT_ET_P3 = 3,   /* rate-constrained: telemetría           */
    TUXRT_ET_P4 = 4,   /* rate-constrained: configuración        */
    TUXRT_ET_P5 = 5    /* best-effort: logs, firmware, bulk      */
} tuxrt_et_priority_t;

/* Bits del campo `Flags` (§4.2). */
#define TUXRT_ET_FLAG_FEC          0x01u
#define TUXRT_ET_FLAG_PREEMPTABLE  0x02u
#define TUXRT_ET_FLAG_EXPRESS      0x04u
#define TUXRT_ET_FLAG_DISCOVERY    0x08u
#define TUXRT_ET_FLAG_FRM          0x10u   /* duplicado de redundancia FRM */

/* Estados ET-ESM del nodo emisor (§6.1). */
typedef enum {
    TUXRT_ET_STATE_INIT   = 0,   /* solo Discovery                     */
    TUXRT_ET_STATE_CONFIG = 1,   /* Discovery + mailbox                */
    TUXRT_ET_STATE_SAFE   = 2,   /* P0, P1 y mailbox                   */
    TUXRT_ET_STATE_OP     = 3,   /* P0–P5                              */
    TUXRT_ET_STATE_FAULT  = 4    /* cuarentena: solo Discovery         */
} tuxrt_et_state_t;

/* Cabecera EtherTux deserializada (v0.1.0, 26 B en el cable). */
typedef struct {
    uint8_t  type;          /* tuxrt_et_type_t                       */
    uint8_t  priority;      /* tuxrt_et_priority_t                   */
    uint8_t  flags;         /* TUXRT_ET_FLAG_*                       */
    uint8_t  state;         /* tuxrt_et_state_t del emisor           */
    uint8_t  flow_id;       /* 1..255                                    */
    uint32_t seq_num;       /* secuencia por flujo                       */
    uint64_t tx_timestamp;  /* ns del reloj monotónico en la transmisión */
    uint16_t wkc;           /* ET-WKC: nodos que procesaron la trama     */
    uint16_t block_id;      /* (bloque << 8) | índice dentro del bloque  */
    uint8_t  flow_count;    /* flujos agrupados (1 = sin agregación)     */
    uint32_t crc32;         /* CRC-32C de cabecera + carga útil          */
} tuxrt_et_header_t;

/* ------------------------------------------------------------------ */
/* Trama y CRC (helpers puros, verificables sin hardware)              */
/* ------------------------------------------------------------------ */

/** Serializa la cabecera (22 B, little-endian). Devuelve 22. */
TUXRT_SDK_API uint32_t tuxrt_et_header_build(uint8_t out[TUXRT_ET_HEADER_SIZE],
                                                const tuxrt_et_header_t* h);

/** Construye cabecera + carga útil y rellena el CRC-32C. Devuelve los bytes. */
TUXRT_SDK_API uint32_t tuxrt_et_pdu_build(uint8_t* out, uint32_t out_size,
                                              const tuxrt_et_header_t* h,
                                              const void* payload, uint16_t len);

/**
 * Construye SOLO la cabecera (26 B) con el CRC-32C ya calculado sobre
 * cabecera + carga útil, **sin copiar la carga útil**. Permite enviar con
 * `sendmsg`/iovec (cero copias de la aplicación) y evita el `memcpy` por trama.
 * Devuelve 26, o 0 si los parámetros no son válidos.
 */
TUXRT_SDK_API uint32_t tuxrt_et_pdu_build_header(uint8_t out[TUXRT_ET_HEADER_SIZE],
                                                     const tuxrt_et_header_t* h,
                                                     const void* payload, uint16_t len);

/* ------------------------------------------------------------------ */
/* Tasa de trama alcanzable (física del enlace)                        */
/* ------------------------------------------------------------------ */

/** Bytes en el cable de una trama: 14 Eth + 26 EtherTux + n + 4 FCS + 20 (preámbulo+IFG). */
TUXRT_SDK_API uint32_t tuxrt_et_wire_frame_bytes(uint32_t payload_len);

/**
 * Tramas por segundo que permite el enlace para esa carga útil (redondeo hacia
 * abajo). Es el techo **físico**: p. ej. 1 Gbps y 1464 B ⇒ ~81 800 tramas/s;
 * con 20 B (trama mínima de 64 B en el cable) ⇒ ~1 488 000 tramas/s.
 */
TUXRT_SDK_API uint32_t tuxrt_et_wire_pps(uint64_t link_bps, uint32_t payload_len);

/** Carga útil máxima que cabe a `pps` tramas/s en un enlace de `link_bps`. */
TUXRT_SDK_API uint32_t tuxrt_et_payload_for_pps(uint64_t link_bps, uint32_t pps);

/**
 * Deserializa y VALIDA el CRC. Devuelve los bytes consumidos o 0 si la trama
 * está mal formada o corrupta. `payload` apunta dentro de `in`.
 */
TUXRT_SDK_API uint32_t tuxrt_et_pdu_parse(const uint8_t* in, uint32_t in_len,
                                              tuxrt_et_header_t* h,
                                              const uint8_t** payload);

/** CRC-32C de cabecera (con el campo crc a cero) + carga útil. */
TUXRT_SDK_API uint32_t tuxrt_et_crc(const tuxrt_et_header_t* h,
                                        const void* payload, uint16_t len);

/** Nombre legible del tipo de trama. */
TUXRT_SDK_API const char* tuxrt_et_type_name(tuxrt_et_type_t type);

/* ------------------------------------------------------------------ */
/* FEC Reed-Solomon bloque a bloque                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    TUXRT_ET_FEC_OFF    = 0,   /* sin FEC                          */
    TUXRT_ET_FEC_LIGHT  = 1,   /* RS(10,8): 25 % de overhead       */
    TUXRT_ET_FEC_ROBUST = 2    /* RS(16,8): 100 % de overhead      */
} tuxrt_et_fec_mode_t;

typedef struct tuxrt_et_fec tuxrt_et_fec_t;

/** Crea un codec RS(n,k) explícito. Requiere 1 <= k < n <= 64. */
TUXRT_SDK_API tuxrt_et_fec_t* tuxrt_et_fec_create(uint8_t k, uint8_t n);
/** Crea el codec asociado a un modo predefinido (OFF => NULL). */
TUXRT_SDK_API tuxrt_et_fec_t* tuxrt_et_fec_create_mode(tuxrt_et_fec_mode_t mode);
TUXRT_SDK_API void tuxrt_et_fec_destroy(tuxrt_et_fec_t* fec);

TUXRT_SDK_API uint8_t  tuxrt_et_fec_k(const tuxrt_et_fec_t* fec);
TUXRT_SDK_API uint8_t  tuxrt_et_fec_n(const tuxrt_et_fec_t* fec);
TUXRT_SDK_API uint32_t tuxrt_et_fec_overhead_percent(const tuxrt_et_fec_t* fec);

/** Codifica k bloques de datos en n-k bloques de paridad. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_fec_encode(const tuxrt_et_fec_t* fec,
                                                         const uint8_t* const* data,
                                                         uint8_t* const* parity,
                                                         uint32_t len);

/** Reconstruye in situ hasta n-k bloques perdidos. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_fec_decode(const tuxrt_et_fec_t* fec,
                                                         uint8_t* const* blocks,
                                                         const bool* present,
                                                         uint32_t len);

/* ------------------------------------------------------------------ */
/* QoS: token bucket jerárquico, backpressure y LAPM                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t rate_bytes_per_sec;   /* tasa garantizada de la clase    */
    uint32_t burst_bytes;          /* ráfaga máxima admitida          */
} tuxrt_et_bucket_cfg_t;

typedef struct {
    tuxrt_et_bucket_cfg_t bucket[TUXRT_ET_PRIORITIES];
    uint8_t pause_percent;      /* backpressure P0 (def. 80 %)         */
    bool    lapm_enabled;       /* migración dinámica bajo carga       */
    uint8_t lapm_high_percent;  /* umbral de subida (def. 80 %)        */
    uint8_t lapm_low_percent;   /* umbral de bajada (def. 60 %, hist.) */
} tuxrt_et_qos_config_t;

typedef struct {
    uint64_t admitted;
    uint64_t backpressure;
    uint64_t dropped;
    uint64_t degraded;
    uint64_t bytes_admitted;
} tuxrt_et_qos_stats_t;

typedef struct tuxrt_et_qos tuxrt_et_qos_t;

TUXRT_SDK_API tuxrt_et_qos_t* tuxrt_et_qos_create(void);
TUXRT_SDK_API void tuxrt_et_qos_destroy(tuxrt_et_qos_t* qos);
TUXRT_SDK_API void tuxrt_et_qos_default_config(tuxrt_et_qos_config_t* cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_qos_configure(tuxrt_et_qos_t* qos,
                                                            const tuxrt_et_qos_config_t* cfg);

/**
 * Control de admisión. Devuelve:
 *   SUCCESS                 -> admitida (consume tokens)
 *   TUXRT_ERR_BACKPRESSURE -> P0–P4 no admitida: el productor debe esperar
 *   TUXRT_ERR_DROPPED      -> P5 descartada (best-effort)
 * La degradación entre clases (P1→P2→P4, P4→P5) es automática y con
 * histéresis.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_qos_admit(tuxrt_et_qos_t* qos,
                                                        uint8_t priority, uint32_t bytes,
                                                        uint64_t now_ns);

/** `true` si el productor de esa clase debe pausarse (backpressure activa). */
TUXRT_SDK_API bool tuxrt_et_qos_backpressure(const tuxrt_et_qos_t* qos, uint8_t priority);

/** Prioridad efectiva tras aplicar LAPM (puede ser mayor que la nominal). */
TUXRT_SDK_API uint8_t tuxrt_et_qos_effective_priority(const tuxrt_et_qos_t* qos,
                                                           uint8_t priority);

TUXRT_SDK_API tuxrt_err_t tuxrt_et_qos_stats(const tuxrt_et_qos_t* qos,
                                                        tuxrt_et_qos_stats_t* out);
TUXRT_SDK_API void tuxrt_et_qos_reset_stats(tuxrt_et_qos_t* qos);

/* ------------------------------------------------------------------ */
/* Planificador time-triggered (superframe con ventanas dedicadas)    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  priority;
    uint16_t node;
    uint16_t frame_bytes;
    uint32_t offset_ns;     /* desplazamiento dentro del superframe */
} tuxrt_et_slot_t;

typedef struct tuxrt_et_sched tuxrt_et_sched_t;

/** Crea el planificador. `link_bps` = 0 usa 1 Gbps. `cycle_ns` = 0 usa 1 ms. */
TUXRT_SDK_API tuxrt_et_sched_t* tuxrt_et_sched_create(uint32_t cycle_ns, uint64_t link_bps);
TUXRT_SDK_API void tuxrt_et_sched_destroy(tuxrt_et_sched_t* sched);

/** Reparte el superframe en las cuatro ventanas (control, determinista, rate, BE). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sched_set_windows(tuxrt_et_sched_t* sched,
                                                                uint32_t control_ns,
                                                                uint32_t deterministic_ns,
                                                                uint32_t rate_ns,
                                                                uint32_t best_effort_ns);

TUXRT_SDK_API tuxrt_err_t tuxrt_et_sched_add_slot(tuxrt_et_sched_t* sched,
                                                             uint8_t priority, uint16_t node,
                                                             uint16_t frame_bytes,
                                                             uint32_t offset_ns);
TUXRT_SDK_API int  tuxrt_et_sched_remove_node(tuxrt_et_sched_t* sched, uint16_t node);
TUXRT_SDK_API uint32_t tuxrt_et_sched_slot_count(const tuxrt_et_sched_t* sched);
TUXRT_SDK_API bool tuxrt_et_sched_slot(const tuxrt_et_sched_t* sched, uint32_t index,
                                           tuxrt_et_slot_t* out);
TUXRT_SDK_API uint32_t tuxrt_et_sched_cycle_ns(const tuxrt_et_sched_t* sched);

/** Ventana activa en `now_ns` (0=control, 1=determinista, 2=rate, 3=BE). */
TUXRT_SDK_API uint8_t tuxrt_et_sched_window_at(const tuxrt_et_sched_t* sched, uint64_t now_ns);

/** `true` si esa prioridad puede transmitir en la ventana actual. */
TUXRT_SDK_API bool tuxrt_et_sched_can_transmit(const tuxrt_et_sched_t* sched,
                                                    uint8_t priority, uint64_t now_ns);

/** Tiempo de transmisión de una trama a la tasa del enlace. */
TUXRT_SDK_API uint64_t tuxrt_et_tx_time_ns(uint32_t frame_bytes, uint64_t link_bps);

/* ------------------------------------------------------------------ */
/* Flow Aggregator (§7.6): varias entradas (FlowID, len, data)        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t        flow_id;
    uint16_t       len;
    const uint8_t* data;
} tuxrt_et_flow_entry_t;

/** Serializa la lista de flujos como carga útil agregada. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_flowagg_build(const tuxrt_et_flow_entry_t* entries,
                                                            uint32_t count,
                                                            uint8_t* out, uint32_t out_size,
                                                            uint32_t* out_len);

/** Deserializa la carga útil agregada en vistas sobre el propio búfer. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_flowagg_parse(const uint8_t* in, uint32_t len,
                                                            tuxrt_et_flow_entry_t* views,
                                                            uint32_t max_views, uint32_t* out_count);

/**
 * Bytes de cabecera ahorrados frente a enviar cada flujo en su propia trama.
 * Cada entrada cuesta 3 B (FlowID + longitud) en lugar de una cabecera EtherTux
 * completa (26 B) más la de Ethernet (14 B).
 */
TUXRT_SDK_API uint32_t tuxrt_et_flowagg_saved_bytes(uint32_t count, uint32_t data_bytes);

/* ------------------------------------------------------------------ */
/* ET-ESM: máquina de estados del nodo (§6.1)                          */
/* ------------------------------------------------------------------ */

typedef struct tuxrt_et_esm tuxrt_et_esm_t;

TUXRT_SDK_API tuxrt_et_esm_t* tuxrt_et_esm_create(void);
TUXRT_SDK_API void tuxrt_et_esm_destroy(tuxrt_et_esm_t* esm);
/** Vuelve a `INIT` (arranque o recuperación desde `FAULT`). */
TUXRT_SDK_API void tuxrt_et_esm_reset(tuxrt_et_esm_t* esm);
TUXRT_SDK_API tuxrt_et_state_t tuxrt_et_esm_state(const tuxrt_et_esm_t* esm);

/** Transiciones válidas: INIT→CONFIG→SAFE→OP y cualquiera→FAULT; FAULT→INIT. */
TUXRT_SDK_API bool tuxrt_et_esm_can_transition(tuxrt_et_state_t from,
                                                   tuxrt_et_state_t to);
/** Aplica una transición validada (el maestro la difunde vía `StateChange`). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_esm_request(tuxrt_et_esm_t* esm,
                                                          tuxrt_et_state_t next);

/** Tráfico permitido en cada estado (§6.1). */
TUXRT_SDK_API bool tuxrt_et_esm_allows_traffic(tuxrt_et_state_t state,
                                                   uint8_t type, uint8_t priority);

/** Carga útil de `StateChange`: estado destino (1 B) + motivo (1 B). */
TUXRT_SDK_API uint32_t tuxrt_et_esm_build_state_change(tuxrt_et_state_t target,
                                                           uint8_t reason,
                                                           uint8_t out[2]);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_esm_parse_state_change(const uint8_t* in, uint32_t len,
                                                                    tuxrt_et_state_t* target,
                                                                    uint8_t* reason);
/** Valida el campo `State` de una trama; falso si no es un estado conocido. */
TUXRT_SDK_API bool tuxrt_et_esm_state_from_wire(uint8_t raw, tuxrt_et_state_t* out);

/* ------------------------------------------------------------------ */
/* ET-Sync + Monitor de Ventana de Sincronización (§5)                 */
/* ------------------------------------------------------------------ */

typedef enum {
    TUXRT_ET_SYNC_OK = 0,
    TUXRT_ET_SYNC_WARN = 1,
    TUXRT_ET_SYNC_OUT_OF_SYNC = 2
} tuxrt_et_sync_status_t;

typedef struct {
    uint32_t warn_threshold_ns;         /* desviación para WARN (def. 1 µs)   */
    uint32_t out_of_sync_threshold_ns;  /* desviación para fuera de sync (2 µs) */
    uint32_t report_period_frames;      /* periodo de SyncReport (def. 10)    */
} tuxrt_et_sync_config_t;

typedef struct {
    uint32_t nodes;
    uint32_t ok;
    uint32_t warn;
    uint32_t out_of_sync;
    uint64_t max_abs_offset_ns;
} tuxrt_et_sync_stats_t;

typedef struct tuxrt_et_sync tuxrt_et_sync_t;

TUXRT_SDK_API tuxrt_et_sync_t* tuxrt_et_sync_create(void);
TUXRT_SDK_API void tuxrt_et_sync_destroy(tuxrt_et_sync_t* sync);
TUXRT_SDK_API void tuxrt_et_sync_default_config(tuxrt_et_sync_config_t* cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_configure(tuxrt_et_sync_t* sync,
                                                             const tuxrt_et_sync_config_t* cfg);
/**
 * Modelo de retardo calibrado del switch para un nodo (medido en `CONFIG`).
 * El offset reportado se corrige restándole este retardo.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_set_link_delay(tuxrt_et_sync_t* sync,
                                                                  uint16_t node_id,
                                                                  uint32_t delay_ns);
/** Procesa un `SyncReport` (offset medido por el nodo) y clasifica el estado. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_process_report(tuxrt_et_sync_t* sync,
                                                                  uint16_t node_id,
                                                                  int64_t measured_offset_ns,
                                                                  uint64_t now_ns,
                                                                  tuxrt_et_sync_status_t* out_status);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_status_of(const tuxrt_et_sync_t* sync,
                                                             uint16_t node_id,
                                                             int64_t* offset_ns,
                                                             tuxrt_et_sync_status_t* out_status);
/** Deriva estimada en partes por billón (Δoffset/Δt). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_drift_ppb(const tuxrt_et_sync_t* sync,
                                                             uint16_t node_id,
                                                             int64_t* out_ppb);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_stats(const tuxrt_et_sync_t* sync,
                                                         tuxrt_et_sync_stats_t* out);
/** Carga útil de `SyncCalibrated`: timestamp de referencia del maestro (8 B). */
TUXRT_SDK_API uint32_t tuxrt_et_sync_build_calibrated(uint64_t master_ns, uint8_t out[8]);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_parse_calibrated(const uint8_t* in, uint32_t len,
                                                                   uint64_t* out_master_ns);
/** Carga útil de `SyncReport`: offset medido por el nodo (8 B, int64). */
TUXRT_SDK_API uint32_t tuxrt_et_sync_build_report(int64_t offset_ns, uint8_t out[8]);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_sync_parse_report(const uint8_t* in, uint32_t len,
                                                               int64_t* out_offset_ns);

/* ------------------------------------------------------------------ */
/* Registro de nodos, ET-WKC y Hot Connect (§6.2–6.5)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t id;                  /* NodeID                                     */
    uint8_t  mac[6];
    uint16_t link_mbps;           /* capacidad declarada                        */
    uint16_t buffer_bytes;
    bool     fec_supported;
    uint16_t slot;                /* ranura TDMA (0xFFFF = libre)               */
    tuxrt_et_state_t state;   /* estado ET-ESM                              */
    uint16_t last_wkc;            /* último ET-WKC observado                    */
    uint16_t expected_wkc;        /* ET-WKC esperado según el registro          */
    uint8_t  wkc_mismatch_count;  /* discrepancias consecutivas                 */
    int64_t  sync_offset_ns;
    tuxrt_et_sync_status_t sync_status;
    uint64_t last_seen_ns;        /* temporizador de heartbeat                  */
} tuxrt_et_node_info_t;

typedef struct {
    uint8_t  wkc_mismatch_threshold;    /* def. 3 superframes                  */
    uint32_t hot_connect_period_frames; /* def. 100 superframes (100 ms)       */
    uint32_t heartbeat_timeout_frames;  /* def. 3 superframes                  */
} tuxrt_et_topology_config_t;

#define TUXRT_ET_SLOT_FREE 0xFFFFu

/** Tamaño del `DiscoveryReply` en el cable. */
#define TUXRT_ET_DISCOVERY_REPLY_SIZE 8u

typedef struct tuxrt_et_topology tuxrt_et_topology_t;

TUXRT_SDK_API tuxrt_et_topology_t* tuxrt_et_topology_create(void);
TUXRT_SDK_API void tuxrt_et_topology_destroy(tuxrt_et_topology_t* topo);
TUXRT_SDK_API void tuxrt_et_topology_default_config(tuxrt_et_topology_config_t* cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_topology_configure(tuxrt_et_topology_t* topo,
                                                                 const tuxrt_et_topology_config_t* cfg);

/** Inserta o actualiza un nodo del registro. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_topology_upsert(tuxrt_et_topology_t* topo,
                                                              const tuxrt_et_node_info_t* info);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_topology_remove(tuxrt_et_topology_t* topo,
                                                              uint16_t node_id);
TUXRT_SDK_API const tuxrt_et_node_info_t* tuxrt_et_topology_find(const tuxrt_et_topology_t* topo,
                                                                         uint16_t node_id);
TUXRT_SDK_API uint32_t tuxrt_et_topology_count(const tuxrt_et_topology_t* topo);
TUXRT_SDK_API const tuxrt_et_node_info_t* tuxrt_et_topology_at(const tuxrt_et_topology_t* topo,
                                                                      uint32_t index);
TUXRT_SDK_API uint16_t tuxrt_et_topology_next_free_slot(const tuxrt_et_topology_t* topo);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_topology_assign_slot(tuxrt_et_topology_t* topo,
                                                                   uint16_t node_id,
                                                                   uint16_t slot);

/**
 * Procesa una trama recibida: refresca el heartbeat, el estado ET-ESM y compara
 * el ET-WKC con el esperado (incrementando `wkc_mismatch_count` al discrepar).
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_topology_on_frame(tuxrt_et_topology_t* topo,
                                                                const uint8_t mac[6],
                                                                const tuxrt_et_header_t* h,
                                                                uint64_t now_ns);
/** `true` si algún nodo superó el umbral de discrepancias y toca `TopoCheck`. */
TUXRT_SDK_API bool tuxrt_et_topology_needs_topo_check(const tuxrt_et_topology_t* topo,
                                                          uint16_t* out_node_id);
/** Marca en `FAULT` los nodos sin heartbeat y libera sus slots. */
TUXRT_SDK_API uint32_t tuxrt_et_topology_tick(tuxrt_et_topology_t* topo,
                                                  uint64_t now_ns, uint32_t cycle_ns);
/** `true` si toca un `Discovery` unicast a una dirección libre (Hot Connect). */
TUXRT_SDK_API bool tuxrt_et_topology_hot_connect_due(const tuxrt_et_topology_t* topo,
                                                         uint64_t frame_index,
                                                         uint16_t* out_node_addr);

TUXRT_SDK_API uint32_t tuxrt_et_topology_build_reply(const tuxrt_et_node_info_t* info,
                                                         uint8_t out[TUXRT_ET_DISCOVERY_REPLY_SIZE]);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_topology_parse_reply(const uint8_t* in, uint32_t len,
                                                                   tuxrt_et_node_info_t* out);

/* ------------------------------------------------------------------ */
/* ET-MBX: mailbox de gestión (§9)                                     */
/* ------------------------------------------------------------------ */

#define TUXRT_ET_MBX_CHANNELS       8u
#define TUXRT_ET_MBX_SEGMENT_BYTES  512u
#define TUXRT_ET_MBX_MAX_RETRIES    3u
#define TUXRT_ET_MBX_ACK_SIZE       4u

typedef enum {
    TUXRT_ET_MBX_IDLE = 0,
    TUXRT_ET_MBX_SENDING,
    TUXRT_ET_MBX_WAIT_ACK,
    TUXRT_ET_MBX_DONE,
    TUXRT_ET_MBX_FAILED
} tuxrt_et_mbx_state_t;

typedef struct {
    uint8_t  max_retries;        /* def. 3                                      */
    uint16_t segment_bytes;      /* def. 512                                    */
    uint8_t  p4_degrade_percent; /* def. 70: por encima se degrada a P5          */
} tuxrt_et_mbx_config_t;

typedef struct {
    tuxrt_et_mbx_state_t state;
    uint16_t seq;
    uint16_t segment;
    uint16_t total_segments;
    uint8_t  retries;
    bool     firmware;           /* firmware => P5                              */
} tuxrt_et_mbx_channel_status_t;

typedef struct tuxrt_et_mbx tuxrt_et_mbx_t;

TUXRT_SDK_API tuxrt_et_mbx_t* tuxrt_et_mbx_create(void);
TUXRT_SDK_API void tuxrt_et_mbx_destroy(tuxrt_et_mbx_t* mbx);
TUXRT_SDK_API void tuxrt_et_mbx_default_config(tuxrt_et_mbx_config_t* cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_mbx_configure(tuxrt_et_mbx_t* mbx,
                                                            const tuxrt_et_mbx_config_t* cfg);
/** Inicia una transferencia en un canal lógico (0..7). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_mbx_begin(tuxrt_et_mbx_t* mbx, uint8_t channel,
                                                        uint32_t total_bytes, bool firmware);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_mbx_status(const tuxrt_et_mbx_t* mbx,
                                                         uint8_t channel,
                                                         tuxrt_et_mbx_channel_status_t* out);
/**
 * Construye el siguiente segmento (`MailboxData`): canal, seq, índice, total,
 * flags y hasta `segment_bytes` de datos. Devuelve los bytes escritos.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_mbx_next_segment(tuxrt_et_mbx_t* mbx,
                                                               uint8_t channel,
                                                               const uint8_t* data, uint32_t len,
                                                               uint8_t* out, uint32_t out_size,
                                                               uint32_t* out_len);
/** Procesa el ACK/NACK del nodo; programa el reintento con backoff si procede. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_mbx_on_ack(tuxrt_et_mbx_t* mbx, uint8_t channel,
                                                         uint16_t seq, bool accepted,
                                                         uint64_t now_ns);
/** `true` si el reintento pendiente ya venció (backoff exponencial). */
TUXRT_SDK_API bool tuxrt_et_mbx_retry_due(const tuxrt_et_mbx_t* mbx, uint8_t channel,
                                              uint64_t now_ns);
/** Prioridad efectiva del mailbox según la ocupación de P4. */
TUXRT_SDK_API uint8_t tuxrt_et_mbx_effective_priority(const tuxrt_et_mbx_t* mbx,
                                                          uint8_t p4_occupancy_percent);
TUXRT_SDK_API uint32_t tuxrt_et_mbx_build_ack(uint8_t channel, uint16_t seq, bool accepted,
                                                  uint8_t out[TUXRT_ET_MBX_ACK_SIZE]);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_mbx_parse_ack(const uint8_t* in, uint32_t len,
                                                            uint8_t* channel, uint16_t* seq,
                                                            bool* accepted);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_mbx_parse_data(const uint8_t* in, uint32_t len,
                                                             uint8_t* channel, uint16_t* seq,
                                                             uint16_t* index, uint16_t* total,
                                                             const uint8_t** data, uint16_t* data_len);

/* ------------------------------------------------------------------ */
/* FRM: redundancia lógica de flujo (§8.2)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t duplicates_tx;
    uint64_t duplicates_rx;
    uint64_t dropped_dup;
    uint32_t path_switches;
} tuxrt_et_frm_stats_t;

typedef struct tuxrt_et_frm tuxrt_et_frm_t;

TUXRT_SDK_API tuxrt_et_frm_t* tuxrt_et_frm_create(void);
TUXRT_SDK_API void tuxrt_et_frm_destroy(tuxrt_et_frm_t* frm);
TUXRT_SDK_API void tuxrt_et_frm_reset(tuxrt_et_frm_t* frm);
/** Solo P0/P1 se duplican (flujos críticos). */
TUXRT_SDK_API bool tuxrt_et_frm_should_duplicate(const tuxrt_et_frm_t* frm, uint8_t priority);
/** FlowID del camino alternativo (bit 7 invertido, evitando 0). */
TUXRT_SDK_API uint8_t tuxrt_et_frm_alternate_flow(uint8_t flow_id);
/** Deduplica por (nodo, SeqNum): `out_is_new` = false si ya se entregó. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_frm_note_rx(tuxrt_et_frm_t* frm, uint16_t node_id,
                                                          uint32_t seq_num, bool duplicate_flag,
                                                          bool* out_is_new);
/** Conmuta el flujo del nodo al camino alternativo tras un fallo de enlace. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_frm_fail_path(tuxrt_et_frm_t* frm, uint16_t node_id);
TUXRT_SDK_API uint8_t tuxrt_et_frm_active_path(const tuxrt_et_frm_t* frm, uint16_t node_id);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_frm_stats(const tuxrt_et_frm_t* frm,
                                                        tuxrt_et_frm_stats_t* out);

/* ------------------------------------------------------------------ */
/* ID local determinista: MAC LAA derivada del ID (2 niveles)          */
/*                                                                     */
/* Nivel 1 (camino crítico): `tuxrt_et_aalid_candidate()` calcula   */
/* el ID candidato con CRC-32C + plegado XOR. Es una función sin ramas  */
/* ni bucles, de tiempo constante, y NO garantiza unicidad: con 16     */
/* bits la paradoja del cumpleaños aparece hacia los ~300 nodos.        */
/*                                                                     */
/* Nivel 2 (solo admisión): el maestro es la autoridad final. Mantiene  */
/* una tabla {MAC de fábrica -> ID} y un bitmap de IDs ocupados;        */
/* resuelve colisiones con un cursor secuencial determinista, fuera del */
/* superframe. El resultado es único en régimen permanente.             */
/* ------------------------------------------------------------------ */

#define TUXRT_ET_LOCAL_ID_MAX  0xFFFFu   /* 65.536 identificadores */
#define TUXRT_ET_LOCAL_ID_NONE 0xFFFFu

/**
 * MAC local (LAA) del ID: `02:00:00:00:ID[15:8]:ID[7:0]`. El bit U/L queda
 * a 1 (dirección administrada localmente) y el I/G a 0 (unicast).
 */
TUXRT_SDK_API uint32_t tuxrt_et_aalid_mac(uint16_t id, uint8_t out[6]);

/** ID codificado en una MAC local; `TUXRT_ET_LOCAL_ID_NONE` si no lo es. */
TUXRT_SDK_API uint16_t tuxrt_et_aalid_id_from_mac(const uint8_t mac[6]);

/** Nivel 1: ID candidato (CRC-32C de la MAC plegado a 16 bits). */
TUXRT_SDK_API uint16_t tuxrt_et_aalid_candidate(const uint8_t mac[6]);

/** Nivel 1 alternativo sin CRC: hash multiplicativo (fallback portable). */
TUXRT_SDK_API uint16_t tuxrt_et_aalid_candidate_mul(const uint8_t mac[6]);

/**
 * Backoff determinista derivado de la MAC: entero en `[0, slots)`. Reproducible
 * entre ejecuciones (a diferencia de `rand()`) y distinto para MACs distintas.
 * `slots` debe ser potencia de 2.
 */
TUXRT_SDK_API uint32_t tuxrt_et_aalid_backoff_slots(const uint8_t mac[6], uint32_t slots);

/** Resultado de una solicitud de admisión. */
typedef enum {
    TUXRT_ET_AALID_OK = 0,           /* ID candidato libre            */
    TUXRT_ET_AALID_COLLISION = 1,    /* candidato ocupado: se resolvió */
    TUXRT_ET_AALID_DUPLICATE = 2,    /* la MAC ya tenía ID: se devuelve */
    TUXRT_ET_AALID_CLONE = 3,        /* misma MAC, distinto nonce      */
    TUXRT_ET_AALID_FULL = 4,         /* no quedan IDs libres           */
    TUXRT_ET_AALID_INVALID = 5
} tuxrt_et_aalid_result_t;

typedef struct {
    uint16_t id_max;        /* IDs asignables - 1 (def. 65535)             */
    uint32_t grace_ms;      /* tiempo de gracia sin heartbeat (def. 600000)*/
    uint32_t table_slots;   /* potencia de 2 (def. 1024; 0 => 1024)        */
} tuxrt_et_aalid_config_t;

typedef struct {
    uint64_t admissions;    /* IDs concedidos                              */
    uint64_t collisions;    /* candidatos ocupados que hubo que reasignar  */
    uint64_t duplicates;    /* reclamaciones de una MAC ya registrada      */
    uint64_t released;      /* liberaciones explícitas                     */
    uint64_t expired;       /* IDs liberados por timeout de heartbeat      */
    uint64_t clone_mac;     /* solicitudes con MAC clonada (rechazadas)    */
    uint32_t occupied;      /* IDs en uso                                  */
} tuxrt_et_aalid_stats_t;

typedef struct tuxrt_et_aalid tuxrt_et_aalid_t;

TUXRT_SDK_API void tuxrt_et_aalid_default_config(tuxrt_et_aalid_config_t* cfg);
TUXRT_SDK_API tuxrt_et_aalid_t* tuxrt_et_aalid_create(const tuxrt_et_aalid_config_t* cfg);
TUXRT_SDK_API void tuxrt_et_aalid_destroy(tuxrt_et_aalid_t* aalid);

/**
 * Resuelve el ID final de `mac` (Nivel 2). Determinista: la misma secuencia de
 * llegadas produce siempre la misma asignación. Devuelve SUCCESS con el ID en
 * `out_id` (también en duplicados y colisiones) o `TUXRT_ERR_BUSY` si la
 * MAC está clonada, `TUXRT_ERR_OVERFLOW` si no quedan IDs.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_aalid_admit(tuxrt_et_aalid_t* aalid,
                                                          const uint8_t mac[6],
                                                          uint32_t nonce,
                                                          uint64_t now_ms,
                                                          uint16_t* out_id,
                                                          tuxrt_et_aalid_result_t* out_result);

TUXRT_SDK_API tuxrt_err_t tuxrt_et_aalid_release(tuxrt_et_aalid_t* aalid,
                                                           uint16_t id);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_aalid_lookup(const tuxrt_et_aalid_t* aalid,
                                                           const uint8_t mac[6],
                                                           uint16_t* out_id);
/** Refresca el temporizador de vida de un ID (mantiene viva la entrada CAM). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_aalid_heartbeat(tuxrt_et_aalid_t* aalid,
                                                              uint16_t id, uint64_t now_ms);
/** Libera los IDs sin heartbeat dentro del tiempo de gracia. Devuelve cuántos. */
TUXRT_SDK_API uint32_t tuxrt_et_aalid_expire(tuxrt_et_aalid_t* aalid, uint64_t now_ms);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_aalid_stats(const tuxrt_et_aalid_t* aalid,
                                                          tuxrt_et_aalid_stats_t* out);
TUXRT_SDK_API bool tuxrt_et_aalid_is_occupied(const tuxrt_et_aalid_t* aalid, uint16_t id);

/**
 * Persistencia {MAC -> ID} para conservar la identidad tras un reinicio.
 * `save` escribe de forma atómica (fichero temporal + `rename`).
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_aalid_save(const tuxrt_et_aalid_t* aalid,
                                                         const char* path);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_aalid_load(tuxrt_et_aalid_t* aalid,
                                                         const char* path);

/* ------------------------------------------------------------------ */
/* Nodos y maestro                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t id;
    uint8_t  mac[6];
    int64_t  last_seen_ns;
    int64_t  clock_offset_ns;   /* offset estimado por RTT (ns)      */
    uint32_t rtt_ns;
    uint32_t missed_cycles;
    bool     lost;
} tuxrt_et_node_t;

typedef struct {
    uint64_t frames_tx;
    uint64_t frames_rx;
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    uint64_t dropped;
    uint64_t backpressure;
    uint64_t fec_blocks;
    uint64_t frame_errors;
    /* --- v0.1.0 --- */
    uint64_t duplicates_tx;     /* tramas duplicadas por FRM               */
    uint64_t wkc_mismatches;    /* discrepancias de ET-WKC observadas      */
    uint64_t sync_out_of_sync;  /* nodos marcados fuera de sincronización  */
    uint64_t mbx_retries;       /* reintentos de ET-MBX                    */
    uint64_t local_id_dropped;  /* descartadas por destino ajeno (ID local) */
} tuxrt_et_master_stats_t;

typedef struct tuxrt_et_master tuxrt_et_master_t;

TUXRT_SDK_API tuxrt_et_master_t* tuxrt_et_master_create(void);
TUXRT_SDK_API void tuxrt_et_master_destroy(tuxrt_et_master_t* master);

/** Abre `ifname` como EtherTux (AF_PACKET, EtherType 0x88B5). 1 Gbps / 1 ms. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_open(tuxrt_et_master_t* master,
                                                           const char* ifname);
/** Igual, declarando la tasa del enlace y el superframe. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_open_ex(tuxrt_et_master_t* master,
                                                             const char* ifname,
                                                             uint64_t link_bps,
                                                             uint32_t cycle_ns);
TUXRT_SDK_API void tuxrt_et_master_close(tuxrt_et_master_t* master);
TUXRT_SDK_API bool tuxrt_et_master_is_open(const tuxrt_et_master_t* master);
TUXRT_SDK_API const char* tuxrt_et_master_interface(const tuxrt_et_master_t* master);

TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_set_qos(tuxrt_et_master_t* master,
                                                             const tuxrt_et_qos_config_t* cfg);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_set_fec(tuxrt_et_master_t* master,
                                                             tuxrt_et_fec_mode_t mode);
/** Asocia (sin tomar propiedad) un planificador al maestro. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_attach_scheduler(tuxrt_et_master_t* master,
                                                                     tuxrt_et_sched_t* sched);

/* Asociaciones opcionales (sin propiedad) de los módulos v0.1.0. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_attach_topology(tuxrt_et_master_t* master,
                                                                    tuxrt_et_topology_t* topo);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_attach_sync(tuxrt_et_master_t* master,
                                                                tuxrt_et_sync_t* sync);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_attach_esm(tuxrt_et_master_t* master,
                                                               tuxrt_et_esm_t* esm);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_attach_mbx(tuxrt_et_master_t* master,
                                                               tuxrt_et_mbx_t* mbx);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_attach_frm(tuxrt_et_master_t* master,
                                                               tuxrt_et_frm_t* frm);

/* --- ID local determinista en el maestro ---------------------------- */

/**
 * Activa el esquema de ID local (Nivel 2) en el maestro. A partir de aquí la
 * recepción descarta las tramas cuyo destino no sea la MAC local ni difusión, y
 * los nodos se identifican con su ID (O(1)) en lugar de buscar por MAC.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_enable_local_ids(
        tuxrt_et_master_t* master, const tuxrt_et_aalid_config_t* cfg);

/** Fija el ID local del maestro y deriva de él su MAC de origen (LAA). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_set_local_id(tuxrt_et_master_t* master,
                                                                  uint16_t id);

/** ID local del maestro (`TUXRT_ET_LOCAL_ID_NONE` si no hay). */
TUXRT_SDK_API uint16_t tuxrt_et_master_local_id(const tuxrt_et_master_t* master);

/** Autoridad de admisión: resuelve el ID final de una MAC (Nivel 2). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_admit_node(tuxrt_et_master_t* master,
                                                                const uint8_t mac[6],
                                                                uint32_t nonce,
                                                                uint64_t now_ms,
                                                                uint16_t* out_id,
                                                                tuxrt_et_aalid_result_t* out_result);

/** Libera los IDs sin heartbeat dentro del tiempo de gracia. Devuelve cuántos. */
TUXRT_SDK_API uint32_t tuxrt_et_master_expire_local_ids(tuxrt_et_master_t* master,
                                                            uint64_t now_ms);

/**
 * `true` si una trama dirigida a `mac` interesa a este maestro (difusión o la
 * propia MAC local). Es el filtro del camino de recepción, en tiempo constante.
 */
TUXRT_SDK_API bool tuxrt_et_master_accepts_destination(const tuxrt_et_master_t* master,
                                                           const uint8_t mac[6]);

/** Estado de admisión asociado (NULL si no está activado). */
TUXRT_SDK_API tuxrt_et_aalid_t* tuxrt_et_master_aalid(tuxrt_et_master_t* master);

/**
 * Envía a un nodo concreto usando su ID local (destino unicast `02:...:ID`), en
 * lugar de difusión: evita el *flooding* del switch y el tráfico parásito en el
 * resto de puertos. `dest_id == TUXRT_ET_LOCAL_ID_NONE` equivale a difusión.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_send_to(tuxrt_et_master_t* master,
                                                             uint16_t dest_id,
                                                             uint8_t priority,
                                                             tuxrt_et_type_t type,
                                                             uint8_t flow_id,
                                                             const void* payload, uint16_t len);


/** ET-WKC de la última trama recibida. */
TUXRT_SDK_API uint16_t tuxrt_et_master_last_wkc(const tuxrt_et_master_t* master);

/** Velocidad del enlace en bps según sysfs (`/sys/class/net/<if>/speed`); 0 si no se puede leer. */
TUXRT_SDK_API uint64_t tuxrt_et_link_speed_bps(const char* ifname);

/**
 * Abre la interfaz detectando la velocidad real del enlace (1G/2.5G/5G/10G) y
 * ajustando el superframe al presupuesto de bytes correspondiente. Si no se
 * puede leer la velocidad, usa 1 Gbps.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_open_auto(tuxrt_et_master_t* master,
                                                               const char* ifname);

/**
 * Envía sin copiar la carga útil: cabecera + payload en dos iovecs (`sendmsg`).
 * La carga debe permanecer válida hasta que la llamada retorne.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_send_zerocopy(tuxrt_et_master_t* master,
                                                                   uint8_t priority,
                                                                   tuxrt_et_type_t type,
                                                                   uint8_t flow_id,
                                                                   const void* payload,
                                                                   uint16_t len);

/** Elemento de un envío por lotes. */
typedef struct {
    uint8_t              priority;
    tuxrt_et_type_t  type;
    uint8_t              flow_id;
    const void*          payload;
    uint16_t             len;
} tuxrt_et_batch_item_t;

/**
 * Envía hasta 16 tramas en **una sola llamada al sistema** (`sendmmsg`),
 * amortizando el coste de syscall por trama. Devuelve el número de tramas
 * entregadas al kernel, o -1 si falla el envío completo.
 */
TUXRT_SDK_API int tuxrt_et_master_send_batch(tuxrt_et_master_t* master,
                                                 const tuxrt_et_batch_item_t* items,
                                                 uint32_t count);

/* ------------------------------------------------------------------ */
/* Modo de transmisión (llamada al sistema vs. anillo en user space)   */
/* ------------------------------------------------------------------ */

typedef enum {
    TUXRT_ET_TX_AUTO    = 0,  /* anillo TX si el kernel lo permite     */
    TUXRT_ET_TX_SYSCALL = 1,  /* sendmsg / sendmmsg                    */
    TUXRT_ET_TX_RING    = 2   /* PACKET_TX_RING obligatorio             */
} tuxrt_et_tx_mode_t;

/**
 * Selecciona el camino de transmisión. Con el anillo (`PACKET_TX_RING`) la
 * trama se escribe en una ranura mapeada en user space y un único `send()`
 * vacía el lote completo: se elimina la llamada al sistema por trama, que es
 * el coste dominante con tramas pequeñas. `AUTO` lo intenta y, si el kernel o
 * la interfaz no lo permiten, continúa con llamadas al sistema.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_set_tx_mode(tuxrt_et_master_t* master,
                                                                 tuxrt_et_tx_mode_t mode);

/** `true` si el anillo de transmisión está activo en esta interfaz. */
TUXRT_SDK_API bool tuxrt_et_master_tx_ring_active(const tuxrt_et_master_t* master);

/** Envía una PDU (aplica QoS, CRC-32C y, si procede, marca FEC). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_send(tuxrt_et_master_t* master,
                                                          uint8_t priority,
                                                          tuxrt_et_type_t type,
                                                          uint8_t flow_id,
                                                          const void* payload, uint16_t len);

/** Envía con pacing por hardware (SO_TXTIME) si la NIC lo soporta. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_send_at(tuxrt_et_master_t* master,
                                                             uint8_t priority,
                                                             tuxrt_et_type_t type,
                                                             uint8_t flow_id,
                                                             const void* payload, uint16_t len,
                                                             uint64_t tx_deadline_ns);

/**
 * Envía varios flujos agregados en una sola trama (`FlowCount > 1`, §7.6).
 * La carga útil se serializa como lista de entradas (FlowID, longitud, datos).
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_send_aggregated(tuxrt_et_master_t* master,
                                                                     uint8_t priority,
                                                                     uint8_t flow_id,
                                                                     const tuxrt_et_flow_entry_t* entries,
                                                                     uint32_t count);

/**
 * Envía un segmento de ET-MBX (§9): prioridad P4, o P5 si la ocupación de P4
 * supera el umbral configurado. Requiere un mailbox asociado.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_send_mailbox(tuxrt_et_master_t* master,
                                                                  uint8_t channel,
                                                                  const void* data, uint16_t len,
                                                                  uint8_t p4_occupancy_percent);

/**
 * Envía un bloque FEC completo: `count` PDUs de datos (count debe ser K) más
 * los N-K bloques de paridad. Los datos deben tener todos `len` bytes.
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_send_fec_block(tuxrt_et_master_t* master,
                                                                    uint8_t priority,
                                                                    uint8_t flow_id,
                                                                    const uint8_t* const* data,
                                                                    uint32_t count,
                                                                    uint32_t len);

/** Recibe una PDU. Devuelve los bytes de carga útil, o -1 en timeout/error. */
TUXRT_SDK_API int tuxrt_et_master_recv(tuxrt_et_master_t* master,
                                           tuxrt_et_header_t* hdr,
                                           void* payload, uint32_t bufsize,
                                           uint32_t timeout_us);

/** Marca caídos los nodos sin heartbeat en 3 superframes. Devuelve cuántos. */
TUXRT_SDK_API uint32_t tuxrt_et_master_heartbeat(tuxrt_et_master_t* master,
                                                     uint64_t now_ns);
TUXRT_SDK_API uint32_t tuxrt_et_master_node_count(const tuxrt_et_master_t* master);
TUXRT_SDK_API bool tuxrt_et_master_node(const tuxrt_et_master_t* master, uint32_t index,
                                            tuxrt_et_node_t* out);
/** Sincroniza el reloj de un nodo por RTT (offset ≈ -RTT/2). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_rtt_sync(tuxrt_et_master_t* master,
                                                              const uint8_t mac[6],
                                                              uint32_t rtt_ns);

TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_stats(const tuxrt_et_master_t* master,
                                                           tuxrt_et_master_stats_t* out);
TUXRT_SDK_API tuxrt_err_t tuxrt_et_master_reset_stats(tuxrt_et_master_t* master);

/* ------------------------------------------------------------------ */
/* modo de operación: master vs esclavo                               */
/* ------------------------------------------------------------------ */

typedef enum {
    TUXRT_ET_MODE_MASTER = 0,  /* modo maestro: descubrimiento, QoS, scheduling */
    TUXRT_ET_MODE_SLAVE  = 1   /* modo esclavo: send/receive básico, sin nodo table */
} tuxrt_et_mode_t;

/* Estructura opaca de contexto del esclavo (comparte et_port_t pero sin campos master) */
typedef struct tuxrt_et_slave tuxrt_et_slave_t;

/* Estadísticas del modo esclavo */
typedef struct {
    uint64_t frames_tx;      /* tramas transmitidas exitosas   */
    uint64_t frames_rx;      /* tramas recibidas (después de filtro) */
    uint64_t bytes_tx;       /* bytes transmitidos             */
    uint64_t bytes_rx;       /* bytes recibidos (después de filtro) */
    uint64_t dropped;        /* tramas descartadas (filtro destino) */
    uint64_t frame_errors;   /* errores de formato de trama    */
} tuxrt_et_slave_stats_t;

/* ------------------------------------------------------------------ */
/* Funciones del modo esclavo                                         */
/* ------------------------------------------------------------------ */

/** Crea un contexto de esclavo para la interfaz dada.
 *  \param ifname Nombre de la interfaz de red (ej. "eth0")
 *  \param link_bps Velocidad del enlace en bps (0 = 1 Gbps por defecto)
 *  \param cycle_ns Periodo del superframe en ns (0 = 1 ms por defecto)
 *  \out out_ctx Puntero al nuevo contexto de esclavo (opaco)
 *  \return TUXRT_OK si éxito, o error negativo
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_create(const char* ifname,
                                                 uint64_t link_bps,
                                                 uint32_t cycle_ns,
                                                 tuxrt_et_slave_t** out_ctx);

/** Destruye un contexto de esclavo y libera recursos. */
TUXRT_SDK_API void tuxrt_et_slave_destroy(tuxrt_et_slave_t* slave);

/** Abre la interfaz EtherTux para el modo esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_open(tuxrt_et_slave_t* slave, const char* ifname);

/** Cierra la interfaz EtherTux para el modo esclavo. */
TUXRT_SDK_API void tuxrt_et_slave_close(tuxrt_et_slave_t* slave);

/** Envía una trama en modo esclavo (destino broadcast). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_send(tuxrt_et_slave_t* slave,
                                                 uint8_t priority,
                                                 tuxrt_et_type_t type,
                                                 uint8_t flow_id,
                                                 const void* payload,
                                                 uint16_t len);

/** Envía una trama a un nodo específico en modo esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_send_to(tuxrt_et_slave_t* slave,
                                                  uint16_t dest_id,
                                                  uint8_t priority,
                                                  tuxrt_et_type_t type,
                                                  uint8_t flow_id,
                                                  const void* payload,
                                                  uint16_t len);

/** Envía una trama con pacing por hardware (SO_TXTIME) en modo esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_send_at(tuxrt_et_slave_t* slave,
                                                  uint8_t priority,
                                                  tuxrt_et_type_t type,
                                                  uint8_t flow_id,
                                                  const void* payload,
                                                  uint16_t len,
                                                  uint64_t tx_deadline_ns);

/** Envía varias tramas agregadas en modo esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_send_aggregated(tuxrt_et_slave_t* slave,
                                                          uint8_t priority,
                                                          uint8_t flow_id,
                                                          const tuxrt_et_flow_entry_t* entries,
                                                          uint32_t count);

/** Recibe una trama en modo esclavo. Devuelve los bytes de carga útil, o -1 en timeout/error. */
TUXRT_SDK_API int tuxrt_et_slave_recv(tuxrt_et_slave_t* slave,
                                       tuxrt_et_header_t* hdr,
                                       void* payload,
                                       uint32_t bufsize,
                                       uint32_t timeout_us);

/** Obtiene el modo actual del contexto (master o slave). */
TUXRT_SDK_API uint8_t tuxrt_et_slave_mode(const tuxrt_et_slave_t* slave);

/** Obtiene las estadísticas del modo esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_stats(const tuxrt_et_slave_t* slave,
                                                tuxrt_et_slave_stats_t* out);

/** Reinicia la máquina de estados ET-ESM al estado INIT. */
TUXRT_SDK_API void tuxrt_et_slave_esm_reset(tuxrt_et_slave_t* slave);

/** Obtiene el estado actual de la ESM del esclavo. */
TUXRT_SDK_API uint8_t tuxrt_et_slave_esm_state(const tuxrt_et_slave_t* slave);

/** Solicita una transición de estado en la ESM del esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_esm_request(tuxrt_et_slave_t* slave,
                                                      uint8_t next);

/** Inicia una transferencia en un canal lógico de mailbox (ET-MBX). */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_mbx_begin(tuxrt_et_slave_t* slave,
                                                      uint8_t channel,
                                                      uint32_t total_bytes,
                                                      bool firmware);

/** Obtiene el estado del canal de mailbox del esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_mbx_status(const tuxrt_et_slave_t* slave,
                                                      uint8_t channel,
                                                      uint8_t* out);

/** Habilita el esquema de ID local (Nivel 2) en el esclavo. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_enable_local_ids(tuxrt_et_slave_t* slave,
                                                          const tuxrt_et_aalid_config_t* cfg);

/** Establece el retraso de sincronización del enlace. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_sync_set_link_delay(tuxrt_et_slave_t* slave,
                                                              uint16_t node_id,
                                                              uint32_t delay_ns);

/** Procesa un informe de sincronización. */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_slave_sync_process_report(tuxrt_et_slave_t* slave,
                                                              uint16_t node_id,
                                                              int64_t measured_offset_ns,
                                                              uint64_t now_ns,
                                                              uint8_t* out_status);

/* ------------------------------------------------------------------ */
/* Función factory: crear contexto según modo                         */
/* ------------------------------------------------------------------ */

/** Crea un contexto EtherTux según el modo especificado.
 *  \param mode Modo: TUXRT_ET_MODE_MASTER o TUXRT_ET_MODE_SLAVE
 *  \param ifname Nombre de la interfaz de red
 *  \param out_ctx Puntero al puntero del contexto creado (opaco)
 *  \return TUXRT_OK si éxito, TUXRT_ERR_NOT_SUPPORTED si el modo no está disponible
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_et_create(tuxrt_et_mode_t mode,
                                           const char* ifname,
                                           void** out_ctx);

/* ------------------------------------------------------------------ */
/* --- ID local determinista en el esclavo ---------------------------- */
/* ------------------------------------------------------------------ */

/** MAC local (LAA) del ID del esclavo: `02:00:00:00:ID[15:8]:ID[7:0]`. */
TUXRT_SDK_API uint32_t tuxrt_et_slave_aalid_mac(uint16_t id, uint8_t out[6]);

/** ID codificado en una MAC local del esclavo; `TUXRT_ET_LOCAL_ID_NONE` si no lo es. */
TUXRT_SDK_API uint16_t tuxrt_et_slave_aalid_id_from_mac(const uint8_t mac[6]);

/* ------------------------------------------------------------------ */
/* WebSocket Cliente asíncrono no bloqueante                           */
/* ------------------------------------------------------------------ */

typedef struct tuxrt_ws_client tuxrt_ws_client_t;

/** Estados del cliente WebSocket (coincide con WS_STATE_* internos) */
typedef enum {
    TUXRT_WS_IDLE      = 0,   /* sin conexión                     */
    TUXRT_WS_CONNECTING = 1, /* handshake en curso               */
    TUXRT_WS_CONNECTED = 2,   /* conectado                        */
    TUXRT_WS_CLOSING   = 3,   /* cierre iniciado                  */
    TUXRT_WS_CLOSED    = 4    /* cerrado completamente            */
} tuxrt_ws_state_t;

/** Flags de frame WebSocket (mismos valores que CURLWS_*) */
typedef enum {
    TUXRT_WS_FLAG_TEXT     = (1 << 0),
    TUXRT_WS_FLAG_BINARY   = (1 << 1),
    TUXRT_WS_FLAG_CONT     = (1 << 2),
    TUXRT_WS_FLAG_CLOSE    = (1 << 3),
    TUXRT_WS_FLAG_PING     = (1 << 4),
    TUXRT_WS_FLAG_OFFSET   = (1 << 5),
    TUXRT_WS_FLAG_PONG     = (1 << 6)
} tuxrt_ws_flag_t;

/** Frame binario listo para Mojo FFI: puntero + longitud + flags. */
typedef struct {
    const uint8_t* data;   /* puntero a los datos del frame         */
    uint32_t       len;    /* longitud en bytes                     */
    uint32_t       flags;  /* tuxrt_ws_flag_t bitmask               */
    uint64_t       timestamp_ns; /* timestamp monotónico al recibir */
} tuxrt_ws_frame_t;

/**
 * Establece una conexión WebSocket asíncrona no bloqueante.
 *
 * La conexión se inicia pero NO bloquea el hilo llamante. El caller debe
 * invocar tuxrt_core_ws_service() repetidamente hasta que el estado sea
 * TUXRT_WS_CONNECTED o se produzca un error.
 *
 * \param url        URL WebSocket (ws:// o wss://)
 * \param timeout_ms Timeout para completar el handshake (ms)
 * \param out_client Puntero al puntero del cliente creado (opaco)
 * \return TUXRT_OK si éxito, o código de error negativo
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_connect(const char* url,
                                                   uint32_t timeout_ms,
                                                   tuxrt_ws_client_t** out_client);

/**
 * Cierra la conexión y libera todos los recursos del cliente.
 * NULL-safe: no hace nada si client es NULL.
 */
TUXRT_SDK_API void tuxrt_core_ws_disconnect(tuxrt_ws_client_t* client);

/**
 * Procesa eventos de la conexión (handshake, frames entrantes).
 * No bloqueante: retorna TUXRT_ERR_AGAIN si aún no hay datos disponibles.
 *
 * \param client      Cliente WebSocket (debe estar conectado)
 * \param timeout_ms  Timeout máximo de espera (ms)
 * \return TUXRT_OK si hay algo que procesar o conectado, TUXRT_ERR_AGAIN
 *         si necesita más tiempo, TUXRT_ERR_TIMEDOUT si expira el timeout
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_service(tuxrt_ws_client_t* client,
                                                  uint32_t timeout_ms);

/**
 * Envía datos binarios por el canal WebSocket.
 *
 * \param client Cliente WebSocket conectado
 * \param data   Puntero a los datos a enviar
 * \param len    Longitud en bytes
 * \return TUXRT_OK si éxito, TUXRT_ERR_IO si falla el envío
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_send(tuxrt_ws_client_t* client,
                                               const uint8_t* data,
                                               uint32_t len);

/**
 * Recibe un frame WebSocket de forma no bloqueante.
 *
 * \param client      Cliente WebSocket conectado
 * \param out_frame   Estructura donde se llena el frame recibido
 * \param timeout_ms  Timeout de espera (ms)
 * \return TUXRT_OK si recibió frame, TUXRT_ERR_TIMEDOUT, TUXRT_ERR_IO
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_recv(tuxrt_ws_client_t* client,
                                               tuxrt_ws_frame_t* out_frame,
                                               uint32_t timeout_ms);

/**
 * Consulta el estado actual del cliente.
 */
TUXRT_SDK_API tuxrt_ws_state_t tuxrt_core_ws_get_state(tuxrt_ws_client_t* client);

/**
 * Devuelve el último error ocurrido en el cliente (TUXRT_OK si no hubo errores).
 */
TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_last_error(tuxrt_ws_client_t* client);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_ETHERTUX_H */
