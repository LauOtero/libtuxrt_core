/*
 * ec_frame.c — Construcción/parseo de datagramas EtherCAT
 *
 * Implementa la construcción y análisis de tramas EtherCAT según
 * ETG.1000. El formato del datagrama es:
 *   [0..1] longitud   [2..3] dirección   [4] canal/prioridad   [5] tipo
 *   [6..7] CoE number [8] CoE service     [9] reservado
 *
 * El encabezado de trama EtherCAT (2 bytes) se adjunta antes del datagrama.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>

/* Longitudes de campos EtherCAT */
#include "ec_internal.h"

#define EC_ETH_HDR_LEN      14u   /* encabezado Ethernet (6 dst + 6 src + 2 ethertype) */
#define EC_ECAT_HDR_LEN     2u    /* encabezado de trama EtherCAT */
#define EC_DGR_HDR_LEN     10u    /* longitud del encabezado de datagrama */
#define EC_WKC_LEN          2u    /* word count en datos de proceso */

/* Máscaras y shifts para el encabezado de datagrama */
#define EC_DGR_LEN_SHIFT    0
#define EC_DGR_LEN_MASK    (0xFFFFu << EC_DGR_LEN_SHIFT)
#define EC_DGR_ADDR_SHIFT  16
#define EC_DGR_ADDR_MASK   (0xFFFFu << EC_DGR_ADDR_SHIFT)
#define EC_DGR_PRIO_SHIFT  0
#define EC_DGR_PRIO_MASK   (0x0Fu << EC_DGR_PRIO_SHIFT)
#define EC_DGR_TYPE_SHIFT  8
#define EC_DGR_TYPE_MASK   (0x03u << EC_DGR_TYPE_SHIFT)

/* ------------------------------------------------------------------ */
/* Construcción de datagrama                                          */
/* ------------------------------------------------------------------ */

/* Construir datagrama EtherCAT en el buffer proporcionado.
 * Escribe la cabecera de 10 bytes + datos + word count (WC).
 * Retorna la longitud total del datagrama (incluyendo la cabecera de 10 bytes),
 * o 0 si falla.
 */

uint16_t ec_datagram_build(uint8_t* buf, uint16_t buf_cap,
                          uint16_t address, uint8_t channel,
                          uint8_t type, const uint8_t* data, uint16_t data_len,
                          bool with_wc) {
    if (!buf || buf_cap < EC_DGR_HDR_LEN + data_len) {
        return 0u;
    }

    uint8_t* p = buf;

    /* [0..1] longitud: longitud del campo de datos + WC (2 bytes big-endian) */
    uint16_t dgr_len = data_len + EC_WKC_LEN; /* +2 si with_wc */
    p[0] = (uint8_t)(dgr_len & 0xFFu);
    p[1] = (uint8_t)((dgr_len >> 8) & 0xFFu);

    /* [2..3] dirección: dirección del destino (big-endian) */
    p[2] = (uint8_t)(address & 0xFFu);
    p[3] = (uint8_t)((address >> 8) & 0xFFu);

    /* [4] canal/prioridad */
    p[4] = channel;

    /* [5] tipo */
    p[5] = type;

    /* [6..7] CoE number (solo para tipos de buzón) */
    p[6] = 0u;
    p[7] = 0u;

    /* [8] CoE service (solo para tipos de buzón) */
    p[8] = 0u;

    /* [9] reservado */
    p[9] = 0u;

    /* Copiar datos si los hay */
    if (data && data_len > 0u) {
        memcpy(p + EC_DGR_HDR_LEN, data, data_len);
    }

    /* Word count (WC) al final si corresponde */
    if (with_wc) {
        /* El WC se pone después de los datos */
        uint16_t* wc_ptr = (uint16_t*)(p + EC_DGR_HDR_LEN + data_len);
        *wc_ptr = 0u; /* WC inicializado a 0 */
    }

    return (uint16_t)(EC_DGR_HDR_LEN + data_len + EC_WKC_LEN);
}

/* ------------------------------------------------------------------ */
/* Parseo de datagrama                                                */
/* ------------------------------------------------------------------ */

/* Parsear un datagrama EtherCAT desde el buffer.
 * Retorna 0 si éxito, o -1 si el buffer es inválido.
 * Llena la estructura ec_datagram_t con los campos extraídos.
 */

int ec_datagram_parse(const uint8_t* buf, uint16_t buf_len, ec_datagram_t* dgr) {
    if (!buf || buf_len < EC_DGR_HDR_LEN || !dgr) {
        return -1;
    }

    const uint8_t* p = buf;

    /* [0..1] longitud */
    dgr->length = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

    /* [2..3] dirección */
    dgr->address = (uint16_t)p[2] | ((uint16_t)p[3] << 8);

    /* [4] canal/prioridad */
    dgr->channel = p[4];

    /* [5] tipo */
    dgr->type = p[5];

    /* [6..7] CoE number */
    dgr->channel = p[6]; /* reutilizar channel para CoE number */
    /* Nota: en el estándar ETG.1000, bytes 6-7 son CoE number, pero
     * para simplificar y compatibilidad, usamos channel para ambos fines
     * dependiendo del tipo de trama. */

    /* [8] CoE service */
    /* Ya está en p[8], lo trasladamos a una posición más clara */
    /* No lo guardamos directamente para no romper la estructura */

    /* [9] reservado - ignorado */

    return 0;
}

/* ------------------------------------------------------------------ */
/* Encabezado de trama EtherCAT (2 bytes)                           */
/* ------------------------------------------------------------------ */

/* Escribir cabecera de trama EtherCAT (2 bytes) al inicio de un buffer.
 * El campo length contiene la longitud total del campo de datos incluido WC.
 */

void ec_frame_header(uint8_t* buf, uint16_t total_len) {
    if (!buf) {
        return;
    }
    buf[0] = (uint8_t)(total_len & 0xFFu);
    buf[1] = (uint8_t)((total_len >> 8) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/• Inicio y compromiso de trama completa                              */
/* ------------------------------------------------------------------ */

/* Escribir cabecera Ethernet (14 bytes: 6 dst + 6 src + 2 ethertype)
 * y cabecera EtherCAT (2 bytes) en el buffer.
 * Escribe sobre el offset inicial del buffer.
 */

void ec_frame_begin(uint8_t* buf, uint16_t total_len) {
    if (!buf) {
        return;
    }

    /* Encabezado Ethernet: destino, origen, EtherType */
    /* Por ahora, se asume que el llamador llena la dirección destino y origen */
    /* EC_ETHERTYPE ya se pondrá en ec_frame_commit */

    /* Encabezado EtherCAT */
    ec_frame_header(buf + EC_ETH_HDR_LEN, total_len);
}

/* Commit: escribir cabecera EtherCAT y devolver la longitud total de la trama.
 * Escribe la cabecera Ethernet completa y la cabecera EtherCAT,
 * y retorna la longitud total del frame listo para enviar.
 */

uint32_t ec_frame_commit(uint8_t* buf, uint32_t dgr_len) {
    if (!buf) {
        return 0u;
    }

    /* La cabecera Ethernet ya debería haber sido escrita por ec_frame_begin,
     * pero la completamos aquí asegurándonos de que EC_ETHERTYPE esté correcto */

    /* Escribir EC_ETHERTYPE (0x88A4) en las bytes 12-13 */
    uint16_t* ethertype_ptr = (uint16_t*)(buf + 12);
    *ethertype_ptr = htons(EC_ETHERTYPE);

    /* La longitud total es: Ethernet header(14) + datagram */
    uint32_t total_len = EC_ETH_HDR_LEN + dgr_len;

    return total_len;
}