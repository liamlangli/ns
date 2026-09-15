#pragma once
#include "ns_type.h"
typedef struct camera_frame {
    i32 width, height;
    i64 sequence;
    f64 timestamp;
} camera_frame;
i32 camera_permission(void);
void camera_request_permission(void);
ns_bool camera_open(i32 position, i32 fps);
void camera_close(void);
i32 camera_status(void);
ns_bool camera_read_rgba(u8 *data, i32 capacity, camera_frame *info);
ns_bool camera_record_start(const char *path);
void camera_record_stop(void);
i32 camera_record_status(void);
f64 camera_record_duration(void);
const char *camera_last_error(void);
