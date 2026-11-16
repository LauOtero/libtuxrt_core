/**
 * channel.h — Canal SPSC wait-free (ring de bytes)
 *
 * Canal de un productor / un consumidor (Single-Producer Single-Consumer)
 * sin bloqueo y sin espera activa (wait-free) sobre un ring de bytes de
 * capacidad potencia de 2. Diseñado para el traspaso de datos entre el hilo
 * de tiempo real y el hilo de servicio.
 *
 * Modelo de memoria: el productor escribe los datos y publica con un store
 * `release` de `tail`; el consumidor observa `tail` con un load `acquire`.
 * Simétricamente para `head`. No requiere CAS ni spin (wait-free).
 *
 * Design: TRT-24 - Channel SPSC (P0) / F1b-01
 */

#ifndef TUXRT_CHANNEL_H
#define TUXRT_CHANNEL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    _Alignas(64) _Atomic uint32_t head; /* índice de lectura (consumidor) */
    _Alignas(64) _Atomic uint32_t tail; /* índice de escritura (productor) */
    uint32_t capacity;                  /* potencia de 2                    */
    uint32_t mask;                      /* capacity - 1                     */
    _Alignas(64) uint8_t buf[];         /* almacenamiento del ring          */
} tuxrt_channel_t;

/**
 * Crear un canal con capacidad >= `capacity` (se redondea a potencia de 2).
 * @return Canal, o NULL si falla la asignación o `capacity == 0`.
 */
tuxrt_channel_t* tuxrt_channel_create(uint32_t capacity);

/** Liberar un canal creado con tuxrt_channel_create(). */
void tuxrt_channel_destroy(tuxrt_channel_t* ch);

/**
 * Escribir hasta `len` bytes (no bloqueante).
 * @return Número de bytes realmente escritos (puede ser < len si el ring
 *         está lleno; 0 si está lleno o `len == 0`).
 */
size_t tuxrt_channel_write(tuxrt_channel_t* ch, const void* data, size_t len);

/**
 * Leer hasta `len` bytes (no bloqueante).
 * @return Número de bytes realmente leídos (0 si está vacío).
 */
size_t tuxrt_channel_read(tuxrt_channel_t* ch, void* out, size_t len);

/** Bytes disponibles para lectura. */
size_t tuxrt_channel_available_read(const tuxrt_channel_t* ch);

/** Bytes disponibles para escritura. */
size_t tuxrt_channel_available_write(const tuxrt_channel_t* ch);

/** Capacidad total del canal en bytes. */
uint32_t tuxrt_channel_capacity(const tuxrt_channel_t* ch);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_CHANNEL_H */
