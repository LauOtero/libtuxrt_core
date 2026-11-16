/**
 * transport_usb.c — USB Transport Adapter (libusb backend)
 * 
 * Implements tuxrt_transport_vtable_t for USB communication
 */

#define _POSIX_C_SOURCE 200809L
#define TUXRT_CORE_EXPORTS
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include "../../sdk/include/tuxrt_sdk.h"

// libusb headers (conditionally included)
#ifdef HAVE_LIBUSB
#include <libusb-1.0/libusb.h>
#else
// Stub types when libusb not available
typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;
#endif

typedef struct {
    libusb_context* ctx;
    libusb_device_handle* handle;
    uint8_t ep_in;
    uint8_t ep_out;
    int interface_num;
    pthread_mutex_t mutex;
    bool claimed;
} usb_transport_impl_t;

static tuxrt_err_t usb_open(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return TUXRT_ERR_INVAL;
    
    usb_transport_impl_t* impl = calloc(1, sizeof(usb_transport_impl_t));
    if (!impl) return TUXRT_ERR_NOMEM;
    
    pthread_mutex_init(&impl->mutex, NULL);
    
#ifdef HAVE_LIBUSB
    int ret = libusb_init(&impl->ctx);
    if (ret < 0) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }
    
    // Open device by VID/PID or path
    // For now, use device path as VID:PID
    uint16_t vid = 0, pid = 0;
    if (adapter->config.device_path) {
        sscanf(adapter->config.device_path, "%hx:%hx", &vid, &pid);
    }
    
    impl->handle = libusb_open_device_with_vid_pid(impl->ctx, vid, pid);
    if (!impl->handle) {
        libusb_exit(impl->ctx);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_NOENT;
    }
    
    // Claim interface
    ret = libusb_claim_interface(impl->handle, impl->interface_num);
    if (ret < 0) {
        libusb_close(impl->handle);
        libusb_exit(impl->ctx);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }
    impl->claimed = true;
    
    // Find endpoints (simplified - would parse descriptors)
    impl->ep_in = 0x81;
    impl->ep_out = 0x01;
#else
    // Stub: just mark as opened
    (void)adapter;
#endif
    
    adapter->impl = impl;
    adapter->active = true;
    
    // Update health
    tuxrt_sirc_update_health_success(adapter);
    
    return TUXRT_OK;
}

static tuxrt_err_t usb_close(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) return TUXRT_ERR_INVAL;
    
    usb_transport_impl_t* impl = (usb_transport_impl_t*)adapter->impl;
    
    pthread_mutex_lock(&impl->mutex);
    
#ifdef HAVE_LIBUSB
    if (impl->claimed) {
        libusb_release_interface(impl->handle, impl->interface_num);
        impl->claimed = false;
    }
    if (impl->handle) {
        libusb_close(impl->handle);
        impl->handle = NULL;
    }
    if (impl->ctx) {
        libusb_exit(impl->ctx);
        impl->ctx = NULL;
    }
#endif
    
    pthread_mutex_unlock(&impl->mutex);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    adapter->impl = NULL;
    adapter->active = false;
    
    return TUXRT_OK;
}

static tuxrt_err_t usb_send(tuxrt_transport_adapter_t* adapter, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !data) return TUXRT_ERR_INVAL;
    
    usb_transport_impl_t* impl = (usb_transport_impl_t*)adapter->impl;
    (void)deadline;
    
    pthread_mutex_lock(&impl->mutex);
    
#ifdef HAVE_LIBUSB
    int transferred = 0;
    int ret = libusb_bulk_transfer(impl->handle, impl->ep_out, 
                                    (unsigned char*)data, len, &transferred, 1000);
    if (ret < 0 || (size_t)transferred != len) {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_IO;
    }
#else
    (void)len;
    (void)deadline;
#endif
    
    pthread_mutex_unlock(&impl->mutex);
    
    // Update health
    tuxrt_sirc_update_health_success(adapter);
    
    return TUXRT_OK;
}

static tuxrt_err_t usb_recv(tuxrt_transport_adapter_t* adapter, uint8_t* buf, size_t* len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !buf || !len) return TUXRT_ERR_INVAL;
    
    usb_transport_impl_t* impl = (usb_transport_impl_t*)adapter->impl;
    (void)deadline;
    
    pthread_mutex_lock(&impl->mutex);
    
#ifdef HAVE_LIBUSB
    int transferred = 0;
    int ret = libusb_bulk_transfer(impl->handle, impl->ep_in, buf, *len, &transferred, 1000);
    if (ret < 0) {
        pthread_mutex_unlock(&impl->mutex);
        tuxrt_sirc_update_health_failure(adapter);
        return TUXRT_ERR_IO;
    }
    *len = transferred;
#else
    (void)deadline;
    *len = 0;
#endif
    
    pthread_mutex_unlock(&impl->mutex);
    
    // Update health
    tuxrt_sirc_update_health_success(adapter);
    
    return TUXRT_OK;
}

static tuxrt_err_t usb_flush(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) return TUXRT_ERR_INVAL;
    (void)adapter;
    // USB bulk endpoints don't need explicit flush
    return TUXRT_OK;
}

static tuxrt_err_t usb_get_stats(tuxrt_transport_adapter_t* adapter, void* stats_out) {
    if (!adapter || !stats_out) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&adapter->mutex);
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    pthread_mutex_unlock(&adapter->mutex);
    
    return TUXRT_OK;
}

static void usb_destroy(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    usb_close(adapter);
}

// VTable for USB transport
const tuxrt_transport_vtable_t tuxrt_transport_usb_vtable = {
    .abi_version = 1,
    .open = usb_open,
    .close = usb_close,
    .send = usb_send,
    .recv = usb_recv,
    .flush = usb_flush,
    .get_stats = usb_get_stats,
    .destroy = usb_destroy
};