/**
 * channel.c — Canal SPSC wait-free (ring de bytes)
 *
 * Implementación de un ring de bytes de un productor / un consumidor
 * (Single-Producer Single-Consumer) sin bloqueo y sin espera activa.
 *
 * Diseño:
 *   - `head` es el índice de lectura, escrito únicamente por el consumidor.
 *   - `tail` es el índice de escritura, escrito únicamente por el productor.
 *   - Ambos son contadores libres (free-running) de 32 bits que envuelven de
 *     forma natural. La distancia `tail - head` da el número de bytes
 *     ocupados SIN desperdiciar una ranura (a diferencia del esquema
 *     capacity-1). El direccionamiento al buffer usa `índice & mask`.
 *   - El productor publica los datos con un store `release` de `tail` y lee
 *     el progreso del consumidor con un load `acquire` de `head`.
 *   - El consumidor publica con `release` de `head` y lee `tail` con
 *     `acquire`. Este emparejamiento release/acquire garantiza que las
 *     escrituras al buffer sean visibles antes que los índices, sin CAS ni
 *     spin: la operación es wait-free (termina en un número acotado de pasos).
 *
 * Design: TRT-24 - Channel SPSC (P0) / F1b-01
 */

#include "channel.h"

#include <stdlib.h>
#include <string.h>

/* Redondea a la potencia de 2 inmediatamente superior (>= 1). */
static uint32_t round_up_pow2(uint32_t v)
{
    if (v == 0u) {
        return 1u;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

tuxrt_channel_t* tuxrt_channel_create(uint32_t capacity)
{
    if (capacity == 0u) {
        return NULL;
    }

    /* Se limita la capacidad máxima a 2^30 para que los contadores libres de
     * 32 bits nunca puedan confundirse entre "lleno" y "vacío" (distancia
     * máxima < 2^31). */
    if (capacity > (1u << 30)) {
        capacity = (1u << 30);
    }

    uint32_t cap = round_up_pow2(capacity);

    tuxrt_channel_t* ch = malloc(sizeof(tuxrt_channel_t) + (size_t)cap);
    if (ch == NULL) {
        return NULL;
    }

    atomic_init(&ch->head, 0u);
    atomic_init(&ch->tail, 0u);
    ch->capacity = cap;
    ch->mask = cap - 1u;
    return ch;
}

void tuxrt_channel_destroy(tuxrt_channel_t* ch)
{
    free(ch);
}

size_t tuxrt_channel_available_read(const tuxrt_channel_t* ch)
{
    uint32_t tail = atomic_load_explicit(&ch->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&ch->head, memory_order_relaxed);
    return (size_t)(uint32_t)(tail - head);
}

size_t tuxrt_channel_available_write(const tuxrt_channel_t* ch)
{
    uint32_t tail = atomic_load_explicit(&ch->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&ch->head, memory_order_acquire);
    uint32_t used = tail - head;
    return (size_t)(ch->capacity - used);
}

uint32_t tuxrt_channel_capacity(const tuxrt_channel_t* ch)
{
    return ch->capacity;
}

size_t tuxrt_channel_write(tuxrt_channel_t* ch, const void* data, size_t len)
{
    if (len == 0u) {
        return 0u;
    }

    uint32_t head = atomic_load_explicit(&ch->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&ch->tail, memory_order_relaxed);

    uint32_t used = tail - head;
    uint32_t free_bytes = ch->capacity - used;

    size_t n = len;
    if (n > (size_t)free_bytes) {
        n = (size_t)free_bytes;
    }
    if (n == 0u) {
        return 0u;
    }

    const uint8_t* src = (const uint8_t*)data;
    uint32_t idx = tail & ch->mask;
    uint32_t first = ch->capacity - idx; /* bytes hasta el final del buffer */
    if (first > (uint32_t)n) {
        first = (uint32_t)n;
    }
    memcpy(&ch->buf[idx], src, first);
    if (n > (size_t)first) {
        memcpy(&ch->buf[0], src + first, n - (size_t)first);
    }

    atomic_store_explicit(&ch->tail, tail + (uint32_t)n, memory_order_release);
    return n;
}

size_t tuxrt_channel_read(tuxrt_channel_t* ch, void* out, size_t len)
{
    if (len == 0u) {
        return 0u;
    }

    uint32_t tail = atomic_load_explicit(&ch->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&ch->head, memory_order_relaxed);

    uint32_t avail = tail - head;

    size_t n = len;
    if (n > (size_t)avail) {
        n = (size_t)avail;
    }
    if (n == 0u) {
        return 0u;
    }

    uint8_t* dst = (uint8_t*)out;
    uint32_t idx = head & ch->mask;
    uint32_t first = ch->capacity - idx; /* bytes hasta el final del buffer */
    if (first > (uint32_t)n) {
        first = (uint32_t)n;
    }
    memcpy(dst, &ch->buf[idx], first);
    if (n > (size_t)first) {
        memcpy(dst + first, &ch->buf[0], n - (size_t)first);
    }

    atomic_store_explicit(&ch->head, head + (uint32_t)n, memory_order_release);
    return n;
}
