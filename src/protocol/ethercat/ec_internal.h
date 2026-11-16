/*
 * ec_internal.h — Definiciones internas del protocolo EtherCAT
 *
 * NO es una cabecera pública: solo la usan los módulos de
 * src/protocol/ethercat/.
 *
 * Organización de módulos (v0.1.0):
 *   ec_socket.c     — socket AF_PACKET (0x88A4), PACKET_TX_RING, SO_TXTIME
 *   ec_frame.c      — serialización/parseo de la cabecera y el datagrama
 *   ec_master.c     — ciclo de vida, envío/recepción, nodos y heartbeat
 *   ec_dc.c         — relojes distribuidos (Distributed Clocks)
 *   ec_mailbox.c    — buzón (SM0/SM1) y Coe (SDO)
 *   ec_slave.c      — configuración, SII, FMMU, máquina de estados AL
 */

#ifndef TUXRT_ETHERCAT_INTERNAL_H
#define TUXRT_ETHERCAT_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <net/if.h>

/* ------------------------------------------------------------------ */
/* Constantes internas                                                 */
/* ------------------------------------------------------------------ */

#define EC_ETH_HDR_LEN      14u   /* encabezado Ethernet (6 dst + 6 src + 2 ethertype) */
#define EC_ECAT_HDR_LEN     2u    /* encabezado de trama EtherCAT */
#define EC_DGR_HDR_LEN     10u    /* longitud del encabezado de datagrama */
#define EC_WKC_LEN          2u    /* word count en datos de proceso */
#define EC_MAX_SLAVES       64    /* número máximo de esclavos */
#define EC_MAX_FRAME       1518   /* tamaño máximo de trama Ethernet */
#define EC_TIMEOUT_RET    10000   /* timeout por defecto (us) */
#define EC_TIMEOUT_MBX    500000  /* timeout buzón (us = 500ms) */
#define EC_TIMEOUT_EEP    100000  /* timeout EEPROM (us = 100ms) */

/* Direcciones de registros ESC (offsets genéricos) */
#define EC_REG_STADR        0x0001  /* dirección de estación */
#define EC_REG_ALSTAT       0x0003  /* estatus de máquina de estados */
#define EC_REG_ALCTL        0x0004  /* control AL */
#define EC_REG_DLSTAT       0x0005  /* estatus de enlaces */
#define EC_REG_ESCSUP       0x0008  /* soporte EtherCAT */
#define EC_REG_TYPE         0x0009  /* tipo de ESC */
#define EC_REG_DLCTRL       0x0011  /* control DMA */
#define EC_REG_DCSTAT       0x0012  /* estatus DC */
#define EC_REG_DCSYSTIME    0x0015  /* tiempo de sistema DC (64-bit) */
#define EC_REG_DCTIME0      0x0016  /* tiempo de recepción puerto 0 (32-bit) */
#define EC_REG_DCTIME1      0x0017  /* tiempo de recepción puerto 1 (32-bit) */
#define EC_REG_DCSYSDELAY   0x0018  /* desfase de sincronización */
#define EC_REG_DCCYCLE0     0x0019  /* ciclo SYNC0 (32-bit, periodo-1) */
#define EC_REG_DCCYCLE1     0x001A  /* ciclo SYNC1 (32-bit, periodo-1) */
#define EC_REG_DCSYNC0      0x001B  /* tiempo de inicio SYNC0 (64-bit) */
#define EC_REG_DCSYNC1      0x001C  /* tiempo de inicio SYNC1 (64-bit) */
#define EC_REG_DCCUC        0x001D  /* control ciclo continuo */
#define EC_REG_EEPADR       0x0020  /* dirección EEPROM palabra */
#define EC_REG_EEPCTL       0x0021  /* control EEPROM */
#define EC_REG_EEPSTAT      0x0022  /* estatus EEPROM */
#define EC_REG_EEPDAT       0x0023  /* datos EEPROM */
#define EC_REG_SM0          0x0040  /* sync manager 0 */
#define EC_REG_SM1          0x0042  /* sync manager 1 */
#define EC_REG_SM2          0x0044  /* sync manager 2 */
#define EC_REG_SM3          0x0046  /* sync manager 3 */
#define EC_REG_FMMU0        0x0100  /* FMMU 0 */
#define EC_FMMU_LEN         16u     /* longitud de cada entrada FMMU */

/* Longitudes de sync manager */
#define EC_SM_LEN           8u      /* cada SM tiene 8 registros */

/* Flags de SM */
#define EC_SMFLAG_ENABLE    0x80u   /* habilitar SM */
#define EC_SMSTATUS_FULL    0x01u   /* buzón de salida lleno */
#define EC_SMSTATUS_EMPTY   0x02u   /* buzón de entrada vacío */

/* Bytes de control AL */
#define EC_ALCTL_RESET      0x01u   /* reset */
#define EC_ALCTL_ACK        0x10u   /* acknowledge interrupt */

/* Estados de la máquina AL */
#define EC_STATE_NONE_VAL   0x00
#define EC_STATE_INIT       0x01
#define EC_STATE_PREOP      0x02
#define EC_STATE_SAFEOP     0x04
#define EC_STATE_OP         0x08
#define EC_STATE_BOOT       0x10
#define EC_STATE_ACK_VAL    0x20
#define EC_STATE_ERROR      0x40

/* Máscara de estado (bottom 4 bits) */
#define EC_STATE_MASK       0x0Fu

/* ------------------------------------------------------------------ */
/• Estructura de puerto                                               */
/* ------------------------------------------------------------------ */

/* Estado de una interfaz/puerto EtherCAT */
typedef struct {
    int fd;               /* fd del socket AF_PACKET */
    uint8_t mac[6];       /* dirección MAC del adaptador */
    int ifindex;          /* índice de interfaz */
    char ifname[16];      /* nombre de la interfaz (ej. "eth0") */
    bool redundant;       /* modo redundante (NIC primaria + secundaria) */
    /* Estadísticas */
    uint64_t frames_sent;
    uint64_t frames_received;
    uint64_t frames_lost;
    uint64_t bytes_sent;
    uint64_t bytes_received;
} ec_port_t;

/* ------------------------------------------------------------------ */
/• Estructura maestro completo                                         */
/* ------------------------------------------------------------------ */

/* Maestro EtherCAT completo con todos los subsistemas */
typedef struct {
    ec_port_t port;              /* configuración de puerto */
    uint8_t slaves[EC_MAX_SLAVES]; /* tabla de identificadores de esclavos (posición 1-based) */
    uint8_t iomap[EC_MAX_FRAME];   /* IOMap asignado (memoria de proceso) */
    uint32_t iomap_size;           /* tamaño del IOMap asignado */
    uint32_t expected_wkc;         /* WC (Word Count) esperado por ciclo */
    uint32_t tx_frame_len;         /* última longitud de trama transmitida */
    uint32_t rx_frame_len;         /* última longitud de trama recibida */
    bool is_open;                  /* ¿está abierta la interfaz? */
    bool is_configured;            /* ¿está configurado el maestro (scan+config_init+config_map)? */
    bool has_dc;                   /* ¿el bus tiene relojes distribuidos activos? */
    int64_t dc_time;               /* tiempo de sistema del bus (ns desde epoch 2000) */
    uint64_t dc_delay;             /* retraso de propagación por puerto (ns) */
    /* Estadísticas de monitoreo */
    uint64_t frames_sent_total;
    uint64_t frames_received_total;
    uint64_t frames_lost_total;
    uint64_t bytes_sent_total;
    uint64_t bytes_received_total;
    uint16_t process_errors;
    uint16_t frames_lost_counter;
} ec_master_t;

/* ------------------------------------------------------------------ */
/• Estructura de esclavos (interna)                                   */
/* ------------------------------------------------------------------ */

/* Información pública de un esclavo (accesible desde fuera del maestro) */
typedef struct {
    uint16_t position;   /* posición en la cadena (1-based) */
    uint16_t station_addr; /* dirección de estación asignada por el maestro */
    uint16_t alias;      /* alias de la esclava */
    uint8_t  dl_status;  /* estatus de enlaces DL */
    uint8_t  ports;      /* número de puertos físicos */
    bool     has_dc;     /* ¿esta esclava tiene reloj distribuido? */
    uint32_t config_addr; /* dirección de configuración (config_addr = station_addr) */
    uint16_t vendor_id;  /* ID de vendor del SII */
    uint32_t product_code; /* código de producto del SII */
    uint32_t revision;   /* revisión del firmware */
    uint32_t serial;     /* número de serie */
    uint16_t mbx_wr;     /* dirección de escritura del buzón (SM0) */
    uint16_t mbx_rr;     /* tamaño/recepción del buzón (SM1) */
    uint16_t mbx_wl;     /* longitud máxima de escritura */
    uint16_t mbx_rl;     /* longitud máxima de lectura */
    uint16_t o_bytes;    /* bytes de salida (PDO) */
    uint16_t o_offset;   /* offset de salida en el IOMap */
    uint16_t i_bytes;    /* bytes de entrada (PDO) */
    uint16_t i_offset;   /* offset de entrada en el IOMap */
} ec_slave_info_t;

/* Información interna extendida del esclavo */
typedef struct {
    ec_slave_info_t pub;       /* datos públicos */
    uint16_t cfg_sm_start[4];  /* inicio de cada SM */
    uint16_t cfg_sm_len[4];    /* longitud de cada SM */
    uint32_t cfg_sm_flags[4];  /* flags de cada SM */
    uint8_t  cfg_sm_type[4];   /* tipo de cada SM (mbxout, mbxin, output, input) */
} ec_slave_internal_t;

/* ------------------------------------------------------------------ */
/• Comandos y utilidades                                              */
/* ------------------------------------------------------------------ */

/* Leer registro ESC */
int ec_master_read_reg(ec_master_t* master, uint16_t adp, uint16_t ado, void* out, uint16_t len);

/* Escribir registro ESC */
int ec_master_write_reg(ec_master_t* master, uint16_t adp, uint16_t ado, const void* in, uint16_t len);

/* Función auxiliar little-endian 16 */
uint16_t ec_le16(const uint8_t* p);

/* Función auxiliar little-endian 32 */
uint32_t ec_le32(const uint8_t* p);

/* Función auxiliar big-endian 16 */
void ec_put16(uint8_t* p, uint16_t v);

/* ------------------------------------------------------------------ */
/* Estados de solicitud de máquinas                                   */
/* ------------------------------------------------------------------ */

/* Valores para ec_master_request_state y ec_master_request_all_states */
#define EC_REQ_STATE_INIT      0x01
#define EC_REQ_STATE_PREOP     0x02
#define EC_REQ_STATE_SAFEOP    0x04
#define EC_REQ_STATE_OP        0x08
#define EC_REQ_STATE_BOOT      0x10
#define EC_REQ_STATE_ERROR     0x20

#endif /* TUXRT_ETHERCAT_INTERNAL_H */