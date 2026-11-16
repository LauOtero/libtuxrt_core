#ifndef TUXRT_ACCEL_RUNTIME_H
#define TUXRT_ACCEL_RUNTIME_H

#include <stddef.h>

typedef struct {
    int (*find_command)(const char* data, size_t len, char cmd);
    size_t (*skip_whitespace)(const char* data, size_t len);
    // Extend with crypto, crc, etc.
} tuxrt_accel_ops_t;

__attribute__((visibility("default"))) void tuxrt_accel_runtime_init(void);
const tuxrt_accel_ops_t* tuxrt_accel_runtime_get(void);

#endif
