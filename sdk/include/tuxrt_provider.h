/*
 * tuxrt_provider.h — Contrato universal de proveedor (TuxRT Provider ABI).
 *
 * Cualquier biblioteca puede publicarse como PROVEEDOR de capacidades para
 * libtuxrt_core (o cualquier otro consumidor) exportando un ÚNICO símbolo
 * descriptor:
 *
 *      tuxrt_provider_descriptor   (tipo: tuxrt_provider_descriptor_t)
 *
 * El consumidor lo descubre y enlaza en tiempo de ejecución con dlopen/dlsym
 * (ver tuxrt_provider_open/close/symbol/probe/query en tuxrt_sdk.h), de modo
 * que:
 *   - La dependencia es UNIDIRECCIONAL: consumidor -> proveedor.
 *   - El proveedor NO enlaza ni conoce al consumidor.
 *   - Añadir un proveedor nuevo sólo requiere exportar el símbolo (o invocar
 *     la función CMake `tuxrt_provide()` del módulo TuxRTProvider.cmake).
 *
 * Este header es AUTOCONTENIDO: el proveedor sólo necesita esta cabecera, sin
 * arrastrar el resto del SDK ni la librería del consumidor.
 *
 * REGLA ABI: el layout del descriptor es parte del ABI. Nunca reordenar ni
 * cambiar el tamaño de los campos existentes; sólo añadir al final y subir
 * TUXRT_PROVIDER_ABI_VERSION.
 */
#ifndef TUXRT_PROVIDER_H
#define TUXRT_PROVIDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Versión del contrato de proveedor: major<<16 | minor<<8 | patch. */
#define TUXRT_PROVIDER_ABI_VERSION 0x000100UL

/* Nombre del símbolo descriptor que TODO proveedor debe exportar. Con
 * RTLD_LOCAL cada proveedor queda aislado, por lo que el nombre puede ser
 * idéntico en todos ellos sin colisión. */
#define TUXRT_PROVIDER_SYMBOL "tuxrt_provider_descriptor"

/* Visibilidad del símbolo descriptor. Necesaria con -fvisibility=hidden, el
 * preset habitual de las bibliotecas del ecosistema. */
#if defined(_WIN32) || defined(__CYGWIN__)
  #define TUXRT_PROVIDER_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
  #define TUXRT_PROVIDER_API __attribute__((visibility("default")))
#else
  #define TUXRT_PROVIDER_API
#endif

/* Descriptor de proveedor. El layout es el ABI. */
typedef struct tuxrt_provider_descriptor {
    uint32_t    abi_version;   /* == TUXRT_PROVIDER_ABI_VERSION              */
    const char* name;          /* identidad legible, p. ej. "ChronosRT"      */
    const char* version;       /* versión del proveedor, p. ej. "0.1.0"      */
    const char* capabilities;  /* CSV, p. ej. "ethercat,rt-scheduler,simd"   */
    uint32_t    flags;         /* reservado para el futuro; debe ser 0       */
} tuxrt_provider_descriptor_t;

/* Declara y exporta el descriptor. Usar UNA sola vez en el proveedor:
 *
 *      TUXRT_PROVIDER_EXPORT("ChronosRT", "0.1.0", "ethercat,rt-scheduler");
 */
#define TUXRT_PROVIDER_EXPORT(nm, ver, caps)                          \
    TUXRT_PROVIDER_API const tuxrt_provider_descriptor_t              \
        tuxrt_provider_descriptor = {                                 \
            TUXRT_PROVIDER_ABI_VERSION, (nm), (ver), (caps), 0u }

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_PROVIDER_H */
