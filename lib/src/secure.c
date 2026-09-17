#include "secure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincred.h>
typedef SOCKET secure_socket;
#define close_socket closesocket
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
typedef int secure_socket;
#define close_socket close
#endif
#ifdef __APPLE__
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#else
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#endif
#define SECURE_CAP 128
#define FRAME_CAP 65536
#define OUTPUT_CAP (FRAME_CAP * 4)
typedef struct {
    int used, listener, ready, failed, header_n, input_n, input_size, output_n, output_pos, write_pending;
    secure_socket fd;
    time_t started;
    unsigned char header[4];
    char input[FRAME_CAP + 1], output[OUTPUT_CAP];
#ifdef __APPLE__
    SSLContextRef tls;
    CFArrayRef identities;
#else
    SSL_CTX *context;
    SSL *tls;
#endif
} secure_connection;
static secure_connection connections[SECURE_CAP];
static _Thread_local char result[4096];
static int allocate_connection(void) {
    for (int i = 1; i < SECURE_CAP; i++) if (!connections[i].used) {
        memset(&connections[i], 0, sizeof(connections[i]));
        connections[i].used = 1;
        connections[i].fd = (secure_socket)-1;
        connections[i].started = time(NULL);
        return i;
    }
    return -1;
}
static secure_connection *connection(int id) {
    return id > 0 && id < SECURE_CAP && connections[id].used ? &connections[id] : NULL;
}
static void nonblocking(secure_socket fd) {
#ifdef _WIN32
    u_long value = 1;
    ioctlsocket(fd, FIONBIO, &value);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int value = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#endif
#endif
}
static bool pending_socket(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}
#ifdef __APPLE__
static OSStatus apple_read(SSLConnectionRef ref, void *data, size_t *size) {
    secure_connection *c = (secure_connection *)ref;
    ssize_t n = recv(c->fd, data, *size, 0);
    if (n > 0) { *size = (size_t)n; return noErr; }
    *size = 0;
    return n == 0 ? errSSLClosedGraceful : pending_socket() ? errSSLWouldBlock : errSSLClosedAbort;
}
static OSStatus apple_write(SSLConnectionRef ref, const void *data, size_t *size) {
    secure_connection *c = (secure_connection *)ref;
    ssize_t n = send(c->fd, data, *size, 0);
    if (n >= 0) { *size = (size_t)n; return noErr; }
    *size = 0;
    return pending_socket() ? errSSLWouldBlock : errSSLClosedAbort;
}
#endif
void secure_close(int id) {
    secure_connection *c = connection(id);
    if (!c) return;
#ifdef __APPLE__
    if (c->tls) CFRelease(c->tls);
    if (c->identities) CFRelease(c->identities);
#else
    if (c->tls) SSL_free(c->tls);
    if (c->context) SSL_CTX_free(c->context);
#endif
    if (c->fd != (secure_socket)-1) close_socket(c->fd);
    memset(c, 0, sizeof(*c));
}
static bool configure_tls(secure_connection *c, secure_connection *listener, const char *host, const char *ca) {
#ifdef __APPLE__
    c->tls = SSLCreateContext(NULL, listener ? kSSLServerSide : kSSLClientSide, kSSLStreamType);
    if (!c->tls) return false;
    if (SSLSetProtocolVersionMin(c->tls, kTLSProtocol12) != noErr || SSLSetIOFuncs(c->tls, apple_read, apple_write) != noErr || SSLSetConnection(c->tls, c) != noErr) return false;
    if (listener) return SSLSetCertificate(c->tls, listener->identities) == noErr;
    // SecureTransport performs system trust and hostname verification itself.
    // Custom CA files are intentionally not a bypass on Apple: install a trusted CA.
    (void)ca;
    return SSLSetPeerDomainName(c->tls, host, strlen(host)) == noErr;
#else
    if (listener) {
        c->tls = SSL_new(listener->context);
    } else {
        c->context = SSL_CTX_new(TLS_client_method());
        if (!c->context) return false;
        SSL_CTX_set_min_proto_version(c->context, TLS1_2_VERSION);
        SSL_CTX_set_verify(c->context, SSL_VERIFY_PEER, NULL);
        if (ca && *ca) {
            if (SSL_CTX_load_verify_locations(c->context, ca, NULL) != 1) return false;
        } else if (SSL_CTX_set_default_verify_paths(c->context) != 1) return false;
        c->tls = SSL_new(c->context);
    }
    if (!c->tls || SSL_set_fd(c->tls, (int)c->fd) != 1) return false;
    if (listener) SSL_set_accept_state(c->tls);
    else {
        if (SSL_set1_host(c->tls, host) != 1 || SSL_set_tlsext_host_name(c->tls, host) != 1) return false;
        SSL_set_connect_state(c->tls);
    }
    return true;
#endif
}
int secure_listen(int port, const char *cert, const char *key) {
    if (port < 1 || port > 65535) return -1;
#ifdef _WIN32
    WSADATA data; if (WSAStartup(MAKEWORD(2,2), &data)) return -1;
#endif
    int id = allocate_connection();
    if (id < 0) return -1;
    secure_connection *c = connection(id);
    c->listener = 1;
#ifdef __APPLE__
    FILE *f = fopen(cert, "rb");
    if (!f) goto fail;
    unsigned char bytes[65536]; size_t n = fread(bytes, 1, sizeof(bytes), f); fclose(f);
    CFDataRef blob = CFDataCreate(NULL, bytes, n);
    CFStringRef password = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    const void *keys[] = {kSecImportExportPassphrase}; const void *values[] = {password};
    CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef items = NULL;
    OSStatus status = SecPKCS12Import(blob, options, &items);
    CFRelease(blob); CFRelease(password); CFRelease(options);
    if (status != errSecSuccess || !items || CFArrayGetCount(items) < 1) { if (items) CFRelease(items); goto fail; }
    CFDictionaryRef item = CFArrayGetValueAtIndex(items, 0);
    SecIdentityRef identity = (SecIdentityRef)CFDictionaryGetValue(item, kSecImportItemIdentity);
    const void *identity_values[] = {identity};
    c->identities = CFArrayCreate(NULL, identity_values, 1, &kCFTypeArrayCallBacks);
    CFRelease(items);
#else
    c->context = SSL_CTX_new(TLS_server_method());
    if (!c->context) goto fail;
    SSL_CTX_set_min_proto_version(c->context, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_chain_file(c->context, cert) != 1 || SSL_CTX_use_PrivateKey_file(c->context, key, SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(c->context) != 1) goto fail;
#endif
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd == (secure_socket)-1) goto fail;
    int reuse = 1;
    setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons((uint16_t)port); address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(c->fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(c->fd, 64) != 0) goto fail;
    nonblocking(c->fd);
    return id;
fail:
    secure_close(id); return -1;
}
int secure_accept(int id) {
    secure_connection *listener = connection(id);
    if (!listener || !listener->listener) return -1;
    secure_socket fd = accept(listener->fd, NULL, NULL);
    if (fd == (secure_socket)-1) return 0;
    int client = allocate_connection();
    if (client < 0) { close_socket(fd); return -1; }
    secure_connection *c = connection(client); c->fd = fd;
    nonblocking(fd);
    if (!configure_tls(c, listener, NULL, NULL)) { secure_close(client); return -1; }
    return client;
}
int secure_connect(const char *host, int port, const char *ca) {
    if (!host || !*host || port < 1 || port > 65535) return -1;
#ifdef _WIN32
    WSADATA data; if (WSAStartup(MAKEWORD(2,2), &data)) return -1;
#endif
    char service[8]; snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints = {0}, *addresses = NULL; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, service, &hints, &addresses) != 0) return -1;
    int id = allocate_connection();
    if (id < 0) { freeaddrinfo(addresses); return -1; }
    secure_connection *c = connection(id);
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        c->fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c->fd == (secure_socket)-1) continue;
        nonblocking(c->fd);
        if (connect(c->fd, a->ai_addr, (int)a->ai_addrlen) == 0 || pending_socket()) break;
        close_socket(c->fd); c->fd = (secure_socket)-1;
    }
    freeaddrinfo(addresses);
    if (c->fd == (secure_socket)-1 || !configure_tls(c, NULL, host, ca)) { secure_close(id); return -1; }
    return id;
}
static int tls_transfer(secure_connection *c, void *buffer, int length, bool writing) {
#ifdef __APPLE__
    size_t done = 0;
    OSStatus status = writing ? SSLWrite(c->tls, buffer, (size_t)length, &done) : SSLRead(c->tls, buffer, (size_t)length, &done);
    if (done > 0) return (int)done;
    return status == errSSLWouldBlock ? 0 : -1;
#else
    int n = writing ? SSL_write(c->tls, buffer, length) : SSL_read(c->tls, buffer, length);
    if (n > 0) return n;
    int e = SSL_get_error(c->tls, n);
    return e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE ? 0 : -1;
#endif
}
int secure_poll(int id) {
    secure_connection *c = connection(id);
    if (!c || c->failed || c->listener) return -1;
    if (!c->ready) {
        if (time(NULL) - c->started > 10) { c->failed = 1; return -1; }
#ifdef __APPLE__
        OSStatus status = SSLHandshake(c->tls);
        if (status == errSSLWouldBlock) return 0;
        if (status != noErr) { c->failed = 1; return -1; }
#else
        int n = SSL_do_handshake(c->tls);
        if (n != 1) {
            int e = SSL_get_error(c->tls, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
            c->failed = 1; return -1;
        }
#endif
        c->ready = 1;
    }
    if (c->output_pos < c->output_n) {
        if (!c->write_pending) c->write_pending = c->output_n - c->output_pos;
        int n = tls_transfer(c, c->output + c->output_pos, c->write_pending, true);
        if (n < 0) { c->failed = 1; return -1; }
        c->output_pos += n;
        if (n > 0) c->write_pending = 0;
        if (c->output_pos == c->output_n) c->output_pos = c->output_n = 0;
    }
    return 1;
}
int secure_send(int id, const char *message) {
    secure_connection *c = connection(id);
    if (!c || c->failed || !message) return false;
    size_t n = strlen(message);
    if (n == 0 || n > FRAME_CAP || (size_t)c->output_n + n + 4 > OUTPUT_CAP) return false;
    unsigned char header[] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n};
    memcpy(c->output + c->output_n, header, 4); c->output_n += 4;
    memcpy(c->output + c->output_n, message, n); c->output_n += (int)n;
    return true;
}
int secure_receive(int id) {
    secure_connection *c = connection(id);
    int status = secure_poll(id);
    if (status != 1) return status;
    if (c->input_size > 0 && c->input_n == c->input_size) c->header_n = c->input_n = c->input_size = 0;
    if (c->header_n < 4) {
        int n = tls_transfer(c, c->header + c->header_n, 4 - c->header_n, false);
        if (n < 0) { c->failed = 1; return -1; }
        c->header_n += n;
        if (c->header_n < 4) return 0;
        uint32_t size = ((uint32_t)c->header[0] << 24) | ((uint32_t)c->header[1] << 16) | ((uint32_t)c->header[2] << 8) | c->header[3];
        if (size == 0 || size > FRAME_CAP) { c->failed = 1; return -1; }
        c->input_size = (int)size;
    }
    int n = tls_transfer(c, c->input + c->input_n, c->input_size - c->input_n, false);
    if (n < 0) { c->failed = 1; return -1; }
    c->input_n += n;
    if (c->input_n != c->input_size) return 0;
    if (memchr(c->input, 0, c->input_n)) { c->failed = 1; return -1; }
    c->input[c->input_n] = 0;
    return 1;
}
const char *secure_message(int id) {
    secure_connection *c = connection(id);
    return c && c->input_n == c->input_size && c->input_size > 0 ? c->input : "";
}
static bool random_bytes(unsigned char *bytes, int size) {
#ifdef __APPLE__
    return SecRandomCopyBytes(kSecRandomDefault, (size_t)size, bytes) == errSecSuccess;
#else
    return RAND_bytes(bytes, size) == 1;
#endif
}
static void hex_encode(const unsigned char *data, int size, char *out) {
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < size; i++) { out[i * 2] = digits[data[i] >> 4]; out[i * 2 + 1] = digits[data[i] & 15]; }
    out[size * 2] = 0;
}
static bool hex_decode(const char *text, int size, unsigned char *out) {
    for (int i = 0; i < size; i++) {
        int a = text[2*i], b = text[2*i+1];
        a = a >= '0' && a <= '9' ? a-'0' : a >= 'a' && a <= 'f' ? a-'a'+10 : -1;
        b = b >= '0' && b <= '9' ? b-'0' : b >= 'a' && b <= 'f' ? b-'a'+10 : -1;
        if (a < 0 || b < 0) return false;
        out[i] = (unsigned char)(a*16+b);
    }
    return true;
}
const char *secure_random_hex(int bytes) {
    unsigned char data[128]; result[0] = 0;
    if (bytes < 1 || bytes > 128 || !random_bytes(data, bytes)) return result;
    hex_encode(data, bytes, result); return result;
}
static bool derive(const char *password, const unsigned char *salt, unsigned char *out) {
    size_t n = strlen(password);
    if (n < 8 || n > 256) return false;
#ifdef __APPLE__
    return CCKeyDerivationPBKDF(kCCPBKDF2, password, n, salt, 16, kCCPRFHmacAlgSHA256, 600000, out, 32) == 0;
#else
    return PKCS5_PBKDF2_HMAC(password, (int)n, salt, 16, 600000, EVP_sha256(), 32, out) == 1;
#endif
}
const char *secure_password_hash(const char *password) {
    unsigned char salt[16], hash[32]; result[0] = 0;
    if (!random_bytes(salt, 16) || !derive(password, salt, hash)) return result;
    memcpy(result, "p1$", 3); hex_encode(salt, 16, result+3); result[35] = '$'; hex_encode(hash, 32, result+36);
    return result;
}
int secure_password_verify(const char *password, const char *encoded) {
    if (!encoded || strlen(encoded) != 100 || strncmp(encoded, "p1$", 3) || encoded[35] != '$') return false;
    unsigned char salt[16], expected[32], actual[32];
    if (!hex_decode(encoded+3,16,salt) || !hex_decode(encoded+36,32,expected) || !derive(password,salt,actual)) return false;
    volatile unsigned int difference = 0;
    for (int i = 0; i < 32; i++) difference |= actual[i] ^ expected[i];
    return difference == 0;
}
#ifdef __APPLE__
static CFMutableDictionaryRef credential_query(const char *key) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef account = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("org.nanoscript.evolution-island"));
    CFDictionarySetValue(q, kSecAttrAccount, account); CFRelease(account); return q;
}
int secure_credential_set(const char *key, const char *value) {
    CFMutableDictionaryRef q = credential_query(key);
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)value, strlen(value));
    const void *keys[] = {kSecValueData}; const void *values[] = {data};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus s = SecItemUpdate(q, attrs);
    if (s == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        s = SecItemAdd(q, NULL);
    }
    CFRelease(q); CFRelease(data); CFRelease(attrs); return s == errSecSuccess;
}
const char *secure_credential_get(const char *key) {
    result[0] = 0; CFMutableDictionaryRef q = credential_query(key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDataRef data = NULL;
    if (SecItemCopyMatching(q, (CFTypeRef *)&data) == errSecSuccess) {
        CFIndex n = CFDataGetLength(data);
        if (n < (CFIndex)sizeof(result)) { memcpy(result, CFDataGetBytePtr(data), n); result[n] = 0; }
        CFRelease(data);
    }
    CFRelease(q); return result;
}
int secure_credential_delete(const char *key) {
    CFMutableDictionaryRef q = credential_query(key); OSStatus s = SecItemDelete(q); CFRelease(q);
    return s == errSecSuccess || s == errSecItemNotFound;
}
#elif defined(_WIN32)
int secure_credential_set(const char *key, const char *value) {
    CREDENTIALA c = {0}; c.Type = CRED_TYPE_GENERIC; c.TargetName = (char *)key;
    c.CredentialBlobSize = (DWORD)strlen(value); c.CredentialBlob = (LPBYTE)value; c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteA(&c, 0) != 0;
}
const char *secure_credential_get(const char *key) {
    PCREDENTIALA c = NULL; result[0] = 0;
    if (CredReadA(key, CRED_TYPE_GENERIC, 0, &c)) {
        if (c->CredentialBlobSize < sizeof(result)) { memcpy(result,c->CredentialBlob,c->CredentialBlobSize); result[c->CredentialBlobSize] = 0; }
        CredFree(c);
    }
    return result;
}
int secure_credential_delete(const char *key) { return CredDeleteA(key,CRED_TYPE_GENERIC,0) || GetLastError() == ERROR_NOT_FOUND; }
#else
// Keep Secret Service optional on headless Linux. ABI mirrors libsecret's public schema.
typedef struct { const char *name; int type; } secret_attribute;
typedef struct { const char *name; int flags; secret_attribute attributes[32]; int reserved; void *padding[7]; } secret_schema;
static const secret_schema schema = {.name="org.nanoscript.evolution-island", .attributes={{"key",0},{NULL,0}}};
static void *secret_library(void) { static void *lib; if (!lib) lib = dlopen("libsecret-1.so.0",RTLD_NOW|RTLD_LOCAL); return lib; }
int secure_credential_set(const char *key, const char *value) {
    void *lib = secret_library(); if (!lib) return false;
    int (*fn)(const secret_schema*,const char*,const char*,const char*,void*,void*,...) = dlsym(lib,"secret_password_store_sync");
    return fn && fn(&schema,"default","Evolution Island",value,NULL,NULL,"key",key,NULL);
}
const char *secure_credential_get(const char *key) {
    result[0] = 0; void *lib = secret_library(); if (!lib) return result;
    char *(*fn)(const secret_schema*,void*,void*,...) = dlsym(lib,"secret_password_lookup_sync");
    void (*release)(char*) = dlsym(lib,"secret_password_free");
    if (!fn || !release) return result;
    char *value = fn(&schema,NULL,NULL,"key",key,NULL);
    if (value) { if (strlen(value)<sizeof(result)) strcpy(result,value); release(value); }
    return result;
}
int secure_credential_delete(const char *key) {
    void *lib = secret_library(); if (!lib) return false;
    int (*fn)(const secret_schema*,void*,void*,...) = dlsym(lib,"secret_password_clear_sync");
    return fn && fn(&schema,NULL,NULL,"key",key,NULL);
}
#endif
