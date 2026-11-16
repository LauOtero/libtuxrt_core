/**
 * transport_websocket.c — Cliente WebSocket asíncrono no bloqueante
 *
 * Implementa tuxrt_core_ws_connect() y tuxrt_core_ws_service() usando
 * libcurl (websockets.h) como backend de transporte. Opera en modo
 * no bloqueante: la conexión se establece en segundo plano y el servicio
 * procesa eventos sin bloquear el hilo llamante.
 *
 * Búferes binarios listos para Mojo FFI (structs con campos alineados).
 */

#define _POSIX_C_SOURCE 200809L
#define TUXRT_CORE_EXPORTS

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "../../sdk/include/tuxrt_sdk.h"
#include "../../sdk/include/tuxrt_ethertux.h"
#include <curl/curl.h>
#include <curl/websockets.h>

/* ------------------------------------------------------------------ */
/* Configuración                                                      */
/* ------------------------------------------------------------------ */

#define WS_MAX_URL_LEN     2048u
#define WS_MAX_BUFFER      (64 * 1024)   /* 64 KiB búfer por dirección */
#define WS_HANDSHAKE_MS    5000u        /* timeout default de handshake */
#define WS_POLL_TIMEOUT_MS 100u         /* poll() por defecto */

/* Estado WebSocket (declarado en tuxrt_ethertux.h, incluido arriba) */

/* Estructura interna opaca del cliente WebSocket */
typedef struct {
    CURL*            curl;              /* handle de libcurl */
    tuxrt_ws_state_t state;             /* estado actual */
    char             url[WS_MAX_URL_LEN]; /* URL objetivo */
    int              fd;                /* socket subyacente (-1 si no disponible) */
    uint64_t         connect_ts_ns;     /* timestamp de inicio de conexión */
    uint32_t         timeout_ms;        /* timeout en ms para operaciones */
    bool             auto_pong;         /* responder PONG automáticamente */
    tuxrt_err_t      last_err;          /* último error ocurrido */
    pthread_mutex_t  mutex;             /* sincronización */
} tuxrt_ws_client_impl_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline uint64_t ws_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline int ws_elapsed_ms(uint64_t start_ns) {
    uint64_t now = ws_now_ns();
    if (now < start_ns) return 0;
    return (int)((now - start_ns) / 1000000ull);
}

/* Extrae host, port y path de una URL WebSocket (ws:// o wss://) */
static tuxrt_err_t ws_parse_url(const char* url,
                                 char* host, size_t host_len,
                                 int* out_port,
                                 char* path, size_t path_len) {
    if (!url || !host || !out_port || !path) {
        return TUXRT_ERR_INVAL;
    }

    const char* p = url;

    /* Detectar esquema */
    bool secure = false;
    if (strncmp(p, "wss://", 6) == 0) {
        secure = true;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else {
        return TUXRT_ERR_INVAL;
    }

    /* Encontrar el final del host (/:?#) */
    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    const char* qmark = strchr(p, '?');
    const char* hash  = strchr(p, '#');

    /* Determinar el límite del host */
    const char* host_end = slash;
    if (colon && (!host_end || colon < host_end)) host_end = colon;
    if (qmark && (!host_end || qmark < host_end)) host_end = qmark;
    if (hash  && (!host_end || hash  < host_end)) host_end = hash;
    if (!host_end) host_end = p + strlen(p);

    size_t host_len_req = (size_t)(host_end - p);
    if (host_len_req >= host_len) {
        return TUXRT_ERR_INVAL;
    }
    strncpy(host, p, host_len_req);
    host[host_len_req] = '\0';

    /* Puerto */
    *out_port = secure ? 443 : 80;
    if (colon) {
        long port_val = strtol(colon + 1, NULL, 10);
        if (port_val > 0 && port_val <= 65535) {
            *out_port = (int)port_val;
        }
    }

    /* Path */
    if (slash) {
        size_t plen = strnlen(slash, path_len);
        strncpy(path, slash, path_len);
        path[path_len - 1] = '\0';
    } else {
        strncpy(path, "/", path_len);
    }

    return TUXRT_OK;
}

/* ------------------------------------------------------------------ */
/* Conexión asíncrona (no bloqueante)                                  */
/* ------------------------------------------------------------------ */

TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_connect(const char* url,
                                                  uint32_t timeout_ms,
                                                  tuxrt_ws_client_t** out_client) {
    if (!url || !out_client) {
        return TUXRT_ERR_INVAL;
    }

    *out_client = NULL;

    /* Validar longitud de URL */
    size_t ulen = strlen(url);
    if (ulen == 0 || ulen >= WS_MAX_URL_LEN) {
        return TUXRT_ERR_INVAL;
    }

    /* curl_global_init se espera que se llame una vez al iniciar la aplicación.
       No aquí por llamada por llamada — es thread-safe desde curl 7.56+ pero
       no es seguro llamarlo desde cada tuxrt_core_ws_connect(). */
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);  /* asegurar init; idempotente */

    /* Crear instanciaplugin */
    tuxrt_ws_client_impl_t* impl = (tuxrt_ws_client_impl_t*)calloc(1, sizeof(*impl));
    if (!impl) {
        return TUXRT_ERR_NOMEM;
    }

    impl->curl = curl_easy_init();
    if (!impl->curl) {
        free(impl);
        return TUXRT_ERR_NOMEM;
    }

    strncpy(impl->url, url, WS_MAX_URL_LEN - 1);
    impl->state = TUXRT_WS_IDLE;
    impl->fd = -1;
    impl->timeout_ms = timeout_ms ?: WS_HANDSHAKE_MS;
    impl->auto_pong = true;
    impl->last_err = TUXRT_OK;

    pthread_mutex_init(&impl->mutex, NULL);

    /* Configurar curl para conexión WebSocket */
    curl_easy_setopt(impl->curl, CURLOPT_URL, url);
    curl_easy_setopt(impl->curl, CURLOPT_WS_OPTIONS, CURLWS_NOAUTOPONG);
    curl_easy_setopt(impl->curl, CURLOPT_TIMEOUT_MS, (long)impl->timeout_ms);
    curl_easy_setopt(impl->curl, CURLOPT_CONNECT_ONLY, 1L);

    /* No ejecutar todavía — el caller debe invocar ws_service() para completar */
    impl->state = TUXRT_WS_CONNECTING;
    impl->connect_ts_ns = ws_now_ns();

    *out_client = (tuxrt_ws_client_t*)impl;
    return TUXRT_OK;
}

TUXRT_SDK_API void tuxrt_core_ws_disconnect(tuxrt_ws_client_t* client) {
    if (!client) {
        return;
    }
    tuxrt_ws_client_impl_t* impl = (tuxrt_ws_client_impl_t*)client;

    pthread_mutex_lock(&impl->mutex);

    if (impl->state != TUXRT_WS_CLOSED && impl->state != TUXRT_WS_IDLE) {
        impl->state = TUXRT_WS_CLOSING;
    }

    pthread_mutex_unlock(&impl->mutex);

    if (impl->curl) {
        curl_easy_cleanup(impl->curl);
        impl->curl = NULL;
    }

    pthread_mutex_destroy(&impl->mutex);
    free(impl);
}

/* ------------------------------------------------------------------ */
/* Servicio no bloqueante                                              */
/* ------------------------------------------------------------------ */

TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_service(tuxrt_ws_client_t* client,
                                                   uint32_t timeout_ms) {
    if (!client) {
        return TUXRT_ERR_INVAL;
    }
    tuxrt_ws_client_impl_t* impl = (tuxrt_ws_client_impl_t*)client;

    pthread_mutex_lock(&impl->mutex);

    switch (impl->state) {
    case TUXRT_WS_IDLE:
    case TUXRT_WS_CLOSED:
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_INVAL;

    case TUXRT_WS_CLOSING: {
        /* Intentar cierre limpio */
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_OK; /* Ya se cerró en disconnect */
    }

    case TUXRT_WS_CONNECTING: {
        /* Completar handshake si aún no se hizo */
        uint64_t elapsed = (uint64_t)ws_elapsed_ms(impl->connect_ts_ns);
        if (elapsed > impl->timeout_ms) {
            impl->state = TUXRT_WS_CLOSED;
            impl->last_err = TUXRT_ERR_TIMEDOUT;
            pthread_mutex_unlock(&impl->mutex);
            return TUXRT_ERR_TIMEDOUT;
        }

        int poll_ms = (int)(impl->timeout_ms - (uint32_t)elapsed);
        if (poll_ms <= 0) poll_ms = 1;

        /* Obtener fd del handle curl para poll */
        curl_socket_t sock;
        curl_infotype infotype = CURLINFO_ACTIVESOCKET;
        CURLcode sc = curl_easy_getinfo(impl->curl, infotype, &sock);
        (void)sc; /* INFO socket puede no estar disponible inmediatamente */

        struct pollfd pfd = {.fd = (int)sock, .events = POLLIN | POLLOUT};
        int pr = poll(&pfd, 1, poll_ms);

        if (pr < 0) {
            if (errno == EINTR) {
                pthread_mutex_unlock(&impl->mutex);
                return TUXRT_ERR_AGAIN;
            }
            impl->last_err = TUXRT_ERR_IO;
            impl->state = TUXRT_WS_CLOSED;
            pthread_mutex_unlock(&impl->mutex);
            return TUXRT_ERR_IO;
        }

        if (pr == 0) {
            pthread_mutex_unlock(&impl->mutex);
            return TUXRT_ERR_AGAIN;
        }

        /* Si el socket está listo, intentar completar la conexión */
        if (pfd.revents & (POLLIN | POLLOUT)) {
            CURLcode cc = curl_easy_perform(impl->curl);
            if (cc == CURLE_OK) {
                impl->state = TUXRT_WS_CONNECTED;
                impl->last_err = TUXRT_OK;
                pthread_mutex_unlock(&impl->mutex);
                return TUXRT_OK;
            } else if (cc == CURLE_OPERATION_TIMEDOUT) {
                impl->state = TUXRT_WS_CLOSED;
                impl->last_err = TUXRT_ERR_TIMEDOUT;
                pthread_mutex_unlock(&impl->mutex);
                return TUXRT_ERR_TIMEDOUT;
            }
            impl->last_err = TUXRT_ERR_IO;
            impl->state = TUXRT_WS_CLOSED;
            pthread_mutex_unlock(&impl->mutex);
            return TUXRT_ERR_IO;
        }

        if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)) {
            impl->last_err = TUXRT_ERR_IO;
            impl->state = TUXRT_WS_CLOSED;
            pthread_mutex_unlock(&impl->mutex);
            return TUXRT_ERR_IO;
        }

        /* Aún no listo, volver a intentarlo */
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_AGAIN;
    }

    case TUXRT_WS_CONNECTED: {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_OK; /* ya conectado; service es no-op hasta que llegue dato */
    }

    default:
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_INVAL;
    }
}

/* ------------------------------------------------------------------ */
/* Envío no bloqueante                                                 */
/* ------------------------------------------------------------------ */

TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_send(tuxrt_ws_client_t* client,
                                               const uint8_t* data,
                                               uint32_t len) {
    if (!client || !data || len == 0) {
        return TUXRT_ERR_INVAL;
    }
    tuxrt_ws_client_impl_t* impl = (tuxrt_ws_client_impl_t*)client;

    pthread_mutex_lock(&impl->mutex);
    if (impl->state != TUXRT_WS_CONNECTED) {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_INVAL;
    }
    pthread_mutex_unlock(&impl->mutex);

    size_t sent = 0;
    CURLcode rc = curl_ws_send(impl->curl, data, (size_t)len, &sent,
                                (curl_off_t)len, CURLWS_BINARY);
    if (rc != CURLE_OK) {
        return TUXRT_ERR_IO;
    }
    (void)sent;
    return TUXRT_OK;
}

/* ------------------------------------------------------------------ */
/* Recepción no bloqueante                                             */
/* ------------------------------------------------------------------ */

TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_recv(tuxrt_ws_client_t* client,
                                               tuxrt_ws_frame_t* out_frame,
                                               uint32_t timeout_ms) {
    if (!client || !out_frame) {
        return TUXRT_ERR_INVAL;
    }
    tuxrt_ws_client_impl_t* impl = (tuxrt_ws_client_impl_t*)client;

    pthread_mutex_lock(&impl->mutex);
    if (impl->state != TUXRT_WS_CONNECTED) {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_INVAL;
    }
    pthread_mutex_unlock(&impl->mutex);

    /* Calcular deadline */
    uint64_t deadline_ns = ws_now_ns() + ((uint64_t)timeout_ms * 1000000ull);

    while (true) {
        uint8_t buf[WS_MAX_BUFFER];
        size_t recv_len = 0;
        const struct curl_ws_frame* meta = NULL;

        CURLcode rc = curl_ws_recv(impl->curl, buf, sizeof(buf), &recv_len, &meta);

        if (rc == CURLE_OK && recv_len > 0) {
            /* Frame recibido exitosamente */
            out_frame->data = buf;
            out_frame->len = (uint32_t)recv_len;
            out_frame->flags = meta ? meta->flags : 0;
            out_frame->timestamp_ns = ws_now_ns();
            impl->last_err = TUXRT_OK;
            return TUXRT_OK;
        }

        if (rc == CURLE_AGAIN) {
            /* No hay datos disponibles — verificar timeout */
            uint64_t now = ws_now_ns();
            if (now >= deadline_ns) {
                impl->last_err = TUXRT_ERR_TIMEDOUT;
                return TUXRT_ERR_TIMEDOUT;
            }
            /* Poll no bloqueante esperando datos */
            curl_socket_t sock;
            curl_easy_getinfo(impl->curl, CURLINFO_ACTIVESOCKET, &sock);
            struct pollfd pfd = {.fd = (int)sock, .events = POLLIN};
            uint64_t remaining_us = deadline_ns - now;
            int wait_ms = (int)((remaining_us + 999ULL) / 1000ULL);
            if (wait_ms <= 0) wait_ms = 1;
            int pr = poll(&pfd, 1, wait_ms);
            if (pr < 0 && errno == EINTR) {
                continue;
            }
            if (pr <= 0) {
                impl->last_err = TUXRT_ERR_TIMEDOUT;
                return TUXRT_ERR_TIMEDOUT;
            }
            continue;
        }

        if (rc == CURLE_GOT_NOTHING || rc == CURLE_SEND_ERROR) {
            /* Ping/pong interno o frame vacío — reintentar */
            continue;
        }

        /* Error fatal */
        impl->last_err = TUXRT_ERR_IO;
        return TUXRT_ERR_IO;
    }
}

/* ------------------------------------------------------------------ */
/* Consulta de estado                                                  */
/* ------------------------------------------------------------------ */

TUXRT_SDK_API tuxrt_ws_state_t tuxrt_core_ws_get_state(tuxrt_ws_client_t* client) {
    if (!client) {
        return TUXRT_WS_IDLE;
    }
    tuxrt_ws_client_impl_t* impl = (tuxrt_ws_client_impl_t*)client;
    pthread_mutex_lock(&impl->mutex);
    tuxrt_ws_state_t st = impl->state;
    pthread_mutex_unlock(&impl->mutex);
    return st;
}

TUXRT_SDK_API tuxrt_err_t tuxrt_core_ws_last_error(tuxrt_ws_client_t* client) {
    if (!client) {
        return TUXRT_ERR_INVAL;
    }
    tuxrt_ws_client_impl_t* impl = (tuxrt_ws_client_impl_t*)client;
    pthread_mutex_lock(&impl->mutex);
    tuxrt_err_t err = impl->last_err;
    pthread_mutex_unlock(&impl->mutex);
    return err;
}
