/*
 * et_slave_bridge.c — Bridging layer between public tuxrt_et_slave API
 *                     and internal et_slave implementation.
 *
 * Rule: never introduce dynamic memory allocation (malloc/free) in runtime paths.
 * All buffers are pre-allocated or statically sized.  Create/destroy are setup/
 * teardown paths (not hot paths), matching the pattern used in et_esm.c and
 * et_aalid.c.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <string.h>
#include <stdlib.h>

#include "../../sdk/include/tuxrt_ethertux.h"
#include "et_internal.h"

/* ------------------------------------------------------------------ */
/* Helper: convertir status_t a tuxrt_err_t                           */
/* ------------------------------------------------------------------ */

/** Convierte un status_t interno en un error tuxrt_err_t. */
tuxrt_err_t tuxrt_err_from_status(status_t st)
{
    switch (st) {
        case SUCCESS:
            return TUXRT_OK;
        case ERROR_INVALID:
            return TUXRT_ERR_INVAL;
        case ERROR_STATE:
            return TUXRT_ERR_STATE;
        case ERROR_IO:
            return TUXRT_ERR_IO;
        case ERROR_TIMEOUT:
            return TUXRT_ERR_TIMEDOUT;
        default:
            return TUXRT_ERR_IO;
    }
}

/* ------------------------------------------------------------------ */
/* Puente: tuxrt_et_slave_t contiene un puntero a et_slave_t opaco    */
/* ------------------------------------------------------------------ */

/** Contexto de esclavo EtherTux (opaco, contiene la implementación interna). */
struct tuxrt_et_slave {
    et_slave_t*       internal;  /* puntero a implementación interna (propiedad del bridge) */
    bool              initialized;  /* true después de et_slave_create */
};

/* ------------------------------------------------------------------ */
/* Implementación de la API pública                                     */
/* ------------------------------------------------------------------ */

tuxrt_err_t tuxrt_et_slave_create(const char* ifname,
                                   uint64_t link_bps,
                                   uint32_t cycle_ns,
                                   tuxrt_et_slave_t** out_ctx)
{
    if (out_ctx == NULL) {
        return TUXRT_ERR_INVAL;
    }

    *out_ctx = NULL;

    /* Validación de parámetros */
    if (ifname == NULL || ifname[0] == '\0') {
        return TUXRT_ERR_INVAL;
    }

    /* Allocate the opaque context */
    tuxrt_et_slave_t* slave = (tuxrt_et_slave_t*)calloc(1, sizeof(tuxrt_et_slave_t));
    if (slave == NULL) {
        return TUXRT_ERR_NOMEM;
    }

    /* Inicializar la estructura interna */
    slave->initialized = false;

    /* Llama a la creación interna: et_slave_create() retorna un puntero heap */
    et_slave_t* internal = et_slave_create(ifname, link_bps, cycle_ns);
    if (internal == NULL) {
        free(slave);
        return TUXRT_ERR_NOMEM;
    }

    /* Almacenamos el puntero: el bridge es ahora dueño de la memoria */
    slave->internal = internal;
    slave->initialized = true;

    *out_ctx = slave;
    return TUXRT_OK;
}

void tuxrt_et_slave_destroy(tuxrt_et_slave_t* slave)
{
    if (slave == NULL) {
        return;
    }

    if (slave->initialized) {
        et_slave_destroy(slave->internal);
    }

    free(slave);
}

tuxrt_err_t tuxrt_et_slave_open(tuxrt_et_slave_t* slave, const char* ifname)
{
    if (slave == NULL || !slave->initialized || ifname == NULL || ifname[0] == '\0') {
        return TUXRT_ERR_INVAL;
    }

    status_t st = et_slave_open(slave->internal, ifname);
    if (st != SUCCESS) {
        return tuxrt_err_from_status(st);
    }

    return TUXRT_OK;
}

void tuxrt_et_slave_close(tuxrt_et_slave_t* slave)
{
    if (slave == NULL) {
        return;
    }

    if (slave->initialized) {
        et_slave_close(slave->internal);
    }
}

tuxrt_err_t tuxrt_et_slave_send(tuxrt_et_slave_t* slave,
                                 uint8_t priority,
                                 tuxrt_et_type_t type,
                                 uint8_t flow_id,
                                 const void* payload,
                                 uint16_t len)
{
    if (slave == NULL || !slave->initialized) {
        return TUXRT_ERR_INVAL;
    }

    status_t st = et_slave_send(slave->internal, priority, type, flow_id, payload, len);
    if (st != SUCCESS) {
        return tuxrt_err_from_status(st);
    }

    return TUXRT_OK;
}

tuxrt_err_t tuxrt_et_slave_send_to(tuxrt_et_slave_t* slave,
                                    uint16_t dest_id,
                                    uint8_t priority,
                                    tuxrt_et_type_t type,
                                    uint8_t flow_id,
                                    const void* payload,
                                    uint16_t len)
{
    (void)dest_id; /* El esclavo solo envía difusión */

    if (slave == NULL || !slave->initialized) {
        return TUXRT_ERR_INVAL;
    }

    status_t st = et_slave_send(slave->internal, priority, type, flow_id, payload, len);
    if (st != SUCCESS) {
        return tuxrt_err_from_status(st);
    }

    return TUXRT_OK;
}

tuxrt_err_t tuxrt_et_slave_send_at(tuxrt_et_slave_t* slave,
                                    uint8_t priority,
                                    tuxrt_et_type_t type,
                                    uint8_t flow_id,
                                    const void* payload,
                                    uint16_t len,
                                    uint64_t tx_deadline_ns)
{
    if (slave == NULL || !slave->initialized) {
        return TUXRT_ERR_INVAL;
    }

    status_t st = et_slave_send_at(slave->internal, priority, type, flow_id, payload, len, tx_deadline_ns);
    if (st != SUCCESS) {
        return tuxrt_err_from_status(st);
    }

    return TUXRT_OK;
}

int tuxrt_et_slave_recv(tuxrt_et_slave_t* slave,
                         tuxrt_et_header_t* hdr,
                         void* payload,
                         uint32_t bufsize,
                         uint32_t timeout_us)
{
    if (slave == NULL || !slave->initialized || hdr == NULL) {
        return -1;
    }

    int ret = et_slave_recv(slave->internal, (et_header_t*)hdr, payload, bufsize, timeout_us);
    if (ret < 0) {
        return -1;
    }

    return ret;
}

uint8_t tuxrt_et_slave_mode(const tuxrt_et_slave_t* slave)
{
    if (slave == NULL || !slave->initialized) {
        return 0;
    }

    return et_slave_mode(slave->internal);
}

tuxrt_err_t tuxrt_et_slave_stats(const tuxrt_et_slave_t* slave,
                                  tuxrt_et_slave_stats_t* out)
{
    if (slave == NULL || !slave->initialized || out == NULL) {
        return TUXRT_ERR_INVAL;
    }

    et_slave_stats_t stats;
    status_t st = et_slave_stats(slave->internal, &stats);
    if (st != SUCCESS) {
        return tuxrt_err_from_status(st);
    }

    out->frames_tx = stats.frames_tx;
    out->frames_rx = stats.frames_rx;
    out->bytes_tx = stats.bytes_tx;
    out->bytes_rx = stats.bytes_rx;
    out->dropped = stats.dropped;
    out->frame_errors = stats.frame_errors;

    return TUXRT_OK;
}

void tuxrt_et_slave_esm_reset(tuxrt_et_slave_t* slave)
{
    if (slave == NULL || !slave->initialized) {
        return;
    }

    et_esm_slave_transition(slave->internal, ET_STATE_INIT);
}

uint8_t tuxrt_et_slave_esm_state(const tuxrt_et_slave_t* slave)
{
    if (slave == NULL || !slave->initialized) {
        return TUXRT_ET_STATE_INIT;
    }

    return et_slave_esm_state(slave->internal);
}

tuxrt_err_t tuxrt_et_slave_esm_request(tuxrt_et_slave_t* slave, uint8_t next)
{
    if (slave == NULL || !slave->initialized) {
        return TUXRT_ERR_INVAL;
    }

    status_t st = et_esm_slave_transition(slave->internal, (et_state_t)next);
    if (st != SUCCESS) {
        return tuxrt_err_from_status(st);
    }

    return TUXRT_OK;
}

tuxrt_err_t tuxrt_et_slave_mbx_begin(tuxrt_et_slave_t* slave, uint8_t channel,
                                      uint32_t total_bytes, bool firmware)
{
    (void)firmware;
    if (slave == NULL || !slave->initialized) {
        return TUXRT_ERR_INVAL;
    }

    /* No hay implementación mailbox en el nivel base; delegar al maestro */
    return TUXRT_ERR_NOT_SUPPORTED;
}

tuxrt_err_t tuxrt_et_slave_mbx_status(const tuxrt_et_slave_t* slave, uint8_t channel,
                                       uint8_t* out)
{
    (void)channel;
    if (slave == NULL || !slave->initialized || out == NULL) {
        return TUXRT_ERR_INVAL;
    }

    *out = TUXRT_ET_MBX_IDLE;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_et_slave_enable_local_ids(tuxrt_et_slave_t* slave,
                                             const tuxrt_et_aalid_config_t* cfg)
{
    if (slave == NULL || !slave->initialized || cfg == NULL) {
        return TUXRT_ERR_INVAL;
    }

    /* El esclavo usa el mismo esquema de ID local que el maestro.
       Delegar a la función de nivel superior. */
    (void)cfg;
    return TUXRT_ERR_NOT_SUPPORTED;
}

tuxrt_err_t tuxrt_et_slave_sync_set_link_delay(tuxrt_et_slave_t* slave,
                                               uint16_t node_id,
                                               uint32_t delay_ns)
{
    if (slave == NULL || !slave->initialized) {
        return TUXRT_ERR_INVAL;
    }

    /* El esclavo no gestiona delay directo; esto es función del maestro. */
    (void)node_id;
    (void)delay_ns;
    return TUXRT_ERR_NOT_SUPPORTED;
}

tuxrt_err_t tuxrt_et_slave_sync_process_report(tuxrt_et_slave_t* slave,
                                               uint16_t node_id,
                                               int64_t measured_offset_ns,
                                               uint64_t now_ns,
                                               uint8_t* out_status)
{
    if (slave == NULL || !slave->initialized || out_status == NULL) {
        return TUXRT_ERR_INVAL;
    }

    (void)node_id;
    (void)measured_offset_ns;
    (void)now_ns;

    /* El esclavo informa siempre OK en modo básico */
    *out_status = (uint8_t)TUXRT_ET_SYNC_OK;
    return TUXRT_OK;
}

/* ------------------------------------------------------------------ */
/* Funciones faltantes: implementaciones puente                        */
/* ------------------------------------------------------------------ */

tuxrt_err_t tuxrt_et_slave_send_aggregated(tuxrt_et_slave_t* slave,
                                            uint8_t priority,
                                            uint8_t flow_id,
                                            const tuxrt_et_flow_entry_t* entries,
                                            uint32_t count)
{
    if (slave == NULL || !slave->initialized || entries == NULL || count == 0) {
        return TUXRT_ERR_INVAL;
    }

    /* Construir la carga útil agregada usando Flow Aggregator */
    uint8_t agg_buf[TUXRT_ET_MAX_FRAME];
    uint32_t agg_len = 0;

    tuxrt_err_t ret = tuxrt_et_flowagg_build(entries, count, agg_buf, sizeof(agg_buf), &agg_len);
    if (ret != TUXRT_OK) {
        return ret;
    }

    /* Enviar la trama agregada usando la priority y flow_id del esclavo */
    return tuxrt_et_slave_send(slave, priority, TUXRT_ET_TYPE_DATA, flow_id,
                               agg_buf, agg_len);
}

tuxrt_err_t tuxrt_et_create(tuxrt_et_mode_t mode,
                            const char* ifname,
                            void** out_ctx)
{
    if (out_ctx == NULL || ifname == NULL || ifname[0] == '\0') {
        return TUXRT_ERR_INVAL;
    }

    *out_ctx = NULL;

    switch (mode) {
        case TUXRT_ET_MODE_MASTER:
        {
            /* Crear contexto master usando la API pública */
            tuxrt_et_master_t* master = (tuxrt_et_master_t*)calloc(1, sizeof(tuxrt_et_master_t));
            if (master == NULL) {
                return TUXRT_ERR_NOMEM;
            }
            /* TODO: master creation would need more setup; for now return not supported */
            free(master);
            return TUXRT_ERR_NOT_SUPPORTED;
        }

        case TUXRT_ET_MODE_SLAVE:
        {
            tuxrt_et_slave_t** slave_ctx = (tuxrt_et_slave_t**)out_ctx;
            return tuxrt_et_slave_create(ifname, 0, 0, slave_ctx);
        }

        default:
            return TUXRT_ERR_INVAL;
    }
}

/* ------------------------------------------------------------------ */
/* ID local determinista en el esclavo                                 */
/* ------------------------------------------------------------------ */

uint32_t tuxrt_et_slave_aalid_mac(uint16_t id, uint8_t out[6])
{
    /* Delegar a la implementación de nivel superior */
    return tuxrt_et_aalid_mac(id, out);
}

uint16_t tuxrt_et_slave_aalid_id_from_mac(const uint8_t mac[6])
{
    /* Delegar a la implementación de nivel superior */
    return tuxrt_et_aalid_id_from_mac(mac);
}

/* ------------------------------------------------------------------ */
/* FIM                                                              */
/* ------------------------------------------------------------------ */