#ifndef NS_SECURE_H
#define NS_SECURE_H
#include <stdbool.h>
int secure_listen(int port, const char *certificate, const char *key);
int secure_accept(int listener);
int secure_connect(const char *host, int port, const char *ca_file);
int secure_poll(int handle);
int secure_send(int handle, const char *message);
int secure_receive(int handle);
const char *secure_message(int handle);
void secure_close(int handle);
const char *secure_random_hex(int bytes);
const char *secure_password_hash(const char *password);
int secure_password_verify(const char *password, const char *encoded);
int secure_credential_set(const char *key, const char *value);
const char *secure_credential_get(const char *key);
int secure_credential_delete(const char *key);
#endif
