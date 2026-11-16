/*
 * protocol.c — Resolución del protocolo de red (EtherCAT / EtherTux).
 */
#include "tuxrt_protocol.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

tuxrt_protocol_t tuxrt_protocol_default(void) {
    return TUXRT_PROTOCOL_ETHERCAT;
}

tuxrt_protocol_t tuxrt_protocol_from_string(const char* name) {
    if (name == NULL) {
        return tuxrt_protocol_default();
    }

    /* Normaliza a minúsculas descartando espacios y tabuladores. */
    char buf[16];
    size_t len = 0u;
    for (const char* p = name; *p != '\0' && len < sizeof(buf) - 1u; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') {
            continue;
        }
        buf[len++] = (char)tolower(c);
    }
    buf[len] = '\0';

    if (strcmp(buf, "ethertux") == 0) {
        return TUXRT_PROTOCOL_ETHERTUX;
    }
    if (strcmp(buf, "ethercat") == 0) {
        return TUXRT_PROTOCOL_ETHERCAT;
    }
    return tuxrt_protocol_default();
}

const char* tuxrt_protocol_name(tuxrt_protocol_t protocol) {
    return (protocol == TUXRT_PROTOCOL_ETHERTUX) ? "ethertux" : "ethercat";
}

tuxrt_protocol_t tuxrt_protocol_from_env(void) {
    const char* env = getenv("TUXRT_PROTOCOL");
    if (env == NULL || env[0] == '\0') {
        return tuxrt_protocol_default();
    }
    return tuxrt_protocol_from_string(env);
}
