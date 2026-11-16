/**
 * msgblock.h — Framing binario determinista V2 con fragmentación
 *
 * Trama V2 (little-endian en todos los campos multibyte):
 *
 *   ┌────────┬─────────┬──────┬──────┬────────┬─────────┬─────────┬──────┐
 *   │ Magic  │ Version │ Type │ Seq  │ Flags  │ Length  │ Payload │ CRC  │
 *   │  2 B   │   1 B   │ 1 B  │ 2 B  │  1 B   │  2 B    │  0-N B  │ 4 B  │
 *   │ 0x5452 │   0x02  │  T   │  N   │   F    │    L    │         │ U32  │
 *   └────────┴─────────┴──────┴──────┴────────┴─────────┴─────────┴──────┘
 *
 * El CRC-32C cubre desde `Version` hasta el final del `Payload` (no el magic).
 *
 * Fragmentación: los payloads > `TUXRT_MSGBLOCK_MAX_PAYLOAD` se dividen en
 * fragmentos que comparten el mismo `Seq`; todos salvo el último llevan
 * `MORE_FRAGMENTS`, y el último lleva `LAST_FRAGMENT`. El reensamblador
 * (`tuxrt_reasm_t`) concatena en orden de llegada, valida la continuidad de
 * `Seq` y el límite de tamaño, y devuelve el payload completo.
 *
 * Design: TRT-24 - msgblock V2 framing (P0) / F1b-03
 */

#ifndef TUXRT_MSGBLOCK_H
#define TUXRT_MSGBLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Constantes de trama -------------------------------------------------- */
#define TUXRT_MSGBLOCK_MAGIC     0x5452u /* 'T','R' */
#define TUXRT_MSGBLOCK_VERSION   0x02u
#define TUXRT_MSGBLOCK_HEADER_SZ 9u  /* magic(2)+version(1)+type(1)+seq(2)+flags(1)+len(2) */
#define TUXRT_MSGBLOCK_CRC_SZ    4u
#define TUXRT_MSGBLOCK_MAX_PAYLOAD 4096u

/* --- Flags ---------------------------------------------------------------- */
#define TUXRT_MSGBLOCK_FLAG_MORE_FRAGMENTS 0x01u
#define TUXRT_MSGBLOCK_FLAG_LAST_FRAGMENT  0x02u
#define TUXRT_MSGBLOCK_FLAG_COMPRESSED     0x04u
#define TUXRT_MSGBLOCK_FLAG_HAS_TIMESTAMP  0x08u

/* --- Estado del parser ---------------------------------------------------- */
typedef enum {
    TUXRT_MSGBLOCK_BAD       = -1, /* version/CRC inválidos; *consumed = 1 (avanzar) */
    TUXRT_MSGBLOCK_NEED_MORE =  0, /* faltan bytes para una trama completa           */
    TUXRT_MSGBLOCK_OK        =  1, /* trama válida; *consumed = tamaño total         */
    TUXRT_MSGBLOCK_RESYNC    =  2, /* *consumed = bytes de basura a descartar        */
} tuxrt_msgblock_status_e;

/* --- Mensaje (vista sobre bytes ya existentes) ---------------------------- */
typedef struct {
    uint8_t        type;
    uint16_t       seq;
    uint8_t        flags;
    uint16_t       len;
    const uint8_t* payload; /* apunta a dentro del buffer de entrada */
} tuxrt_msgblock_t;

/* --- Framing -------------------------------------------------------------- */

/**
 * Serializar un mensaje en `out`.
 * @return Tamaño total de la trama escrita, o 0 si no cabe / args inválidos.
 */
size_t tuxrt_msgblock_frame(const tuxrt_msgblock_t* msg, uint8_t* out, size_t out_max);

/**
 * Parsear una trama al inicio de `in`.
 *
 * @param consumed  Bytes a descartar/avanzar según el estado devuelto.
 * @return `tuxrt_msgblock_status_e`. En `OK`, `out->payload` apunta a `in`.
 */
int tuxrt_msgblock_parse(const uint8_t* in, size_t in_len,
                         tuxrt_msgblock_t* out, size_t* consumed);

/* --- Reensamblador (uno por conexión) ------------------------------------- */

typedef struct tuxrt_reasm tuxrt_reasm_t;

/** Crear un reensamblador con capacidad máxima `max_msg_size` bytes. */
tuxrt_reasm_t* tuxrt_reasm_create(size_t max_msg_size);

/**
 * Alimentar con un fragmento.
 *
 * @param out_full  (solo si completa) buffer interno con el mensaje completo.
 * @param out_len   (solo si completa) longitud del mensaje completo.
 * @return  0 = mensaje completo; 1 = fragmento aceptado, faltan más; -1 = error
 *          (seq inconsistentes, banderas incoherentes, o desbordamiento).
 */
int tuxrt_reasm_feed(tuxrt_reasm_t* r, const tuxrt_msgblock_t* frag,
                     uint8_t** out_full, size_t* out_len);

/** Liberar un reensamblador. */
void tuxrt_reasm_destroy(tuxrt_reasm_t* r);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_MSGBLOCK_H */
