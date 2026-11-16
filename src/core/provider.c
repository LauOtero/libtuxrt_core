/**
 * provider.c — Cargador universal de proveedores (TuxRT Provider ABI)
 *
 * Implementa el enlace UNIDIRECCIONAL consumidor -> proveedor: descubre el
 * símbolo descriptor `tuxrt_provider_descriptor` (ver tuxrt_provider.h) con
 * dlopen()/dlsym() y permite resolver símbolos del proveedor dejándolo
 * enlazado. Nunca aborta ni rompe el proceso si el proveedor no está instalado
 * (degradación elegante).
 *
 * Añadir un proveedor nuevo (ChronosRT o cualquier otro) NO requiere tocar este
 * archivo: basta con que exporte el símbolo descriptor.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <dlfcn.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

// Deriva el soname a partir de un nombre de proveedor:
//   "ChronosRT" -> "libchronosrt.so"   (se omiten los no alfanuméricos)
// Si `name` ya parece un soname (contiene ".so"), se usa tal cual.
static void tuxrt_provider_soname_for(const char* name, char* out, size_t len) {
    if (strstr(name, ".so") != NULL) {
        snprintf(out, len, "%s", name);
        return;
    }

    size_t j = 0;
    int n = snprintf(out, len, "lib");
    j = (n > 0) ? (size_t)n : 0;
    for (const char* p = name; *p != '\0' && j + 1 < len; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
        }
    }
    snprintf(out + j, len - j, ".so");
}

// Copia el descriptor a la estructura de info normalizada.
static void tuxrt_provider_fill(const tuxrt_provider_descriptor_t* d,
                                const char* soname, tuxrt_provider_info_t* out) {
    memset(out, 0, sizeof(*out));
    if (d->name) strncpy(out->name, d->name, sizeof(out->name) - 1);
    if (d->version) strncpy(out->version, d->version, sizeof(out->version) - 1);
    if (d->capabilities) {
        strncpy(out->capabilities, d->capabilities, sizeof(out->capabilities) - 1);
    }
    if (soname) strncpy(out->soname, soname, sizeof(out->soname) - 1);
    out->abi_version = d->abi_version;
    out->available = true;
}

tuxrt_err_t tuxrt_provider_open(const char* soname, void** handle,
                                tuxrt_provider_info_t* out) {
    if (handle) *handle = NULL;
    if (out) memset(out, 0, sizeof(*out));
    if (!soname) return TUXRT_ERR_INVAL;

    void* lib = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return TUXRT_ERR_DEPENDENCY_MISSING;

    void* sym = dlsym(lib, TUXRT_PROVIDER_SYMBOL);
    if (!sym) {
        dlclose(lib);
        return TUXRT_ERR_NOENT;
    }

    // dlsym() devuelve void*; se copia por bytes para evitar la conversión
    // directa que ISO C prohíbe (-Wpedantic/-Wcast-align=strict).
    const tuxrt_provider_descriptor_t* d = NULL;
    memcpy(&d, &sym, sizeof(d));

    if (!d || d->abi_version != TUXRT_PROVIDER_ABI_VERSION) {
        dlclose(lib);
        return TUXRT_ERR_VERSION_MISMATCH;
    }

    if (out) tuxrt_provider_fill(d, soname, out);

    if (handle) {
        *handle = lib;
    } else {
        dlclose(lib);
    }
    return TUXRT_OK;
}

void tuxrt_provider_close(void* handle) {
    if (handle) dlclose(handle);
}

tuxrt_err_t tuxrt_provider_symbol(void* handle, const char* symbol, void** out) {
    if (!handle || !symbol || !out) return TUXRT_ERR_INVAL;
    *out = dlsym(handle, symbol);
    return *out ? TUXRT_OK : TUXRT_ERR_NOENT;
}

tuxrt_err_t tuxrt_provider_probe(const char* soname, tuxrt_provider_info_t* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!soname) return TUXRT_ERR_INVAL;

    void* h = NULL;
    tuxrt_err_t rc = tuxrt_provider_open(soname, &h, out);
    tuxrt_provider_close(h);
    return rc;
}

tuxrt_err_t tuxrt_provider_query(const char* name, tuxrt_provider_info_t* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!name) return TUXRT_ERR_INVAL;

    char soname[TUXRT_PROVIDER_SONAME_MAX];
    tuxrt_provider_soname_for(name, soname, sizeof(soname));
    return tuxrt_provider_probe(soname, out);
}
