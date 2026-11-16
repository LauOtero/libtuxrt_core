/**
 * transport_uart.c — UART/Serial Transport Adapter (termios backend)
 * 
 * Implements tuxrt_transport_vtable_t for UART communication
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include <termios.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "../../sdk/include/tuxrt_sdk.h"

typedef struct {
    int fd;
    struct termios old_tios;
    pthread_mutex_t mutex;
    bool configured;
} uart_transport_impl_t;

static speed_t baud_to_speed(uint32_t baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 500000: return B500000;
        case 576000: return B576000;
        case 921600: return B921600;
        case 1000000: return B1000000;
        case 1152000: return B1152000;
        case 1500000: return B1500000;
        case 2000000: return B2000000;
        case 2500000: return B2500000;
        case 3000000: return B3000000;
        case 3500000: return B3500000;
        case 4000000: return B4000000;
        default: return B115200;
    }
}

static tuxrt_err_t uart_open(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return TUXRT_ERR_INVAL;
    
    uart_transport_impl_t* impl = calloc(1, sizeof(uart_transport_impl_t));
    if (!impl) return TUXRT_ERR_NOMEM;
    
    pthread_mutex_init(&impl->mutex, NULL);
    
    // Open device
    impl->fd = open(adapter->config.device_path, O_RDWR | O_NOCTTY | O_SYNC);
    if (impl->fd < 0) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_NOENT;
    }
    
    // Save old settings
    tcgetattr(impl->fd, &impl->old_tios);
    
    // Configure
    struct termios tios = {0};
    tios.c_cflag = CS8 | CREAD | CLOCAL;
    tios.c_iflag = IGNBRK | IGNPAR;
    tios.c_oflag = 0;
    tios.c_lflag = 0;
    tios.c_cc[VMIN] = 1;
    tios.c_cc[VTIME] = 0;
    
    cfsetospeed(&tios, baud_to_speed(adapter->config.baudrate));
    cfsetispeed(&tios, baud_to_speed(adapter->config.baudrate));
    
    if (adapter->config.flow_control) {
        tios.c_cflag |= CRTSCTS;
    }
    
    tcflush(impl->fd, TCIOFLUSH);
    if (tcsetattr(impl->fd, TCSANOW, &tios) < 0) {
        close(impl->fd);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }
    
    impl->configured = true;
    adapter->impl = impl;
    adapter->active = true;
    
    tuxrt_sirc_update_health_success(adapter);
    
    return TUXRT_OK;
}

static tuxrt_err_t uart_close(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) return TUXRT_ERR_INVAL;
    
    uart_transport_impl_t* impl = (uart_transport_impl_t*)adapter->impl;
    
    pthread_mutex_lock(&impl->mutex);
    
    if (impl->configured) {
        tcsetattr(impl->fd, TCSANOW, &impl->old_tios);
        impl->configured = false;
    }
    if (impl->fd >= 0) {
        close(impl->fd);
        impl->fd = -1;
    }
    
    pthread_mutex_unlock(&impl->mutex);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    adapter->impl = NULL;
    adapter->active = false;
    
    return TUXRT_OK;
}

static tuxrt_err_t uart_send(tuxrt_transport_adapter_t* adapter, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !data) return TUXRT_ERR_INVAL;
    
    uart_transport_impl_t* impl = (uart_transport_impl_t*)adapter->impl;
    
    pthread_mutex_lock(&impl->mutex);
    
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(impl->fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                // Check deadline
                if (tuxrt_now_ns() >= deadline) break;
                continue;
            }
            pthread_mutex_unlock(&impl->mutex);
            tuxrt_sirc_update_health_failure(adapter);
            return TUXRT_ERR_IO;
        }
        written += n;
    }
    
    pthread_mutex_unlock(&impl->mutex);
    
    if (written == len) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    
    return (written == len) ? TUXRT_OK : TUXRT_ERR_TIMEDOUT;
}

static tuxrt_err_t uart_recv(tuxrt_transport_adapter_t* adapter, uint8_t* buf, size_t* len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !buf || !len) return TUXRT_ERR_INVAL;
    
    uart_transport_impl_t* impl = (uart_transport_impl_t*)adapter->impl;
    
    pthread_mutex_lock(&impl->mutex);
    
    size_t read_total = 0;
    while (read_total < *len) {
        ssize_t n = read(impl->fd, buf + read_total, *len - read_total);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                if (tuxrt_now_ns() >= deadline) break;
                continue;
            }
            pthread_mutex_unlock(&impl->mutex);
            tuxrt_sirc_update_health_failure(adapter);
            return TUXRT_ERR_IO;
        }
        if (n == 0) break; // EOF
        read_total += n;
    }
    
    *len = read_total;
    
    pthread_mutex_unlock(&impl->mutex);
    
    if (read_total > 0) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    pthread_mutex_unlock(&adapter->mutex);
    
    return TUXRT_OK;
}

static tuxrt_err_t uart_flush(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) return TUXRT_ERR_INVAL;
    
    uart_transport_impl_t* impl = (uart_transport_impl_t*)adapter->impl;
    pthread_mutex_lock(&impl->mutex);
    tcflush(impl->fd, TCIOFLUSH);
    pthread_mutex_unlock(&impl->mutex);
    
    return TUXRT_OK;
}

static tuxrt_err_t uart_get_stats(tuxrt_transport_adapter_t* adapter, void* stats_out) {
    if (!adapter || !stats_out) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&adapter->mutex);
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    pthread_mutex_unlock(&adapter->mutex);
    
    return TUXRT_OK;
}

static void uart_destroy(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    uart_close(adapter);
}

const tuxrt_transport_vtable_t tuxrt_transport_uart_vtable = {
    .abi_version = 1,
    .open = uart_open,
    .close = uart_close,
    .send = uart_send,
    .recv = uart_recv,
    .flush = uart_flush,
    .get_stats = uart_get_stats,
    .destroy = uart_destroy
};