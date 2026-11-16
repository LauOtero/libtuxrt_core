/**
 * transport.c — Transport / MCU Communication (CORE) — Legacy API
 * 
 * USB/UART/CAN/Ethernet communication con MCU
 * Legacy simple transport API - wraps SIRC transport adapters
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>

#ifndef CRTSCTS
#define CRTSCTS 020000000000
#endif

// Implement legacy struct (separate from SIRC tuxrt_transport_t)
struct tuxrt_transport_t {
    tuxrt_transport_config_t cfg;
    int fd;
    bool open;
    pthread_mutex_t mutex;
    // Ring buffers
    uint8_t* rx_buf;
    size_t rx_cap;
    size_t rx_head, rx_tail;
    uint8_t* tx_buf;
    size_t tx_cap;
    size_t tx_head, tx_tail;
};

static int uart_configure(int fd, uint32_t baudrate) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;
    
    speed_t speed;
    switch (baudrate) {
        case 115200: speed = B115200; break;
        case 230400: speed = B230400; break;
        case 460800: speed = B460800; break;
        case 500000: speed = B500000; break;
        case 576000: speed = B576000; break;
        case 921600: speed = B921600; break;
        case 1000000: speed = B1000000; break;
        default: speed = B115200;
    }
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
    
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    
    return tcsetattr(fd, TCSANOW, &tty);
}

tuxrt_err_t tuxrt_transport_create(const tuxrt_transport_config_t* cfg, tuxrt_legacy_transport_t** out_transport) {
    if (!cfg || !out_transport) return TUXRT_ERR_INVAL;
    
    tuxrt_legacy_transport_t* t = calloc(1, sizeof(tuxrt_legacy_transport_t));
    if (!t) return TUXRT_ERR_NOMEM;
    
    t->cfg = *cfg;
    t->fd = -1;
    t->open = false;
    t->rx_cap = 4096;
    t->rx_buf = malloc(t->rx_cap);
    t->tx_cap = 4096;
    t->tx_buf = malloc(t->tx_cap);
    pthread_mutex_init(&t->mutex, NULL);
    
    if (!t->rx_buf || !t->tx_buf) {
        free(t->rx_buf);
        free(t->tx_buf);
        free(t);
        return TUXRT_ERR_NOMEM;
    }
    
    *out_transport = t;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_transport_open(tuxrt_legacy_transport_t* transport) {
    if (!transport) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&transport->mutex);

    if (transport->open) {
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_OK;
    }

    int fd = -1;
    switch (transport->cfg.type) {
        case TUXRT_TRANSPORT_UART:
            fd = open(transport->cfg.device_path, O_RDWR | O_NOCTTY | O_SYNC);
            if (fd >= 0 && uart_configure(fd, transport->cfg.baudrate) < 0) {
                close(fd);
                fd = -1;
            }
            break;
        case TUXRT_TRANSPORT_ETHERCAT: {
            // EtherCAT uses the vtable from transport_ethercat.c
            // The legacy API doesn't fully support raw socket operations,
            // so we return DEPRECATED to encourage using the SIRC transport API.
            pthread_mutex_unlock(&transport->mutex);
            return TUXRT_ERR_NOSYS;  // Use tuxrt_sirc_register_transport() instead
        }
        case TUXRT_TRANSPORT_ETHERTUX: {
            // EtherTux uses the vtable from transport_ethertux.c
            // The legacy API doesn't fully support raw socket operations,
            // so we return DEPRECATED to encourage using the SIRC transport API.
            pthread_mutex_unlock(&transport->mutex);
            return TUXRT_ERR_NOSYS;  // Use tuxrt_sirc_register_transport() instead
        }
        case TUXRT_TRANSPORT_USB:
        case TUXRT_TRANSPORT_CAN:
        case TUXRT_TRANSPORT_SPI:
        case TUXRT_TRANSPORT_I2C:
            // TODO: implementar
            fd = -1;
            break;
    }

    if (fd < 0) {
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_ERR_IO;
    }

    transport->fd = fd;
    transport->open = true;
    pthread_mutex_unlock(&transport->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_transport_close(tuxrt_legacy_transport_t* transport) {
    if (!transport) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&transport->mutex);
    if (transport->fd >= 0) {
        close(transport->fd);
        transport->fd = -1;
    }
    transport->open = false;
    pthread_mutex_unlock(&transport->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_transport_send(tuxrt_legacy_transport_t* transport, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline) {
    if (!transport || !data || len == 0) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&transport->mutex);
    if (!transport->open || transport->fd < 0) {
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_ERR_IO;
    }

    // Compute remaining time until deadline for timeout calculation
    tuxrt_timestamp_ns_t now = tuxrt_scheduler_now(NULL); // Note: scheduler must be initialized
    tuxrt_timestamp_ns_t remaining_ns = deadline - now;

    struct pollfd pfd = {.fd = transport->fd, .events = POLLOUT};
    int ret;
    if (remaining_ns > 0) {
        // Convert nanoseconds to milliseconds for poll, with minimum of 1ms
        long timeout_ms = (long)(remaining_ns / 1000000LL);
        if (timeout_ms < 1) timeout_ms = 1;
        if (timeout_ms > 1000) timeout_ms = 1000; // cap at 1s
        ret = poll(&pfd, 1, timeout_ms);
    } else {
        // Deadline already passed, immediate return
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_ERR_TIMEDOUT;
    }

    if (ret <= 0) {
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_ERR_TIMEDOUT;
    }

    ssize_t written = write(transport->fd, data, len);
    pthread_mutex_unlock(&transport->mutex);

    if (written < 0) return TUXRT_ERR_IO;
    if ((size_t)written != len) return TUXRT_ERR_TIMEDOUT;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_transport_recv(tuxrt_legacy_transport_t* transport, tuxrt_buffer_t* buf, tuxrt_timestamp_ns_t deadline) {
    if (!transport || !buf) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&transport->mutex);
    if (!transport->open || transport->fd < 0) {
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_ERR_IO;
    }

    // Compute remaining time until deadline for timeout calculation
    tuxrt_timestamp_ns_t now = tuxrt_scheduler_now(NULL); // Note: scheduler must be initialized
    tuxrt_timestamp_ns_t remaining_ns = deadline - now;

    struct pollfd pfd = {.fd = transport->fd, .events = POLLIN};
    int ret;
    if (remaining_ns > 0) {
        // Convert nanoseconds to milliseconds for poll, with minimum of 1ms
        long timeout_ms = (long)(remaining_ns / 1000000LL);
        if (timeout_ms < 1) timeout_ms = 1;
        if (timeout_ms > 1000) timeout_ms = 1000; // cap at 1s
        ret = poll(&pfd, 1, timeout_ms);
    } else {
        // Deadline already passed, immediate return
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_ERR_TIMEDOUT;
    }

    if (ret <= 0) {
        pthread_mutex_unlock(&transport->mutex);
        return TUXRT_ERR_TIMEDOUT;
    }

    if (buf->capacity == 0) {
        buf->capacity = 256;
        buf->data = malloc(buf->capacity);
    }

    ssize_t n = read(transport->fd, buf->data, buf->capacity);
    pthread_mutex_unlock(&transport->mutex);

    if (n < 0) return TUXRT_ERR_IO;
    buf->len = n;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_transport_flush(tuxrt_legacy_transport_t* transport) {
    if (!transport) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&transport->mutex);
    if (transport->fd >= 0) tcdrain(transport->fd);
    pthread_mutex_unlock(&transport->mutex);
    return TUXRT_OK;
}

void tuxrt_transport_destroy(tuxrt_legacy_transport_t* transport) {
    if (!transport) return;
    tuxrt_transport_close(transport);
    pthread_mutex_destroy(&transport->mutex);
    free(transport->rx_buf);
    free(transport->tx_buf);
    free(transport);
}