/*
 * tuxrt_protocol.h — Selección del protocolo de red de TuxRT.
 *
 * TuxRT soporta dos protocolos de red MUTUAMENTE EXCLUYENTES por instancia:
 *
 *   - EtherCAT : esclavas con ASIC/FPGA dedicado, ciclos < 100 µs.
 *   - EtherTux : NICs y switches Ethernet comerciales estándar, SBC
 *                heterogéneas y MCUs con controlador Ethernet SPI/QSPI.
 *
 * La elección se resuelve EN TIEMPO DE CONFIGURACIÓN (variable de entorno
 * `TUXRT_PROTOCOL`, fichero de configuración o parámetro de línea de
 * comandos) y no cambia durante la ejecución: ambos backends comparten la HAL
 * de aceleración y el modelo de prioridades, pero sus ciclos de vida son
 * independientes.
 */
#ifndef TUXRT_PROTOCOL_H
#define TUXRT_PROTOCOL_H

#include "tuxrt_sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TUXRT_PROTOCOL_ETHERCAT = 0,
    TUXRT_PROTOCOL_ETHERTUX  = 1
} tuxrt_protocol_t;

/* Protocolo por defecto cuando no se especifica ninguno (EtherCAT). */
TUXRT_SDK_API tuxrt_protocol_t tuxrt_protocol_default(void);

/* Acepta "ethercat" / "ethertux" (sin distinguir mayúsculas ni espacios). */
TUXRT_SDK_API tuxrt_protocol_t tuxrt_protocol_from_string(const char* name);

/* Nombre canónico ("ethercat" / "ethertux"); nunca NULL. */
TUXRT_SDK_API const char* tuxrt_protocol_name(tuxrt_protocol_t protocol);

/* Lee TUXRT_PROTOCOL del entorno; si no existe o no es válido, ETHERCAT. */
TUXRT_SDK_API tuxrt_protocol_t tuxrt_protocol_from_env(void);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_PROTOCOL_H */
