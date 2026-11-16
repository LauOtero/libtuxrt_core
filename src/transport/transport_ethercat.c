/*
 * transport_ethercat.c — Registro de adaptador de transporte EtherCAT
 *
 * Este archivo registra la vtable tuxrt_transport_ethercat_vtable para que el
 * núcleo SIRC pueda cargar y usar el adaptador EtherCAT de forma dinámica.
 *
 * Adaptado después de transport_ethertux.c de libtuxwarp, implementado para
 * libtuxrt_core sin dependencias externas.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS

#include "../../sdk/include/tuxrt_sdk.h"

/* Declaración de la vtable (definida en transport_ethercat.c) */
extern const tuxrt_transport_vtable_t tuxrt_transport_ethercat_vtable;

/* ------------------------------------------------------------------ */
/* Registro del adaptador                                             */
/* ------------------------------------------------------------------ */

tuxrt_err_t tuxrt_transport_ethercat_register(void) {
    return tuxrt_transport_register_vtable(&tuxrt_transport_ethercat_vtable);
}

/* ------------------------------------------------------------------ */
/* Función de limpieza / cierre                                       */
/* ------------------------------------------------------------------ */

void tuxrt_transport_ethercat_unregister(void) {
    tuxrt_transport_unregister_vtable(&tuxrt_transport_ethercat_vtable);
}