/**
 * zckb_shm.h — ZCKB-Shm: transporte zero-copy por memoria compartida
 *
 * Región de memoria compartida POSIX con un anillo SPSC embebido
 * (`tuxrt_channel_t`) para el traspaso zero-copy de mensajes entre el hilo de
 * tiempo real (productor) y el hilo de servicio (consumidor), potencialmente
 * en procesos distintos.
 *
 * Requisitos (SAD §15.3):
 *   - `shm_open` + `mmap(MAP_SHARED)`.
 *   - `MAP_HUGETLB | MAP_HUGE_2MB` si hay hugepages disponibles (con fallback
 *     transparente a páginas base).
 *   - `mlock()` obligatorio (requiere `RLIMIT_MEMLOCK` suficiente).
 *   - Limpieza de regiones stale al arrancar (magic/version/ready inválidos).
 *   - Buffers alineados a 64 B (cache-line).
 *
 * Modelo de acceso: `tuxrt_zckb_create()` es *open-or-create*. El primer
 * llamador inicializa el anillo; los siguientes con el mismo `name` se
 * *adjuntan* a la región existente (no reinician `head`/`tail`).
 *
 * Design: TRT-24 - ZCKB-Shm zero-copy transport (P0) / F1b-02
 */

#ifndef TUXRT_ZCKB_SHM_H
#define TUXRT_ZCKB_SHM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Identificador de la región ("ZCKB") y versión del layout. */
#define TUXRT_ZCKB_MAGIC   0x5A434B42u
#define TUXRT_ZCKB_VERSION 0x00010000u /* 1.0 */

/**
 * Cabecera de la región compartida. El anillo SPSC se coloca inmediatamente
 * después de la cabecera, alineado a 64 B en `TUXRT_ZCKB_RING_OFFSET`; el
 * buffer del anillo (`ring->buf`) sigue tras la estructura `tuxrt_channel_t`.
 *
 * Layout:  [ hdr (64 B) ][ tuxrt_channel_t (64-aligned) ][ buffer (cap B) ]
 */
#define TUXRT_ZCKB_RING_OFFSET 64u

typedef struct {
    _Alignas(64) uint32_t magic;  /* TUXRT_ZCKB_MAGIC                     */
    uint32_t version;             /* TUXRT_ZCKB_VERSION                   */
    uint32_t ring_bytes;          /* capacidad del anillo (potencia de 2) */
    uint32_t ready;               /* 1 cuando la inicialización terminó   */
} tuxrt_zckb_hdr_t;

_Static_assert(sizeof(tuxrt_zckb_hdr_t) <= TUXRT_ZCKB_RING_OFFSET,
               "la cabecera ZCKB debe caber en TUXRT_ZCKB_RING_OFFSET bytes");

/* Handle opaco. */
typedef struct tuxrt_zckb_s tuxrt_zckb_t;

/**
 * Abrir o crear una región ZCKB-Shm.
 *
 * @param name       Nombre POSIX (sin '/' inicial; se usa `/tuxrt.<name>`).
 * @param ring_bytes Capacidad del anillo en bytes (se redondea a potencia de 2).
 * @param use_huge   Intentar hugepages 2 MiB (fallback a páginas base).
 * @return Handle, o NULL con `errno` fijado (p.ej. fallo de `mlock`).
 */
tuxrt_zckb_t* tuxrt_zckb_create(const char* name, size_t ring_bytes, bool use_huge);

/** Cerrar el handle; el creador desvincula la región (`shm_unlink`). */
void tuxrt_zckb_destroy(tuxrt_zckb_t* z);

/**
 * Obtener el puntero al anillo compartido (`tuxrt_channel_t*`).
 *
 * @param out_size Recibe la capacidad útil del anillo en bytes (o NULL).
 * @return Puntero al anillo, o NULL si `z` es NULL.
 */
void* tuxrt_zckb_map(tuxrt_zckb_t* z, size_t* out_size);

/** Fijar la región en RAM (`mlock`). @return 0 o -1 con `errno`. */
int tuxrt_zckb_lock(tuxrt_zckb_t* z);

/** Liberar la fijación (`munlock`). @return 0 o -1 con `errno`. */
int tuxrt_zckb_unlock(tuxrt_zckb_t* z);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_ZCKB_SHM_H */
