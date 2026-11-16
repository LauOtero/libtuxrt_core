/*
 * ethercat.h — API pública del maestro EtherCAT autónomo
 *
 * API estilo libtuxwarp para el protocolo EtherCAT (IEC 61158 / ETG.1000).
 * Funciona sobre AF_PACKET, EtherType 0x88A4, SIN dependencia de SOEM/IgH.
 * Prioriza: tiempo real, determinismo, resiliencia, ultra rendimiento.
 *
 * NOTAS DE IMPLEMENTACIÓN:
 * - Todos los búferes de trama se pre-asignan en la estructura ec_master_t
 * - No hay reservas de memoria ni esperas indefinidas en los caminos calientes
 * - La enumeración de esclavos usa AP (APRD) buscando posiciones consecutivas
 * - La configuración inicial asigna direcciones SM, FMMU y IOMap
 * - Los relojes distribuidos (DC) usan lecturas de registro ESC ESCDCO/ESCSUP
 * - El transporte usa sockets AF_PACKET con PACKET_TX_RING y SO_TXTIME
 */

#ifndef TUXRT_ETHERCAT_H
#define TUXRT_ETHERCAT_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Constantes                                                        */
/* ------------------------------------------------------------------ */

#define EC_ETHERTYPE          0x88A4  /* EtherType EtherCAT sobre Ethernet crudo */
#define EC_MAX_SLAVES         64      /* Número máximo de esclavos en la cadena */
#define EC_MAX_FRAME         1518     /* Tamaño máximo de trama Ethernet */
#define EC_MAX_DATAGRAM_DATA 256      /* Datos máximos en un datagrama de proceso */

/* Comandos EtherCAT (ETG.1000) */
#define EC_CMD_NOP            0x00
#define EC_CMD_APRD           0x01
#define EC_CMD_APWR           0x02
#define EC_CMD_APRW           0x03
#define EC_CMD_FPRD           0x04
#define EC_CMD_FPWR           0x05
#define EC_CMD_FPRW           0x06
#define EC_CMD_BRD            0x07
#define EC_CMD_BWR            0x08
#define EC_CMD_BRW            0x09
#define EC_CMD_LRD            0x0A
#define EC_CMD_LWR            0x0B
#define EC_CMD_LRW            0x0C
#define EC_CMD_ARMW           0x0D
#define EC_CMD_FRMW           0x0E

/* Estados de máquina de estados AL (ALState) */
#define EC_STATE_NONE         0x00
#define EC_STATE_INIT         0x01
#define EC_STATE_PREOP        0x02
#define EC_STATE_SAFEOP       0x04
#define EC_STATE_OP           0x08
#define EC_STATE_BOOT         0x10
#define EC_STATE_ACK          0x20
#define EC_STATE_ERROR        0x40

/* Máscaras de estado */
#define EC_STATE_MASK         0x0Fu
#define EC_STATE_ERROR_FLAG   0x40u

/* ------------------------------------------------------------------ */
/* Estructuras de interfaz y datagrama                                */
/* ------------------------------------------------------------------ */

/* Estado de una interfaz EtherCAT */
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
} ec_interface_t;

/* Encabezado de datagrama EtherCAT (10 bytes) */
typedef struct {
    uint16_t length;      /* longitud del campo de datos (big-endian) */
    uint16_t address;     /* dirección del destino (big-endian) */
    uint8_t  channel;     /* canal/prioridad */
    uint8_t  type;        /* tipo de trama (0x03 = buzón) */
    uint16_t crc16;       /* CRC-16 sobre el datagrama */
} ec_datagram_t;

/* Encabezado de trama EtherCAT (2 bytes) */
typedef struct {
    uint16_t length;      /* longitud total del campo de datos incluido WC */
} ec_frame_header_t;

//* Estado maestro EtherCAT */
typedef struct {
    ec_interface_t port;          /* configuración de puerto */
    uint8_t slaves[EC_MAX_SLAVES]; /* tabla de identificadores de esclavos */
    uint8_t iomap[EC_MAX_FRAME];   /* IOMap asignado */
    uint32_t iomap_size;           /* tamaño del IOMap */
    uint32_t expected_wkc;         /* WC esperado por ciclo */
    uint32_t tx_frame_len;         /* última longitud de trama transmitida */
    uint32_t rx_frame_len;         /* última longitud de trama recibida */
    bool is_open;                  /* ¿está abierta la interfaz? */
    bool is_configured;            /* ¿está configurado el maestro? */
    bool has_dc;                   /* ¿el bus tiene relojes distribuidos? */
    uint64_t dc_time;              /* tiempo de sistema del bus (ns) */
} ec_master_t;

/* ------------------------------------------------------------------ */
//* API del maestro                                                    */
/* ------------------------------------------------------------------ */

/* Crear nuevo contexto maestro */
ec_master_t* ec_master_create(void);

/* Liberar contexto maestro */
void ec_master_destroy(ec_master_t* master);

/* Abrir interfaz */
status_t ec_master_open(ec_master_t* master, const char* ifname);

/* Cerrar interfaz */
void ec_master_close(ec_master_t* master);

/* Verificar si está abierta */
bool ec_master_is_open(const ec_master_t* master);

/* Obtener nombre de interfaz */
const char* ec_master_interface(const ec_master_t* master);

/* Configurar tiempo de espera por defecto */
void ec_master_set_timeout(ec_master_t* master, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Escaneo y configuración                                             */
/* ------------------------------------------------------------------ */

/* Escanear el bus para detectar esclavos */
int ec_master_scan(ec_master_t* master);

/* Configuración inicial (direcciones, SII, buzón) */
int ec_master_config_init(ec_master_t* master);

/* Asignar IOMap y FMMU/SM */
int ec_master_config_map(ec_master_t* master);

/* ------------------------------------------------------------------ */
/• Acceso a registros ESC                                             */
/* ------------------------------------------------------------------ */

/* Leer registro */
int ec_master_read_reg(ec_master_t* master, uint16_t adp, uint16_t ado, void* out, uint16_t len);

/* Escribir registro */
int ec_master_write_reg(ec_master_t* master, uint16_t adp, uint16_t ado, const void* in, uint16_t len);

/* Leer/Scribir objeto diccionario (CoE/SDO) */
int ec_master_sdo_read(ec_master_t* master, uint32_t slave_index, uint16_t index, uint8_t subindex, void* out, uint32_t* size, uint32_t timeout_us);
int ec_master_sdo_write(ec_master_t* master, uint32_t slave_index, uint16_t index, uint8_t subindex, const void* in, uint32_t size, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Acceso a datos de proceso (PDO)                                    */
/* ------------------------------------------------------------------ */

/* Enviar datos de proceso */
int ec_master_send_processdata(ec_master_t* master);

/* Recibir datos de proceso */
int ec_master_receive_processdata(ec_master_t* master, uint32_t timeout_us);

/* Leer/escribir lógica de registros */
int ec_master_lrw(ec_master_t* master, uint32_t logical_addr, void* buf, uint16_t len);
int ec_master_lrd(ec_master_t* master, uint32_t logical_addr, void* out, uint16_t len);
int ec_master_lwr(ec_master_t* master, uint32_t logical_addr, const void* in, uint16_t len);
int ec_master_armw(ec_master_t* master, uint16_t adp, uint16_t ado, void* buf, uint16_t len);
int ec_master_frmw(ec_master_t* master, uint16_t adp, uint16_t ado, void* buf, uint16_t len);

/* ------------------------------------------------------------------ */
/* Relojes distribuidos (DC)                                          */
/* ------------------------------------------------------------------ */

/* Configurar relojes distribuidos */
int ec_master_config_dc(ec_master_t* master);

/* Sincronización SYNC0 */
int ec_master_sync0(ec_master_t* master, uint32_t index, bool act, uint32_t cycle_ns, int32_t shift_ns);

/* Sincronización SYNC0/1 */
int ec_master_sync01(ec_master_t* master, uint32_t index, bool act, uint32_t cycle0_ns, int32_t shift0_ns, uint32_t cycle1_ns, int32_t shift1_ns);

/* Tiempo de reloj maestro */
int64_t ec_master_dc_time(const ec_master_t* master);

/* ------------------------------------------------------------------ */
/• Buzón (mailbox) y CoE (SDO)                                        */
/* ------------------------------------------------------------------ */

/* Leer/SD0 a esclava */
int ec_mbx_send(ec_master_t* master, int slave, const uint8_t* mbx, uint16_t len);
int ec_mbx_receive(ec_master_t* master, int slave, uint8_t* mbx, uint16_t bufsize, uint32_t timeout_us);

/* CoE: lectura (upload) */
int ec_master_sdo_read(ec_master_t* master, uint32_t slave_index, uint16_t index, uint8_t subindex, void* out, uint32_t* size, uint32_t timeout_us);

/* CoE: escritura (download) */
int ec_master_sdo_write(ec_master_t* master, uint32_t slave_index, uint16_t index, uint8_t subindex, const void* in, uint32_t size, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Envío y recepción de tramas                                        */
/* ------------------------------------------------------------------ */

/* Enviar datagrama */
int ec_master_send(ec_master_t* master, const uint8_t* datagrams, uint32_t len);

/* Recibir datagrama */
int ec_master_recv(ec_master_t* master, uint8_t* buffer, uint32_t bufsize, uint32_t timeout_us);

/* Enviar proceso data */
int ec_master_send_processdata(ec_master_t* master);

/* Recibir proceso data */
int ec_master_receive_processdata(ec_master_t* master, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Estados de las esclavas                                            */
/* ------------------------------------------------------------------ */

/* Verificar estado actual */
int ec_master_request_state(ec_master_t* master, uint16_t adp, uint8_t state, uint32_t timeout_us);

/* Solicitar estado a todas las esclavas */
int ec_master_request_all_states(ec_master_t* master, uint8_t state, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Diagnóstico                                                        */
/* ------------------------------------------------------------------ */

/* Contador de trabajo actual */
uint16_t ec_master_working_counter(const ec_master_t* master);

/* WC esperado */
uint16_t ec_master_expected_wkc(const ec_master_t* master);

/* Frames enviados */
uint64_t ec_master_frames_sent(const ec_master_t* master);

/* Frames perdidos */
uint64_t ec_master_frames_lost(const ec_master_t* master);

/* ------------------------------------------------------------------ */

/* Estados del maestro (para ec_master_request_state/All) */
#define EC_MASTER_STATE_INIT      0x01
#define EC_MASTER_STATE_PREOP     0x02
#define EC_MASTER_STATE_SAFEOP    0x04
#define EC_MASTER_STATE_OP        0x08
#define EC_MASTER_STATE_BOOT      0x10
#define EC_MASTER_STATE_ERROR     0x20

#endif /* TUXRT_ETHERCAT_H */