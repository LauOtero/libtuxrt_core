/**
 * zckb_shm.c — ZCKB-Shm: transporte zero-copy por memoria compartida
 *
 * Implementa `tuxrt_zckb_create()` como *open-or-create*: el primer llamador
 * crea y dimensiona la región (ftruncate + init del anillo embebido); los
 * siguientes con el mismo nombre se adjuntan sin reiniciar `head`/`tail`.
 * Un layout con magic/version permite además detectar y limpiar regiones
 * stale (magic inválido, tamaño insuficiente) reinicializándolas.
 *
 * Design: TRT-24 - ZCKB-Shm zero-copy transport (P0) / F1b-02
 */
#define _GNU_SOURCE

#include "zckb_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/mman.h> /* MAP_HUGE_2MB */
#endif

/* Hugepage 2 MiB. */
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB 0
#endif

#define TUXRT_ZCKB_HUGE_SIZE ((size_t)2u << 20) /* 2 MiB */
#define TUXRT_ZCKB_MAX_RING  (1u << 30)         /* 1 GiB */

struct tuxrt_zckb_s {
    char     name[128];
    int      fd;
    void*    base;
    size_t   map_size;  /* bytes efectivamente mapeados               */
    uint32_t capacity;  /* capacidad útil del anillo (bytes potencia 2) */
    bool     is_owner;  /* creador: debe shm_unlink en destroy        */
    bool     is_huge;   /* mapeado con MAP_HUGETLB                    */
    bool     locked;    /* mlock activo                               */
};

static uint32_t round_up_pow2_u32(uint32_t v)
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

/* Anillo embebido justo tras la cabecera (alineado a 64 B). */
static tuxrt_channel_t* zckb_ring(void* base)
{
    return (tuxrt_channel_t*)(void*)((char*)base + TUXRT_ZCKB_RING_OFFSET);
}

tuxrt_zckb_t* tuxrt_zckb_create(const char* name, size_t ring_bytes, bool use_huge)
{
    if (name == NULL || name[0] == '\0' || ring_bytes == 0u) {
        errno = EINVAL;
        return NULL;
    }
    if (strlen(name) > 96u) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    /* Capacidad del anillo: potencia de 2, acotada. */
    size_t rb = ring_bytes;
    if (rb > (size_t)TUXRT_ZCKB_MAX_RING) {
        rb = (size_t)TUXRT_ZCKB_MAX_RING;
    }
    uint32_t cap = round_up_pow2_u32((uint32_t)rb);

    size_t total = (size_t)TUXRT_ZCKB_RING_OFFSET + sizeof(tuxrt_channel_t) + (size_t)cap;

    char shm_name[128];
    (void)snprintf(shm_name, sizeof shm_name, "/tuxrt.%s", name);

    /* --- open-or-create -------------------------------------------------- */
    bool is_owner = false;
    int fd = shm_open(shm_name, O_RDWR, 0600);
    if (fd < 0) {
        if (errno != ENOENT) {
            return NULL;
        }
        fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) { /* carrera con otro creador */
                fd = shm_open(shm_name, O_RDWR, 0600);
            }
            if (fd < 0) {
                return NULL;
            }
        } else {
            is_owner = true;
        }
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }

    /* Tamaño objetivo de mapeo (hugepage alineado si se solicita). */
    bool want_huge = use_huge;
    size_t map_size = total;
    if (want_huge) {
        map_size = (total + TUXRT_ZCKB_HUGE_SIZE - 1u) & ~(TUXRT_ZCKB_HUGE_SIZE - 1u);
    }

    size_t cur = (size_t)st.st_size;
    bool need_init = true; /* se decide tras inspeccionar la cabecera */

    if ((size_t)st.st_size < map_size) {
        if (ftruncate(fd, (off_t)map_size) != 0) {
            int e = errno;
            close(fd);
            if (is_owner) {
                shm_unlink(shm_name);
            }
            errno = e;
            return NULL;
        }
        cur = map_size;
        need_init = true; /* región nueva o re-dimensionada */
    } else {
        need_init = false; /* puede ser una región existente válida */
    }

    /* --- mmap (con fallback transparente de hugepages) ------------------- */
    bool is_huge = false;
    int flags = MAP_SHARED;
    if (want_huge) {
        flags |= MAP_HUGETLB | MAP_HUGE_2MB;
    }

    void* base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (base == MAP_FAILED && want_huge) {
        /* Fallback a páginas base. */
        want_huge = false;
        map_size = total;
        if (cur < total) {
            if (ftruncate(fd, (off_t)total) != 0) {
                int e = errno;
                close(fd);
                if (is_owner) {
                    shm_unlink(shm_name);
                }
                errno = e;
                return NULL;
            }
        }
        base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if (base == MAP_FAILED) {
        int e = errno;
        close(fd);
        if (is_owner) {
            shm_unlink(shm_name);
        }
        errno = e;
        return NULL;
    }
    is_huge = want_huge;

    /* --- init o adjuntar ------------------------------------------------- */
    tuxrt_zckb_hdr_t* hdr = (tuxrt_zckb_hdr_t*)base;
    tuxrt_channel_t* ring = zckb_ring(base);

    if (!need_init) {
        /* Región existente: ¿layout válido? */
        atomic_thread_fence(memory_order_acquire);
        bool valid = (hdr->magic == TUXRT_ZCKB_MAGIC) &&
                     (hdr->version == TUXRT_ZCKB_VERSION) &&
                     (hdr->ready == 1u) &&
                     (hdr->ring_bytes != 0u);
        if (valid) {
            need_init = false; /* adjuntar: adoptar la configuración existente */
            cap = ring->capacity;
        } else {
            need_init = true; /* región stale -> reinicializar */
        }
    }

    if (need_init) {
        memset(hdr, 0, TUXRT_ZCKB_RING_OFFSET);
        hdr->version = TUXRT_ZCKB_VERSION;
        hdr->ring_bytes = cap;
        ring->capacity = cap;
        ring->mask = cap - 1u;
        atomic_store_explicit(&ring->head, 0u, memory_order_relaxed);
        atomic_store_explicit(&ring->tail, 0u, memory_order_relaxed);
        hdr->ready = 1u;
        atomic_thread_fence(memory_order_release);
        hdr->magic = TUXRT_ZCKB_MAGIC; /* magic al final: publica el layout */
        is_owner = true;
    }

    /* --- mlock obligatorio ---------------------------------------------- */
    if (mlock(base, map_size) != 0) {
        int e = errno;
        munmap(base, map_size);
        close(fd);
        if (is_owner) {
            shm_unlink(shm_name);
        }
        errno = e;
        return NULL;
    }

    tuxrt_zckb_t* z = calloc(1, sizeof(*z));
    if (z == NULL) {
        int e = errno;
        munlock(base, map_size);
        munmap(base, map_size);
        close(fd);
        if (is_owner) {
            shm_unlink(shm_name);
        }
        errno = e;
        return NULL;
    }

    (void)snprintf(z->name, sizeof z->name, "%s", shm_name);
    z->fd = fd;
    z->base = base;
    z->map_size = map_size;
    z->capacity = cap;
    z->is_owner = is_owner;
    z->is_huge = is_huge;
    z->locked = true;
    return z;
}

void tuxrt_zckb_destroy(tuxrt_zckb_t* z)
{
    if (z == NULL) {
        return;
    }
    if (z->locked && z->base != NULL && z->base != MAP_FAILED) {
        (void)munlock(z->base, z->map_size);
    }
    if (z->base != NULL && z->base != MAP_FAILED) {
        (void)munmap(z->base, z->map_size);
    }
    if (z->fd >= 0) {
        (void)close(z->fd);
    }
    if (z->is_owner) {
        (void)shm_unlink(z->name);
    }
    free(z);
}

void* tuxrt_zckb_map(tuxrt_zckb_t* z, size_t* out_size)
{
    if (z == NULL) {
        return NULL;
    }
    tuxrt_channel_t* ring = zckb_ring(z->base);
    if (out_size != NULL) {
        *out_size = (size_t)ring->capacity;
    }
    return (void*)ring;
}

int tuxrt_zckb_lock(tuxrt_zckb_t* z)
{
    if (z == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (z->locked) {
        return 0;
    }
    if (mlock(z->base, z->map_size) != 0) {
        return -1;
    }
    z->locked = true;
    return 0;
}

int tuxrt_zckb_unlock(tuxrt_zckb_t* z)
{
    if (z == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!z->locked) {
        return 0;
    }
    if (munlock(z->base, z->map_size) != 0) {
        return -1;
    }
    z->locked = false;
    return 0;
}
