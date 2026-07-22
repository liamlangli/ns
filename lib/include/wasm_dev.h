#ifndef NS_WASM_DEV_H
#define NS_WASM_DEV_H

#include <stdint.h>

int wasm_dev_listen(int port);
int wasm_dev_bound_port(int fd);
int wasm_dev_accept(int fd);
int wasm_dev_recv_request(int fd);
int wasm_dev_is_reload(void);
int wasm_dev_serve_static(int fd, const char *root);
int wasm_dev_upgrade(int fd);
int wasm_dev_send_text(int fd, const char *text);
int wasm_dev_close(int fd);
uint64_t wasm_dev_fingerprint(const char *root);
int wasm_dev_rebuild(const char *root);

#endif
