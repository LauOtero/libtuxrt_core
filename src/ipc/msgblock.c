/**
 * msgblock.c — Framing binario determinista V2 con fragmentación
 *
 * Implementa el serializado/parseo de la trama V2 y el reensamblador de
 * fragmentos. El CRC-32C (hardware-accelerated vía `tuxrt_ultracrc32`) cubre
 * desde `Version` hasta el final del payload.
 *
 * Design: TRT-24 - msgblock V2 framing (P0) / F1b-03
 */

#include "msgblock.h"

#include "tuxrt_sdk.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Bytes en el cable del magic (0x54 'T', 0x52 'R'). */
#define MSG_MAGIC_B0 0x54u
#define MSG_MAGIC_B1 0x52u

/* --- Framing -------------------------------------------------------------- */

size_t tuxrt_msgblock_frame(const tuxrt_msgblock_t* msg, uint8_t* out, size_t out_max)
{
    if (msg == NULL || out == NULL) {
        errno = EINVAL;
        return 0u;
    }
    if (msg->len > TUXRT_MSGBLOCK_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return 0u;
    }
    if (msg->len > 0u && msg->payload == NULL) {
        errno = EINVAL;
        return 0u;
    }

    size_t total = (size_t)TUXRT_MSGBLOCK_HEADER_SZ + (size_t)msg->len +
                   (size_t)TUXRT_MSGBLOCK_CRC_SZ;
    if (total > out_max) {
        errno = ENOSPC;
        return 0u;
    }

    out[0] = MSG_MAGIC_B0;
    out[1] = MSG_MAGIC_B1;
    out[2] = (uint8_t)TUXRT_MSGBLOCK_VERSION;
    out[3] = msg->type;
    out[4] = (uint8_t)(msg->seq & 0xFFu);
    out[5] = (uint8_t)(msg->seq >> 8);
    out[6] = msg->flags;
    out[7] = (uint8_t)(msg->len & 0xFFu);
    out[8] = (uint8_t)(msg->len >> 8);
    if (msg->len > 0u) {
        memcpy(out + TUXRT_MSGBLOCK_HEADER_SZ, msg->payload, msg->len);
    }

    uint32_t crc = tuxrt_ultracrc32(out + 2, 7u + (size_t)msg->len);
    size_t c = (size_t)TUXRT_MSGBLOCK_HEADER_SZ + (size_t)msg->len;
    out[c + 0u] = (uint8_t)(crc & 0xFFu);
    out[c + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    out[c + 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    out[c + 3u] = (uint8_t)((crc >> 24) & 0xFFu);
    return total;
}

int tuxrt_msgblock_parse(const uint8_t* in, size_t in_len,
                         tuxrt_msgblock_t* out, size_t* consumed)
{
    if (in == NULL || out == NULL || consumed == NULL) {
        errno = EINVAL;
        return TUXRT_MSGBLOCK_BAD;
    }
    *consumed = 0u;

    if (in_len < 2u) {
        return TUXRT_MSGBLOCK_NEED_MORE;
    }

    if (!(in[0] == MSG_MAGIC_B0 && in[1] == MSG_MAGIC_B1)) {
        /* Resincronización: buscar el siguiente magic. */
        for (size_t i = 1u; i + 1u < in_len; i++) {
            if (in[i] == MSG_MAGIC_B0 && in[i + 1u] == MSG_MAGIC_B1) {
                *consumed = i;
                return TUXRT_MSGBLOCK_RESYNC;
            }
        }
        /* Sin magic: descartar todo salvo un posible 0x54 final. */
        *consumed = (in[in_len - 1u] == MSG_MAGIC_B0) ? (in_len - 1u) : in_len;
        return TUXRT_MSGBLOCK_RESYNC;
    }

    if (in_len < (size_t)TUXRT_MSGBLOCK_HEADER_SZ) {
        return TUXRT_MSGBLOCK_NEED_MORE;
    }

    if (in[2] != (uint8_t)TUXRT_MSGBLOCK_VERSION) {
        *consumed = 1u;
        return TUXRT_MSGBLOCK_BAD;
    }

    uint16_t len = (uint16_t)((uint16_t)in[7] | ((uint16_t)in[8] << 8));
    if (len > (uint16_t)TUXRT_MSGBLOCK_MAX_PAYLOAD) {
        *consumed = 1u;
        return TUXRT_MSGBLOCK_BAD;
    }

    size_t total = (size_t)TUXRT_MSGBLOCK_HEADER_SZ + (size_t)len +
                   (size_t)TUXRT_MSGBLOCK_CRC_SZ;
    if (in_len < total) {
        return TUXRT_MSGBLOCK_NEED_MORE;
    }

    uint32_t crc_calc = tuxrt_ultracrc32(in + 2, 7u + (size_t)len);
    size_t c = (size_t)TUXRT_MSGBLOCK_HEADER_SZ + (size_t)len;
    uint32_t crc_stored = (uint32_t)in[c + 0u] |
                          ((uint32_t)in[c + 1u] << 8) |
                          ((uint32_t)in[c + 2u] << 16) |
                          ((uint32_t)in[c + 3u] << 24);
    if (crc_calc != crc_stored) {
        *consumed = 1u;
        return TUXRT_MSGBLOCK_BAD;
    }

    out->type = in[3];
    out->seq = (uint16_t)((uint16_t)in[4] | ((uint16_t)in[5] << 8));
    out->flags = in[6];
    out->len = len;
    out->payload = in + TUXRT_MSGBLOCK_HEADER_SZ;
    *consumed = total;
    return TUXRT_MSGBLOCK_OK;
}

/* --- Reensamblador -------------------------------------------------------- */

struct tuxrt_reasm {
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    bool     active;
    uint16_t seq;
};

tuxrt_reasm_t* tuxrt_reasm_create(size_t max_msg_size)
{
    if (max_msg_size == 0u) {
        errno = EINVAL;
        return NULL;
    }
    tuxrt_reasm_t* r = calloc(1u, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->buf = malloc(max_msg_size);
    if (r->buf == NULL) {
        free(r);
        return NULL;
    }
    r->cap = max_msg_size;
    return r;
}

int tuxrt_reasm_feed(tuxrt_reasm_t* r, const tuxrt_msgblock_t* frag,
                     uint8_t** out_full, size_t* out_len)
{
    if (r == NULL || frag == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (frag->len > 0u && frag->payload == NULL) {
        errno = EINVAL;
        return -1;
    }

    bool more = (frag->flags & TUXRT_MSGBLOCK_FLAG_MORE_FRAGMENTS) != 0u;
    bool last = (frag->flags & TUXRT_MSGBLOCK_FLAG_LAST_FRAGMENT) != 0u;

    if (more && last) { /* banderas incoherentes */
        errno = EPROTO;
        r->active = false;
        r->len = 0u;
        return -1;
    }

    if (!r->active) {
        r->active = true;
        r->seq = frag->seq;
        r->len = 0u;
    } else if (frag->seq != r->seq) {
        errno = EPROTO;
        r->active = false;
        r->len = 0u;
        return -1;
    }

    if (r->len + (size_t)frag->len > r->cap) {
        errno = EMSGSIZE;
        r->active = false;
        r->len = 0u;
        return -1;
    }
    if (frag->len > 0u) {
        memcpy(r->buf + r->len, frag->payload, frag->len);
    }
    r->len += (size_t)frag->len;

    if (more) {
        return 1; /* aceptado, faltan fragmentos */
    }

    /* `last` o mensaje sin banderas: mensaje completo. */
    if (out_full != NULL) {
        *out_full = r->buf;
    }
    if (out_len != NULL) {
        *out_len = r->len;
    }
    r->active = false;
    return 0;
}

void tuxrt_reasm_destroy(tuxrt_reasm_t* r)
{
    if (r == NULL) {
        return;
    }
    free(r->buf);
    free(r);
}
