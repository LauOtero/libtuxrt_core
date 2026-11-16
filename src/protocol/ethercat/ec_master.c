/*
 * ec_master.c — Núcleo del maestro EtherCAT: ciclo de vida, intercambio de
 * datagramas, acceso a registros, ciclo de proceso (PDO) y diagnóstico.
 *
 * Todas las operaciones usan búferes pre-asignados en el maestro y temporización
 * monotónica; no hay reservas de memoria ni esperas indefinidas en el camino
 * caliente.
 *
 * Este archivo implementa un maestro EtherCAT autónomo SIN dependencia de SOEM/IgH.
 * Usa sockets AF_PACKET, EtherType 0x88A4 y construcción/parseo manual de tramas.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "ec_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

ec_master_t* ec_master_create(void) {
    ec_master_t* m = (ec_master_t*)calloc(1, sizeof(ec_master_t));
    if (!m) {
        return NULL;
    }
    /* fd, iomap, etc. ya se ponen a 0 por calloc */
    return m;
}

void ec_master_destroy(ec_master_t* master) {
    if (!master) {
        return;
    }
    ec_master_close(master);
    free(master);
}

status_t ec_master_open(ec_master_t* master, const char* ifname) {
    if (!master || !ifname || ifname[0] == '\0') {
        return ERROR_INVALID;
    }

    if (master->is_open) {
        return ERROR_STATE;
    }

    /* Abrir socket y configurar interfaz */
    status_t st = ec_socket_open(&master->port, ifname);
    if (st != SUCCESS) {
        return st;
    }

    master->is_open = true;
    master->slave_count = 0;
    master->working_counter = 0;
    master->is_configured = false;
    master->process_pending = false;
    master->has_dc = false;
    master->dc_time = 0;
    master->dc_delay = 0;
    master->frames_sent_total = 0;
    master->frames_received_total = 0;
    master->frames_lost_total = 0;
    master->bytes_sent_total = 0;
    master->bytes_received_total = 0;
    master->process_errors = 0;
    master->frames_lost_counter = 0;

    /* Inicializar tabla de esclavos a 0 */
    for (int i = 0; i < EC_MAX_SLAVES; i++) {
        master->slaves[i] = 0;
    }

    return SUCCESS;
}

void ec_master_close(ec_master_t* master) {
    if (!master) {
        return;
    }

    ec_socket_close(&master->port);
    master->is_open = false;
    master->slave_count = 0;
    master->working_counter = 0;
    master->is_configured = false;
    master->process_pending = false;
    master->has_dc = false;
    master->dc_time = 0;
    master->dc_delay = 0;
}

bool ec_master_is_open(const ec_master_t* master) {
    return master && master->is_open;
}

const char* ec_master_interface(const ec_master_t* master) {
    return master ? master->port.ifname : NULL;
}

void ec_master_set_timeout(ec_master_t* master, uint32_t timeout_us) {
    if (master) {
        master->default_timeout_us = timeout_us;
    }
}

/* ------------------------------------------------------------------ */
/* Intercambio de UN datagrama                                         */
/* ------------------------------------------------------------------ */

/* Ejecutar un comando EtherCAT de un solo sentido */
int ec_call(ec_master_t* master, uint8_t cmd, uint32_t address,
           const void* wdata, uint16_t len, void* rdata, uint16_t* wc) {
    if (!master || !master->is_open || len > EC_MAX_DATAGRAM_DATA) {
        errno = EINVAL;
        return -1;
    }

    /* Construir la trama completa: Ethernet header(14) + ECAT header(2) + datagram */
    /* Usar buffer pre-asignado en el maestro (evita stack overflow de 1518 B) */
    uint8_t* frame = master->iomap;
    /* Índice donde escribiremos */
    uint8_t* frame_ptr = frame;

    /* Ethernet header: destino (broadcast), origen, EtherType */
    /* Para broadcast, destino es 0xFFFFFFFFFFFF */
    memset(frame_ptr, 0xFF, 6);     /* dirección destino broadcast */
    frame_ptr += 6;

    /* Origen: dirección MAC del adaptador */
    ec_socket_copy_mac(&master->port, frame_ptr);
    frame_ptr += 6;

    /* EtherType EtherCAT */
    uint16_t ethertype = htons(EC_ETHERTYPE);
    memcpy(frame_ptr, &ethertype, 2);
    frame_ptr += 2;

    /* EtherCAT header (2 bytes): longitud del campo de datos */
    uint16_t dgr_len;
    /* Escribir datagrama comenzando después del header EtherCAT */
    uint8_t* dgr_start = frame_ptr;

    /* Construir datagrama: length + address + channel + type */
    dgr_len = ec_datagram_build(frame_ptr, EC_MAX_FRAME - EC_ETH_HDR_LEN - EC_ECAT_HDR_LEN,
                              address, 0u, cmd, wdata, len, false);
    if (dgr_len == 0u) {
        return -1;
    }
    frame_ptr += dgr_len;

    /* Escribir encabezado EtherCAT de 2 bytes (longitud total) */
    ec_frame_header(frame_ptr, dgr_len);
    frame_ptr += EC_ECAT_HDR_LEN;

    /* Ahora frame tiene: Ethernet(14) + datagram(dgr_len) + ECAT header(2) */
    uint32_t total_len = (uint32_t)(frame_ptr - frame);

    /* Enviar */
    if (ec_socket_send(&master->port, frame, total_len) < 0) {
        master->frames_lost_total++;
        return -1;
    }
    master->frames_sent_total++;

    /* Recibir respuesta con timeout */
    uint32_t timeout = (master->default_timeout_us > 0u) ? master->default_timeout_us : EC_TIMEOUT_RET;
    uint8_t rx_frame[EC_MAX_FRAME];

    int n = ec_socket_recv(&master->port, rx_frame, sizeof(rx_frame), timeout);
    if (n < 0) {
        master->frames_lost_total++;
        return -1;
    }

    master->rx_frame_len = (uint32_t)n;

    /* Parsear respuesta: saltar Ethernet header(14) + ECAT header(2) */
    uint32_t off = EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN; /* = 16 */
    if ((uint32_t)n <= off) {
        return -1;
    }

    ec_datagram_t d;
    if (ec_datagram_parse(rx_frame + off, (uint32_t)n - off, &d) == -1) {
        return -1;
    }

    /* Verificar ID y comando */
    if (d.idx != 0u || d.cmd != cmd) {
        return -1;
    }

    /* Devolver datos si se solicitaron */
    if (rdata && d.data && d.data_len >= len && len > 0u) {
        memcpy(rdata, d.data, len);
    }

    if (wc) {
        *wc = d.working_counter;
    }

    master->working_counter = d.working_counter;
    return (d.working_counter > 0u) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Acceso a registros ESC                                              */
/* ------------------------------------------------------------------ */

int ec_master_read_reg(ec_master_t* master, uint16_t adp, uint16_t ado, void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)EC_CMD_APRD, address, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_write_reg(ec_master_t* master, uint16_t adp, uint16_t ado, const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)EC_CMD_APWR, address, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_fp_read(ec_master_t* master, uint16_t adp, uint16_t ado, void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)EC_CMD_FPRD, address, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_fp_write(ec_master_t* master, uint16_t adp, uint16_t ado, const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)EC_CMD_FPWR, address, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_broadcast_read(ec_master_t* master, uint16_t ado, void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    return (ec_call(master, (uint8_t)EC_CMD_BRD, (uint32_t)ado, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_broadcast_write(ec_master_t* master, uint16_t ado, const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    return (ec_call(master, (uint8_t)EC_CMD_BWR, (uint32_t)ado, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

/* ------------------------------------------------------------------ */
/• Direccionamiento lógico (datos de proceso)                          */
/* ------------------------------------------------------------------ */

int ec_master_lrw(ec_master_t* master, uint32_t logical_addr, void* buf, uint16_t len) {
    if (!master || !buf || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    return (ec_call(master, (uint8_t)EC_CMD_LRW, logical_addr, buf, len, buf, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_lrd(ec_master_t* master, uint32_t logical_addr, void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    return (ec_call(master, (uint8_t)EC_CMD_LRD, logical_addr, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_lwr(ec_master_t* master, uint32_t logical_addr, const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    return (ec_call(master, (uint8_t)EC_CMD_LWR, logical_addr, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_armw(ec_master_t* master, uint16_t adp, uint16_t ado, void* buf, uint16_t len) {
    if (!master || !buf || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)EC_CMD_ARMW, address, buf, len, buf, NULL) == 0)
               ? (int)len : -1;
}

int ec_master_frmw(ec_master_t* master, uint16_t adp, uint16_t ado, void* buf, uint16_t len) {
    if (!master || !buf || len == 0u) {
        errno = EINVAL;
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)EC_CMD_FRMW, address, buf, len, buf, NULL) == 0)
               ? (int)len : -1;
}

/* ------------------------------------------------------------------ */
/• Ciclo de datos de proceso (PDO)                                     */
/* ------------------------------------------------------------------ */

int ec_master_send_processdata(ec_master_t* master) {
    if (!master || !master->is_open || !master->is_configured ||
        !master->iomap || master->iomap_size == 0u) {
        errno = EINVAL;
        return -1;
    }
    if (master->iomap_size > EC_MAX_DATAGRAM_DATA) {
        errno = E2BIG;
        return -1;   /* bus mayor que un datagrama: fuera de alcance del ciclo simple */
    }

    uint8_t frame[EC_MAX_FRAME];
    uint32_t datagram_cap = (uint32_t)sizeof(frame) - EC_ETH_HDR_LEN - EC_ECAT_HDR_LEN;

    ec_frame_begin(master->port.mac, frame);
    uint32_t dl = ec_datagram_build(frame + EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN,
                                  datagram_cap, (uint16_t)0, 0u, (uint8_t)EC_CMD_LRW,
                                  master->iomap, (uint16_t)master->iomap_size, false);
    if (dl == 0u) {
        return -1;
    }
    master->tx_frame_len = ec_frame_commit(frame, dl);

    if (ec_socket_send(&master->port, frame, master->tx_frame_len) < 0) {
        master->frames_lost_total++;
        return -1;
    }
    master->frames_sent_total++;
    master->process_pending = true;
    return 0;
}

int ec_master_receive_processdata(ec_master_t* master, uint32_t timeout_us) {
    if (!master || !master->is_open || !master->process_pending) {
        errno = EINVAL;
        return -1;
    }

    int n = ec_socket_recv(&master->port, master->rx_frame,
                           (uint32_t)sizeof(master->rx_frame), timeout_us);
    master->process_pending = false;
    master->process_cycles++;
    if (n < 0) {
        master->process_errors++;
        return -1;
    }

    uint32_t off = EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN;
    if ((uint32_t)n <= off) {
        master->process_errors++;
        return -1;
    }

    ec_datagram_t d;
    if (ec_datagram_parse(master->rx_frame + off, (uint32_t)n - off, &d) == 0u ||
        d.cmd != (uint8_t)EC_CMD_LRW) {
        master->process_errors++;
        return -1;
    }

    if (d.data && d.data_len >= master->iomap_size) {
        memcpy(master->iomap, d.data, master->iomap_size);
    }

    master->working_counter = d.working_counter;
    if (d.working_counter < master->expected_wkc) {
        master->process_errors++;
        return -1;
    }
    return (int)d.working_counter;
}

/* ------------------------------------------------------------------ */
/• Diagnóstico y tramas crudas                                         */
/* ------------------------------------------------------------------ */

uint16_t ec_master_working_counter(const ec_master_t* master) {
    return master ? master->working_counter : 0u;
}

uint16_t ec_master_expected_wkc(const ec_master_t* master) {
    return master ? master->expected_wkc : 0u;
}

uint64_t ec_master_frames_sent(const ec_master_t* master) {
    return master ? master->frames_sent_total : 0u;
}

uint64_t ec_master_frames_lost(const ec_master_t* master) {
    return master ? master->frames_lost_total : 0u;
}

/* ------------------------------------------------------------------ */
/• Relojes distribuidos (DC)                                           */
/* ------------------------------------------------------------------ */

int ec_master_config_dc(ec_master_t* master) {
    if (!master || !master->is_open || master->slave_count <= 0) {
        errno = EINVAL;
        return ERROR_INVALID;
    }
    if (!master->has_dc) {
        errno = ENOTSUP;
        return ERROR_NOTSUP;
    }

    /* 1) Tiempo de sistema de la primera esclava con DC. */
    int ref = -1;

    for (int i = 1; i <= master->slave_count; i++) {
        /* slaves[] está 1-based, así que accedemos a i */
        if (i >= EC_MAX_SLAVES) {
            break;
        }
        if (!master->slaves[i].pub.has_dc) {
            continue;
        }

        /* Leer tiempo de sistema del registro ESC */
        uint8_t t[8];
        if (ec_master_read_reg(master, (uint16_t)i, EC_REG_DCSYSTIME, t, 8) != 8) {
            continue;
        }

        /* Combinar los 4 bytes bajar y 4 bytes alto (little-endian) */
        master->dc_time = (int64_t)((uint64_t)t[3] | ((uint64_t)t[2] << 8) |
                                   ((uint64_t)t[1] << 16) | ((uint64_t)t[0] << 24));
        /* Nota: el registro DCSYSTIME devuelve 64-bit little-endian */

        ref = i;
        break; /* usamos la primera esclava con DC como referencia */
    }
    if (ref < 0) {
        errno = ENODEV;
        return ERROR_NO_HARDWARE;
    }

    /* 2) Tiempos de recepción por puerto (retardos de propagación). */
    for (int i = 1; i <= master->slave_count; i++) {
        if (i >= EC_MAX_SLAVES) {
            break;
        }
        if (!master->slaves[i].pub.has_dc) {
            continue;
        }

        uint8_t rt[16];
        if (ec_master_read_reg(master, (uint16_t)i, EC_REG_DCTIME0, rt, 16) != 16) {
            continue;
        }

        /* Los registros DCTIME0-3 son 32-bit little-endian */
        master->slaves[i].cfg.dc_rtA = (int32_t)((uint32_t)rt[3] | ((uint32_t)rt[2] << 8) |
                                                  ((uint32_t)rt[1] << 16) | ((uint32_t)rt[0] << 24));
        master->slaves[i].cfg.dc_rtB = (int32_t)((uint32_t)rt[7] | ((uint32_t)rt[6] << 8) |
                                                  ((uint32_t)rt[5] << 16) | ((uint32_t)rt[4] << 24));
        master->slaves[i].cfg.dc_rtC = (int32_t)((uint32_t)rt[11] | ((uint32_t)rt[10] << 8) |
                                                  ((uint32_t)rt[9] << 16) | ((uint32_t)rt[8] << 24));
        master->slaves[i].cfg.dc_rtD = (int32_t)((uint32_t)rt[15] | ((uint32_t)rt[14] << 8) |
                                                  ((uint32_t)rt[13] << 16) | ((uint32_t)rt[12] << 24));

        /* Retardo de propagación (ns) respecto a la referencia: usamos el puerto
         * de recepción más temprano como cota conservadora. */
        /* Usamos dc_rtA como delay para simplificar */
        master->dc_delay = (uint64_t)master->slaves[i].cfg.dc_rtA;

        /* Escribir el desfase en el registro DCSYSDELAY de la esclava */
        uint8_t dly[4];
        dly[0] = (uint8_t)(master->dc_delay & 0xFFu);
        dly[1] = (uint8_t)((master->dc_delay >> 8) & 0xFFu);
        dly[2] = (uint8_t)((master->dc_delay >> 16) & 0xFFu);
        dly[3] = (uint8_t)((master->dc_delay >> 24) & 0xFFu);
        (void)ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYSDELAY, dly, 4);
    }

    /* 3) Difundir el tiempo de sistema del bus (escritura múltiple). */
    uint8_t systime[8];
    /* Pone el tiempo de sistema en el formato little-endian de 8 bytes */
    systime[0] = (uint8_t)(master->dc_time & 0xFFu);
    systime[1] = (uint8_t)((master->dc_time >> 8) & 0xFFu);
    systime[2] = (uint8_t)((master->dc_time >> 16) & 0xFFu);
    systime[3] = (uint8_t)((master->dc_time >> 24) & 0xFFu);
    systime[4] = (uint8_t)((master->dc_time >> 32) & 0xFFu);
    systime[5] = (uint8_t)((master->dc_time >> 40) & 0xFFu);
    systime[6] = (uint8_t)((master->dc_time >> 48) & 0xFFu);
    systime[7] = (uint8_t)((master->dc_time >> 56) & 0xFFu);

    (void)ec_master_broadcast_write(master, EC_REG_DCSYSTIME, systime, 8);

    return SUCCESS;
}

int ec_master_sync0(ec_master_t* master, uint32_t index, bool act, uint32_t cycle_ns, int32_t shift_ns) {
    if (!master || !master->is_open || index < 1u ||
        index > (uint32_t)master->slave_count) {
        errno = EINVAL;
        return -1;
    }
    int i = (int)index;

    /* Desactiva antes de reprogramar. */
    uint8_t off = 0x00u;
    (void)ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &off, 1);

    if (!act) {
        return 0;
    }

    /* Escribir ciclo y tiempo de inicio */
    /* Periodo: el registro almacena (periodo - 1). */
    uint8_t cyc[4];
    cyc[0] = (uint8_t)(cycle_ns & 0xFFu);
    cyc[1] = (uint8_t)((cycle_ns >> 8) & 0xFFu);
    cyc[2] = (uint8_t)((cycle_ns >> 16) & 0xFFu);
    cyc[3] = (uint8_t)((cycle_ns >> 24) & 0xFFu);
    (void)ec_master_write_reg(master, (uint16_t)i, EC_REG_DCCYCLE0, cyc, 4);

    /* Tiempo de arranque: sistema + un periodo, descontando el desfase. */
    int64_t start = master->dc_time + (int64_t)cycle_ns - (int64_t)shift_ns;
    uint8_t st[8];
    st[0] = (uint8_t)(start & 0xFFu);
    st[1] = (uint8_t)((start >> 8) & 0xFFu);
    st[2] = (uint8_t)((start >> 16) & 0xFFu);
    st[3] = (uint8_t)((start >> 24) & 0xFFu);
    st[4] = (uint8_t)((start >> 32) & 0xFFu);
    st[5] = (uint8_t)((start >> 40) & 0xFFu);
    st[6] = (uint8_t)((start >> 48) & 0xFFu);
    st[7] = (uint8_t)((start >> 56) & 0xFFu);
    (void)ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNC0, st, 8);

    /* Control de ciclo continuo (no de un solo disparo). */
    uint8_t cuc = 0x00u;
    (void)ec_master_write_reg(master, (uint16_t)i, EC_REG_DCCUC, &cuc, 1);

    uint8_t on = 0x01u;   /* SYNC0 */
    return (ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &on, 1) == 1)
               ? 0 : -1;
}

int ec_master_sync01(ec_master_t* master, uint32_t index, bool act, uint32_t cycle0_ns, int32_t shift0_ns,
                     uint32_t cycle1_ns, int32_t shift1_ns) {
    if (!master || !master->is_open || index < 1u ||
        index > (uint32_t)master->slave_count) {
        errno = EINVAL;
        return -1;
    }
    int i = (int)index;

    uint8_t off = 0x00u;
    (void)ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &off, 1);

    if (!act) {
        return 0;
    }

    if (ec_master_sync0(master, index, true, cycle0_ns, shift0_ns) < 0) {
        return -1;
    }
    if (ec_master_sync0(master, index, true, cycle1_ns, shift1_ns) < 0) {
        return -1;
    }

    uint8_t on = 0x03u;   /* SYNC0 | SYNC1 */
    return (ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &on, 1) == 1)
               ? 0 : -1;
}

int64_t ec_master_dc_time(const ec_master_t* master) {
    return master ? master->dc_time : 0;
}

/* ------------------------------------------------------------------ */
/• Buzón (mailbox) y CoE (SDO)                                         */
/* ------------------------------------------------------------------ */

int ec_mbx_send(ec_master_t* master, int slave, const uint8_t* mbx, uint16_t len) {
    if (!master || slave < 1 || slave > master->slave_count || !mbx ||
        len < EC_MBX_HDR_LEN || len > EC_MAX_MAILBOX) {
        errno = EINVAL;
        return -1;
    }

    /* Acceso 1-based a slaves[] */
    if (slave >= EC_MAX_SLAVES) {
        errno = EINVAL;
        return -1;
    }
    ec_slave_internal_t* s = &master->slaves[slave];
    if (s->cfg.mbx_wr == 0u) {
        errno = EINVAL;
        return -1;
    }

    /* Espera a que el buzón de salida quede libre (bit "full" del SM0). */
    uint16_t sm0_status = (uint16_t)(EC_REG_SM0 + EC_SM_STATUS_OFF);
    uint64_t deadline = ec_now_us() + EC_TIMEOUT_MBX;
    for (;;) {
        uint8_t st = 0;
        if (ec_master_read_reg(master, (uint16_t)slave, sm0_status, &st, 1) == 1 &&
            (st & EC_SM_STATUS_FULL) == 0u) {
            break;
        }
        if (ec_now_us() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
    }

    /* Escribir en la dirección del buzón de salida */
    int ret = ec_master_fp_write(master, s->pub.config_addr, s->cfg.mbx_wr, mbx, len);
    if (ret < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int ec_mbx_receive(ec_master_t* master, int slave,
                  uint8_t* mbx, uint16_t bufsize, uint32_t timeout_us) {
    if (!master || slave < 1 || slave > master->slave_count || !mbx ||
        bufsize < EC_MBX_HDR_LEN) {
        errno = EINVAL;
        return -1;
    }

    /* Acceso 1-based a slaves[] */
    if (slave >= EC_MAX_SLAVES) {
        errno = EINVAL;
        return -1;
    }
    ec_slave_internal_t* s = &master->slaves[slave];
    if (s->cfg.mbx_rr == 0u) {
        errno = EINVAL;
        return -1;
    }

    uint16_t sm1_status = (uint16_t)(EC_REG_SM0 + EC_SM_LEN + EC_SM_STATUS_OFF);
    uint16_t read_len = (s->cfg.mbx_rl != 0u) ? s->cfg.mbx_rl : EC_MAX_MAILBOX;
    if (read_len > bufsize) {
        read_len = bufsize;
    }

    uint64_t deadline = ec_now_us() + (uint64_t)timeout_us;
    for (;;) {
        uint8_t st = 0;
        if (ec_master_read_reg(master, (uint16_t)slave, sm1_status, &st, 1) == 1 &&
            (st & EC_SM_STATUS_FULL) != 0u) {
            int ret = ec_master_fp_read(master, s->pub.config_addr, s->cfg.mbx_rr, mbx, read_len);
            if (ret < 0) {
                errno = EIO;
                return -1;
            }
            /* El campo de longitud viene en los bytes 0-1 del mensaje de buzón */
            uint16_t total = (uint16_t)EC_MBX_HDR_LEN + ec_le16(mbx + EC_MBX_OFF_LEN);
            if (total > bufsize) {
                total = bufsize;
            }
            return (int)total;
        }
        if (ec_now_us() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* CoE: lectura (upload)                                               */
/* ------------------------------------------------------------------ */

/* Envía una petición CoE/SDO y devuelve la respuesta en `resp`. */
static int ec_sdo_transaction(ec_master_t* master, int slave,
                             uint8_t service, uint16_t index, uint8_t subindex,
                             uint8_t flags, const uint8_t* data, uint16_t data_len,
                             uint8_t* resp, uint16_t resp_size, uint32_t timeout_us) {
    if (!master || slave < 1 || slave > master->slave_count) {
        errno = EINVAL;
        return -1;
    }

    uint8_t req[EC_MAX_MAILBOX];
    memset(req, 0, sizeof(req));

    /* Disposición del buzón CoE (offsets en bytes del marco de buzón):
     *   [0..1] longitud
     *   [2..3] dirección
     *   [4] canal/prioridad
     *   [5] tipo (0x03 = CoE)
     *   [6..7] CoE number
     *   [8] CoE service
     *   [9] reservado
     *   [10..11] SDO index
     *   [12] subindex
     *   [13] flags / acceso completo
     *   [14..] datos SDO
     */

    uint16_t sdo_len = 3u + data_len;           /* index + subindex + flags + datos */
    uint16_t svc_len = 4u + sdo_len;            /* + cabecera CoE */

    /* Longitud total del búreq (incluyendo header de 6 bytes) */
    ec_put16(req + EC_MBX_OFF_LEN, svc_len);
    /* Dirección (bytes 2-3): pondremos 0, es un valor placeholder */
    ec_put16(req + EC_MBX_OFF_ADDR, 0u);
    /* Canal/prioridad (byte 4): derivar de la configuración de la esclava */
    req[EC_MBX_OFF_PRIO] = (uint8_t)((master->slaves[slave].cfg.mbx_cnt & 0x07u) << 4);
    /* Tipo: CoE */
    req[EC_MBX_OFF_TYPE] = 0x03u;
    /* CoE number (bytes 6-7): 0 para acceso estándar */
    req[EC_MBX_OFF_COE] = 0u;
    /* Servicio CoE */
    req[EC_MBX_OFF_SVC] = service;
    /* Índice SDO */
    ec_put16(req + EC_MBX_OFF_IDX, index);
    /* Subíndice */
    req[EC_MBX_OFF_SUB] = subindex;
    /* Flags */
    req[EC_MBX_OFF_FLG] = flags;

    /* Datos si los hay */
    if (data && data_len > 0u) {
        memcpy(req + EC_MBX_OFF_DATA, data, data_len);
    }

    /* Enviar petición */
    uint16_t frame_len = (uint16_t)(EC_MBX_HDR_LEN + svc_len);
    if (ec_mbx_send(master, slave, req, frame_len) < 0) {
        return -1;
    }

    /* Recibir respuesta */
    if (ec_mbx_receive(master, slave, resp, resp_size, timeout_us) < 0) {
        return -1;
    }

    /* Verificar tipo de respuesta */
    if ((resp[EC_MBX_OFF_TYPE] & 0x0Fu) != 0x03u) { /* EC_MBXTYPE_COE */
        errno = EPROTO;
        return -1;
    }

    return 0;
}

status_t ec_master_sdo_read(ec_master_t* master,
                            uint32_t slave_index,
                            uint16_t index, uint8_t subindex,
                            void* out, uint32_t* size,
                            uint32_t timeout_us) {
    if (!master || !out || !size || slave_index < 1u ||
        slave_index > (uint32_t)master->slave_count) {
        errno = EINVAL;
        return ERROR_INVALID;
    }
    int slave = (int)slave_index;
    uint8_t resp[EC_MAX_MAILBOX];
    uint32_t cap = *size;

    if (ec_sdo_transaction(master, slave, EC_SDO_UP_REQ, index, subindex, 0u,
                          NULL, 0u, resp, sizeof(resp), timeout_us) < 0) {
        return ERROR_IO;
    }

    uint8_t svc = resp[EC_MBX_OFF_SVC];
    if ((svc & 0xE0u) == EC_SDO_ABORT) {
        return ERROR_CORRUPT;
    }
    if ((svc & 0x0Fu) != EC_COE_SDORES) {
        return ERROR_IO;
    }

    uint8_t flg = resp[EC_MBX_OFF_FLG];
    uint32_t total;

    if ((flg & EC_SDO_FLAG_EXP) != 0u) {
        /* Transferencia expedita: los datos viven en el propio marco. */
        total = 4u - ((uint32_t)(flg & EC_SDO_FLAG_N) >> 2);
        if (total > cap) {
            total = cap;
        }
        memcpy(out, resp + EC_MBX_OFF_DATA, total);
        *size = total;
        return SUCCESS;
    }

    if ((flg & EC_SDO_FLAG_SIZE) != 0u) {
        /* Tamaño indicado en los 4 bytes de datos. */
        total = ec_le32(resp + EC_MBX_OFF_DATA);
    } else {
        total = 0u;
    }

    uint32_t copied = 0u;
    if (total <= 4u && total > 0u) {
        copied = (total > cap) ? cap : total;
        memcpy(out, resp + (EC_MBX_OFF_DATA + 4u), copied);
        *size = copied;
        return SUCCESS;
    }

    /* Transferencia segmentada. */
    uint8_t toggle = 0u;
    while (copied < total && copied < cap) {
        uint8_t sflg = (uint8_t)(EC_SDO_SEG_UP | (toggle ? EC_SDO_SEG_TOGGLE : 0u));
        if (ec_sdo_transaction(master, slave, EC_SDO_UP_SEG_REQ, index, subindex,
                               sflg, NULL, 0u, resp, sizeof(resp), timeout_us) < 0) {
            return ERROR_IO;
        }
        uint8_t rsvc = resp[EC_MBX_OFF_SVC];
        if ((rsvc & (EC_SDO_SEG_MORE | EC_SDO_SEG_TOGGLE)) == EC_SDO_SEG_MORE) {
            /* Fin de la secuencia. */
        }
        uint32_t chunk = 7u;
        if (chunk > (total - copied)) {
            chunk = total - copied;
        }
        if (chunk > (cap - copied)) {
            chunk = cap - copied;
        }
        memcpy((uint8_t*)out + copied, resp + EC_MBX_OFF_DATA, chunk);
        copied += chunk;
        toggle ^= 1u;
        if ((rsvc & EC_SDO_SEG_MORE) == 0u) {
            break;
        }
    }

    *size = copied;
    return (copied == total) ? SUCCESS : ERROR_IO;
}

status_t ec_master_sdo_write(ec_master_t* master,
                             uint32_t slave_index,
                             uint16_t index, uint8_t subindex,
                             const void* in, uint32_t size,
                             uint32_t timeout_us) {
    if (!master || !in || slave_index < 1u ||
        slave_index > (uint32_t)master->slave_count) {
        errno = EINVAL;
        return ERROR_INVALID;
    }
    int slave = (int)slave_index;
    const uint8_t* src = (const uint8_t*)in;
    uint8_t resp[EC_MAX_MAILBOX];

    /* Determinar tipo de escritura */
    uint8_t service;
    uint8_t flags;
    uint8_t first[4];
    uint32_t first_len;

    if (size <= 4u) {
        /* Descarga expedita. */
        service = EC_SDO_DOWN_EXP;
        flags = (uint8_t)(EC_SDO_FLAG_SIZE | EC_SDO_FLAG_EXP |
                         (uint8_t)(((4u - size) & 0x03u) << 2));
        memset(first, 0, sizeof(first));
        memcpy(first, src, size);
        first_len = 4u;
    } else {
        /* Descarga iniciada, con tamaño, seguida de segmentos. */
        service = EC_SDO_DOWN_INIT;
        flags = EC_SDO_FLAG_SIZE;
        ec_put32(first, size);
        first_len = 4u;
    }

    if (ec_sdo_transaction(master, slave, service, index, subindex, flags,
                          first, (uint16_t)first_len, resp, sizeof(resp), timeout_us) < 0) {
        return ERROR_IO;
    }
    if ((resp[EC_MBX_OFF_SVC] & 0xE0u) == EC_SDO_ABORT) {
        return ERROR_CORRUPT;
    }
    if (size <= 4u) {
        return SUCCESS;
    }

    /* Secuencia de segmentos. */
    uint8_t toggle = 0u;
    uint32_t sent = 0u;
    while (sent < size) {
        uint8_t seg[7];
        uint32_t chunk = size - sent;
        bool more = true;
        if (chunk > 7u) {
            chunk = 7u;
        } else {
            more = false;
        }
        memset(seg, 0, sizeof(seg));
        memcpy(seg, src + sent, chunk);
        uint8_t sflg = (uint8_t)((more ? EC_SDO_SEG_MORE : 0u) |
                                 (toggle ? EC_SDO_SEG_TOGGLE : 0u));
        if (ec_sdo_transaction(master, slave, EC_SDO_SEG_DOWN, index, subindex,
                               sflg, seg, 7u, resp, sizeof(resp), timeout_us) < 0) {
            return ERROR_IO;
        }
        sent += chunk;
        toggle ^= 1u;
        uint8_t rsvc = resp[EC_MBX_OFF_SVC];
        if ((rsvc & EC_SDO_SEG_MORE) == 0u) {
            break;
        }
    }
    return SUCCESS;
}