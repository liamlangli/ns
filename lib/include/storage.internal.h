#ifndef NS_STORAGE_INTERNAL_H
#define NS_STORAGE_INTERNAL_H

#include "storage.h"

#include <stddef.h>

#define STORAGE_PATH_CAPACITY 4096

void storage_clear_error(void);
void storage_set_error(const char *message);
void storage_set_errorf(const char *format, ...);
const char *storage_app_data_dir(void);
i32 storage_make_dirs(const char *path);

#endif
