#include "wasm_dev.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NS_WIN
#include <strings.h>
#endif
#include <sys/stat.h>

#ifdef NS_WIN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define close_socket closesocket
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define close_socket close
#endif

#define REQUEST_CAP 16384
#define PATH_CAP 4096
static char request_buf[REQUEST_CAP];
static char request_path[PATH_CAP];
static char websocket_key[256];
static int request_head;
static int request_reload;

static int socket_startup(void) {
#ifdef NS_WIN
    static int started;
    if (!started) { WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1; started = 1; }
#endif
    return 0;
}

static int safe_send(int fd, const void *data, size_t len) {
    const char *bytes = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
#ifdef NS_WIN
        int n = send(fd, bytes + sent, (int)(len - sent), 0);
#elif defined(MSG_NOSIGNAL)
        ssize_t n = send(fd, bytes + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, bytes + sent, len - sent, 0);
#endif
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int wasm_dev_listen(int port) {
    if (socket_startup() != 0) return -1;
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&yes, sizeof(yes));
#endif
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 64) != 0) {
        close_socket(fd); return -1;
    }
#ifdef NS_WIN
    u_long nonblocking = 1; ioctlsocket(fd, FIONBIO, &nonblocking);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    return fd;
}

int wasm_dev_bound_port(int fd) {
    struct sockaddr_in address; socklen_t len = sizeof(address);
    return getsockname(fd, (struct sockaddr *)&address, &len) == 0 ? ntohs(address.sin_port) : -1;
}

int wasm_dev_accept(int server) {
    struct sockaddr_in address; socklen_t len = sizeof(address);
    int fd = (int)accept(server, (struct sockaddr *)&address, &len);
    if (fd < 0) return -1;
#ifdef NS_WIN
    u_long blocking = 0; ioctlsocket(fd, FIONBIO, &blocking);
    DWORD timeout = 1000; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    int flags = fcntl(fd, F_GETFL, 0); if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval timeout = {.tv_sec = 1}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
#endif
    return fd;
}

static int header_value(const char *name, char *output, size_t capacity) {
    size_t name_len = strlen(name);
    char *line = strstr(request_buf, "\r\n") + 2;
    while (line && *line && strncmp(line, "\r\n", 2) != 0) {
        char *end = strstr(line, "\r\n");
        if (!end) break;
        if ((size_t)(end - line) > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            char *value = line + name_len + 1; while (value < end && isspace((unsigned char)*value)) value++;
            size_t length = (size_t)(end - value); if (length >= capacity) length = capacity - 1;
            memcpy(output, value, length); output[length] = '\0'; return 1;
        }
        line = end + 2;
    }
    return 0;
}

static int header_has_token(const char *value, const char *token) {
    size_t token_len = strlen(token);
    const char *p = value;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *end = p;
        while (*end && *end != ',') end++;
        const char *trim = end;
        while (trim > p && isspace((unsigned char)trim[-1])) trim--;
        if ((size_t)(trim - p) == token_len && strncasecmp(p, token, token_len) == 0) return 1;
        p = end;
    }
    return 0;
}

static int decode_path(const char *source, int length) {
    int out = 0;
    for (int i = 0; i < length && out < PATH_CAP - 1; ++i) {
        if (source[i] == '?') break;
        if (source[i] == '%' && i + 2 < length && isxdigit((unsigned char)source[i + 1]) && isxdigit((unsigned char)source[i + 2])) {
            char hex[3] = {source[i + 1], source[i + 2], 0};
            request_path[out++] = (char)strtol(hex, NULL, 16); i += 2;
        } else request_path[out++] = source[i];
    }
    request_path[out] = '\0';
    return out;
}

int wasm_dev_recv_request(int fd) {
    int total = 0;
    memset(request_buf, 0, sizeof(request_buf));
    request_path[0] = websocket_key[0] = '\0'; request_head = request_reload = 0;
    while (total < REQUEST_CAP - 1) {
        int n = (int)recv(fd, request_buf + total, REQUEST_CAP - 1 - total, 0);
        if (n <= 0) return -1;
        total += n; request_buf[total] = '\0';
        if (strstr(request_buf, "\r\n\r\n")) break;
    }
    if (!strstr(request_buf, "\r\n\r\n")) return -1;
    char *space = strchr(request_buf, ' '); if (!space) return -1;
    request_head = (space - request_buf == 4 && memcmp(request_buf, "HEAD", 4) == 0);
    if (!request_head && !(space - request_buf == 3 && memcmp(request_buf, "GET", 3) == 0)) return -1;
    char *target = space + 1, *target_end = strchr(target, ' '); if (!target_end) return -1;
    decode_path(target, (int)(target_end - target));
    char upgrade[64], connection[128], version[16];
    int has_upgrade = header_value("Upgrade", upgrade, sizeof(upgrade));
    int has_connection = header_value("Connection", connection, sizeof(connection));
    int has_version = header_value("Sec-WebSocket-Version", version, sizeof(version));
    int has_key = header_value("Sec-WebSocket-Key", websocket_key, sizeof(websocket_key));
    request_reload = !request_head && strcmp(request_path, "/__ns/reload") == 0 && has_upgrade &&
        strcasecmp(upgrade, "websocket") == 0 && has_connection && header_has_token(connection, "upgrade") &&
        has_version && strcmp(version, "13") == 0 && has_key;
    return 0;
}

int wasm_dev_is_reload(void) { return request_reload; }

static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".wasm") == 0) return "application/wasm";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".webp") == 0) return "image/webp";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

static int send_status(int fd, int status, const char *reason) {
    char header[512];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status, reason);
    return safe_send(fd, header, (size_t)n);
}

int wasm_dev_serve_static(int fd, const char *root) {
    if (strstr(request_path, "..") || strchr(request_path, '\\') || request_path[0] != '/') return send_status(fd, 403, "Forbidden");
    const char *relative = strcmp(request_path, "/") == 0 ? "/index.html" : request_path;
    char path[PATH_CAP * 2];
    if (snprintf(path, sizeof(path), "%s%s", root, relative) >= (int)sizeof(path)) return send_status(fd, 414, "URI Too Long");
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return send_status(fd, 404, "Not Found");
    FILE *file = fopen(path, "rb"); if (!file) return send_status(fd, 404, "Not Found");
    char header[1024];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", mime_type(path), (long long)st.st_size);
    int result = safe_send(fd, header, (size_t)n);
    if (!request_head && result == 0) {
        char bytes[32768]; size_t count;
        while ((count = fread(bytes, 1, sizeof(bytes), file)) > 0) if (safe_send(fd, bytes, count) != 0) { result = -1; break; }
    }
    fclose(file); return result;
}

typedef struct { unsigned int h[5]; unsigned long long bytes; unsigned char block[64]; unsigned int used; } sha1_ctx;
static unsigned int rol(unsigned int x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1_block(sha1_ctx *c) {
    unsigned int w[80];
    for (int i = 0; i < 16; ++i) w[i] = ((unsigned)c->block[i*4]<<24)|((unsigned)c->block[i*4+1]<<16)|((unsigned)c->block[i*4+2]<<8)|c->block[i*4+3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    unsigned int a=c->h[0],b=c->h[1],d=c->h[3],e=c->h[4],f,k,t,cc=c->h[2];
    for(int i=0;i<80;++i){if(i<20){f=(b&cc)|((~b)&d);k=0x5a827999;}else if(i<40){f=b^cc^d;k=0x6ed9eba1;}else if(i<60){f=(b&cc)|(b&d)|(cc&d);k=0x8f1bbcdc;}else{f=b^cc^d;k=0xca62c1d6;}t=rol(a,5)+f+e+k+w[i];e=d;d=cc;cc=rol(b,30);b=a;a=t;}
    c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;c->used=0;
}
static void sha1_update(sha1_ctx *c,const unsigned char *p,size_t n){c->bytes+=n;while(n--){c->block[c->used++]=*p++;if(c->used==64)sha1_block(c);}}
static void sha1_final(sha1_ctx *c,unsigned char out[20]){unsigned long long bits=c->bytes*8;c->block[c->used++]=0x80;if(c->used>56){while(c->used<64)c->block[c->used++]=0;sha1_block(c);}while(c->used<56)c->block[c->used++]=0;for(int i=7;i>=0;--i)c->block[c->used++]=(unsigned char)(bits>>(i*8));sha1_block(c);for(int i=0;i<5;++i){out[i*4]=c->h[i]>>24;out[i*4+1]=c->h[i]>>16;out[i*4+2]=c->h[i]>>8;out[i*4+3]=c->h[i];}}
static void base64(const unsigned char *in, int len, char *out) { static const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; int o=0; for(int i=0;i<len;i+=3){unsigned v=(unsigned)in[i]<<16;int n=len-i;if(n>1)v|=(unsigned)in[i+1]<<8;if(n>2)v|=in[i+2];out[o++]=table[(v>>18)&63];out[o++]=table[(v>>12)&63];out[o++]=n>1?table[(v>>6)&63]:'=';out[o++]=n>2?table[v&63]:'=';}out[o]=0; }

int wasm_dev_upgrade(int fd) {
    char source[512], accept[64], response[512]; unsigned char digest[20];
    snprintf(source, sizeof(source), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", websocket_key);
    sha1_ctx c={{0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0},0,{0},0}; sha1_update(&c,(unsigned char*)source,strlen(source));sha1_final(&c,digest);base64(digest,20,accept);
    int n=snprintf(response,sizeof(response),"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",accept);
    return safe_send(fd,response,(size_t)n);
}

int wasm_dev_send_text(int fd, const char *text) {
    size_t len = strlen(text); unsigned char head[10]; int h = 0; head[h++] = 0x81;
    if (len < 126) head[h++] = (unsigned char)len;
    else if (len <= 65535) { head[h++] = 126; head[h++] = (unsigned char)(len >> 8); head[h++] = (unsigned char)len; }
    else return -1;
    return safe_send(fd, head, (size_t)h) == 0 ? safe_send(fd, text, len) : -1;
}

int wasm_dev_close(int fd) { return close_socket(fd); }

typedef struct { char *path; long long size; long long mtime; } fingerprint_file;
static int fingerprint_cmp(const void *a,const void *b){return strcmp(((const fingerprint_file*)a)->path,((const fingerprint_file*)b)->path);}
static int ignored_name(const char *name){return strcmp(name,"bin")==0||strcmp(name,".git")==0||strcmp(name,".hg")==0||strcmp(name,".svn")==0;}
static void collect_files(const char *root,const char *relative,fingerprint_file **files,int *count,int *cap){
    char dirpath[PATH_CAP*2];snprintf(dirpath,sizeof(dirpath),"%s%s%s",root,relative[0]?"/":"",relative);
#ifdef NS_WIN
    char pattern[PATH_CAP*2];snprintf(pattern,sizeof(pattern),"%s\\*",dirpath);WIN32_FIND_DATAA d;HANDLE h=FindFirstFileA(pattern,&d);if(h==INVALID_HANDLE_VALUE)return;do{const char*n=d.cFileName;if(strcmp(n,".")==0||strcmp(n,"..")==0||ignored_name(n))continue;char rel[PATH_CAP];snprintf(rel,sizeof(rel),"%s%s%s",relative,relative[0]?"/":"",n);if(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)collect_files(root,rel,files,count,cap);else{struct stat st;char full[PATH_CAP*2];snprintf(full,sizeof(full),"%s/%s",root,rel);if(stat(full,&st)==0){if(*count==*cap){*cap=*cap?*cap*2:128;*files=realloc(*files,(size_t)*cap*sizeof(**files));}(*files)[*count]=(fingerprint_file){_strdup(rel),st.st_size,st.st_mtime};(*count)++;}}}while(FindNextFileA(h,&d));FindClose(h);
#else
    DIR *dir=opendir(dirpath);if(!dir)return;struct dirent *d;while((d=readdir(dir))){const char*n=d->d_name;if(strcmp(n,".")==0||strcmp(n,"..")==0||ignored_name(n))continue;char rel[PATH_CAP];snprintf(rel,sizeof(rel),"%s%s%s",relative,relative[0]?"/":"",n);char full[PATH_CAP*2];snprintf(full,sizeof(full),"%s/%s",root,rel);struct stat st;if(stat(full,&st)!=0)continue;if(S_ISDIR(st.st_mode))collect_files(root,rel,files,count,cap);else if(S_ISREG(st.st_mode)){if(*count==*cap){*cap=*cap?*cap*2:128;*files=realloc(*files,(size_t)*cap*sizeof(**files));}long long mt=(long long)st.st_mtime*1000000000ll;
#if defined(__APPLE__)
    mt+=(long long)st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    mt+=(long long)st.st_mtim.tv_nsec;
#endif
    (*files)[*count]=(fingerprint_file){strdup(rel),st.st_size,mt};(*count)++;}}closedir(dir);
#endif
}

uint64_t wasm_dev_fingerprint(const char *root) { fingerprint_file *files=NULL;int count=0,cap=0;collect_files(root,"",&files,&count,&cap);qsort(files,(size_t)count,sizeof(*files),fingerprint_cmp);uint64_t h=1469598103934665603ull;for(int i=0;i<count;++i){for(const unsigned char*p=(unsigned char*)files[i].path;*p;++p){h^=*p;h*=1099511628211ull;}h^=(uint64_t)files[i].size;h*=1099511628211ull;h^=(uint64_t)files[i].mtime;h*=1099511628211ull;free(files[i].path);}free(files);return h; }

int wasm_dev_rebuild(const char *root) {
    const char *executable = getenv("NS_EXECUTABLE"); if (!executable || !*executable) return 0;
#ifdef NS_WIN
    char command[PATH_CAP*3];snprintf(command,sizeof(command),"\"%s\" build \"%s\"",executable,root);return system(command)==0;
#else
    pid_t child=fork();if(child<0)return 0;if(child==0){execl(executable,executable,"build",root,(char*)NULL);_exit(127);}int status=0;while(waitpid(child,&status,0)<0&&errno==EINTR){}return WIFEXITED(status)&&WEXITSTATUS(status)==0;
#endif
}
