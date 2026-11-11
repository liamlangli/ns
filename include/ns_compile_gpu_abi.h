#pragma once
#include <stdint.h>

// Versioned, pointer-free records shared with the optional Metal service.
// Values are module-global indices; block and at are function-local indices.
typedef struct ns_compile_gpu_event {
    int32_t value, block, at, definition;
} ns_compile_gpu_event;

typedef int (*ns_compile_gpu_run_v1)(const ns_compile_gpu_event *events,
    uint32_t count, uint32_t values, int32_t *last, double *device_ms,
    char *error, uint32_t error_size);
