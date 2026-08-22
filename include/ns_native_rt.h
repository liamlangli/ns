#pragma once

#include <stdint.h>

// Linear-memory runtime used by AArch64-compiled ns programs. Addresses are
// 32-bit offsets into a growable heap so SSA's Wasm32 field/array layout
// (pointer at +0, len at +4, cap at +8) is valid on a 64-bit host. The heap
// base is reserved once and committed in place under a lock so concurrent
// `ns_rt_alloc` calls from worker tasks cannot race or invalidate host
// pointers held by other threads.

void ns_rt_init(void);
void ns_rt_reset(void);
void ns_rt_set_strtab(const char **tab, const int32_t *lens, int n);

int64_t ns_rt_alloc(int64_t size);
int64_t ns_rt_clone(int64_t src, int64_t size);
int64_t ns_rt_load(int64_t addr, int64_t off, int64_t size);
void ns_rt_store(int64_t addr, int64_t off, int64_t val, int64_t size);
void ns_rt_copy(int64_t dst, int64_t doff, int64_t src, int64_t size);

int64_t ns_rt_array_new(int64_t count, int64_t stride);
void ns_rt_array_store(int64_t arr, int64_t idx, int64_t val, int64_t stride);
int64_t ns_rt_array_index(int64_t arr, int64_t idx, int64_t stride);
int64_t ns_rt_array_slot(int64_t arr, int64_t idx, int64_t stride);

int64_t ns_rt_gget(int64_t idx);
void ns_rt_gset(int64_t idx, int64_t val);

int64_t ns_rt_intern(int64_t id);
int64_t ns_rt_from_bytes(const char *s, int32_t len);
int64_t ns_rt_print(int64_t str);

int64_t ns_rt_strcat(int64_t a, int64_t b);
int64_t ns_rt_strcmp(int64_t a, int64_t b);
int64_t ns_rt_substr(int64_t str, int64_t start, int64_t len);
int64_t ns_rt_unescape(int64_t str);
int64_t ns_rt_utf8_len(int64_t str);
int64_t ns_rt_itos(int64_t v);
int64_t ns_rt_utos(int64_t v);
int64_t ns_rt_btos(int64_t v);
int64_t ns_rt_ftos(int64_t bits);
int64_t ns_rt_stof(int64_t str);

int64_t ns_rt_fmod(int64_t a_bits, int64_t b_bits);
int64_t ns_rt_fmodf(int64_t a_bits, int64_t b_bits);
int64_t ns_rt_sqrt(int64_t bits);
int64_t ns_rt_sin(int64_t bits);
int64_t ns_rt_cos(int64_t bits);
int64_t ns_rt_tan(int64_t bits);
int64_t ns_rt_atan2(int64_t y_bits, int64_t x_bits);

int64_t ns_rt_map_new(int64_t cap, int64_t flags);
int64_t ns_rt_map_get(int64_t map, int64_t key);
void ns_rt_map_set(int64_t map, int64_t key, int64_t val);
int64_t ns_rt_map_has(int64_t map, int64_t key);
int64_t ns_rt_map_insert(int64_t map, int64_t key);
int64_t ns_rt_map_remove(int64_t map, int64_t key);
int64_t ns_rt_map_slot_live(int64_t map, int64_t idx);
int64_t ns_rt_map_slot_key(int64_t map, int64_t idx);

int64_t ns_rt_open(int64_t path, int64_t mode);
int64_t ns_rt_read(int64_t fd);
int64_t ns_rt_write(int64_t fd, int64_t buf);
void ns_rt_close(int64_t fd);

int64_t ns_rt_union_new(int64_t tag, int64_t payload);
int64_t ns_rt_union_as(int64_t u, int64_t want_tag);

int64_t ns_rt_to_cstr(int64_t str);
int64_t ns_rt_from_cstr(int64_t ptr);
int64_t ns_rt_native_ptr(int64_t addr);
int64_t ns_rt_array_ptr(int64_t arr);
int64_t ns_rt_callback(int64_t fnval);

int64_t ns_rt_queue_main(void);
int64_t ns_rt_queue_worker(void);
int64_t ns_rt_queue_idle(void);
int64_t ns_rt_task_spawn(int64_t fn, int64_t env, int64_t nargs,
                         int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                         int64_t queue);
int64_t ns_rt_task_dispatch(int64_t queue, int64_t fnval);
int64_t ns_rt_task_await(int64_t t);
void ns_rt_task_wait(int64_t t);
void ns_rt_task_cancel(int64_t t);
int64_t ns_rt_task_done(int64_t t);
int64_t ns_rt_task_cancelled(int64_t t);
void ns_rt_task_sleep(int64_t ms);
