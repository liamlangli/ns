#include "ns_profile_live.h"

// `ns profiler --device`: find a device app waiting in ns_profile_live_serve
// and relay its stream to the viewer.
//
// The viewer always starts `ns profile --live-port <n>` and waits for that
// process to connect. With NS_PROFILE_DEVICE in the environment the process
// does not run a program: it connects to the device instead and copies bytes
// both ways, so the viewer, its protocol and its Stop button are unchanged.
//
// Two routes reach a device:
//   - USB, through usbmuxd: the daemon that iTunes / Finder / Xcode use. A
//     Connect request tunnels a socket to a TCP port on the device's loopback,
//     so no network, pairing prompt or local-network permission is involved.
//   - the local network, through the Bonjour service the app advertises
//     (NS_PROFILE_LIVE_SERVICE); the first connection asks the user for the
//     app's local-network permission on the device.
// A cable wins when both are available: it does not share the radio with the
// camera stream or anything else the app does.

#if defined(_WIN32)
i32 ns_profile_device_list(void) {
    ns_warn("profile", "device profiling is not supported on Windows.\n");
    return 1;
}

i32 ns_profile_device_relay(const char *selector, const char *host, i32 port) {
    (void)selector;
    (void)host;
    (void)port;
    ns_warn("profile", "device profiling is not supported on Windows.\n");
    return 1;
}
#else

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <dns_sd.h>
#endif

#define NS_DEVICE_MAX 16
#define NS_USBMUX_PATH "/var/run/usbmuxd"

typedef enum ns_device_route {
    NS_DEVICE_USB,
    NS_DEVICE_USBMUX_NETWORK, // paired for Wi-Fi sync; still reached via usbmuxd
    NS_DEVICE_BONJOUR,
} ns_device_route;

typedef struct ns_device {
    ns_device_route route;
    char name[128];    // Bonjour instance name, or the UDID for usbmuxd
    char app[128];     // app title from the TXT record, when advertised
    i32 mux_id;        // usbmuxd DeviceID
    char host[256];    // Bonjour host target
    i32 port;          // Bonjour port
} ns_device;

static ns_device ns_devices[NS_DEVICE_MAX];
static i32 ns_device_count = 0;

static const char *ns_device_route_name(ns_device_route route) {
    if (route == NS_DEVICE_USB) return "usb";
    if (route == NS_DEVICE_USBMUX_NETWORK) return "usbmux-wifi";
    return "wifi";
}

// ---------------------------------------------------------------------------
// usbmuxd

static ns_bool ns_io_all(int fd, void *data, szt len, ns_bool write_side) {
    u8 *p = (u8 *)data;
    while (len > 0) {
        ssize_t n = write_side ? write(fd, p, len) : read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= (szt)n;
    }
    return true;
}

static int ns_usbmux_open(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", NS_USBMUX_PATH);
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// One plist request and its reply. Header: u32 length (with header), version 1,
// message 8 (plist), tag; all little-endian.
static char *ns_usbmux_request(int fd, const char *body) {
    static u32 tag = 1;
    char plist[1024];
    int len = snprintf(plist, sizeof(plist),
                       "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                       "<plist version=\"1.0\"><dict>"
                       "<key>ClientVersionString</key><string>ns</string>"
                       "<key>ProgName</key><string>ns</string>%s</dict></plist>\n",
                       body);
    if (len <= 0 || len >= (int)sizeof(plist)) return ns_null;
    u32 header[4] = {(u32)(16 + len), 1, 8, tag++};
    if (!ns_io_all(fd, header, sizeof(header), true) || !ns_io_all(fd, plist, (szt)len, true)) return ns_null;
    u32 reply[4];
    if (!ns_io_all(fd, reply, sizeof(reply), false)) return ns_null;
    if (reply[0] < 16 || reply[0] > (1u << 24)) return ns_null;
    szt size = reply[0] - 16;
    char *text = (char *)malloc(size + 1);
    if (!text) return ns_null;
    if (!ns_io_all(fd, text, size, false)) {
        free(text);
        return ns_null;
    }
    text[size] = 0;
    return text;
}

// The value after `<key>key</key>` inside [from, to): an <integer> or <string>.
static ns_bool ns_plist_value(const char *from, const char *to, const char *key, char *out, szt cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);
    const char *at = strstr(from, pattern);
    if (!at || at >= to) return false;
    at += strlen(pattern);
    while (*at == ' ' || *at == '\n' || *at == '\t' || *at == '\r') at++;
    const char *open = strchr(at, '>');
    if (!open || open >= to) return false;
    open++;
    const char *close = strchr(open, '<');
    if (!close || close > to) return false;
    szt n = (szt)(close - open);
    if (n >= cap) n = cap - 1;
    memcpy(out, open, n);
    out[n] = 0;
    return true;
}

static void ns_usbmux_scan(void) {
    int fd = ns_usbmux_open();
    if (fd < 0) return;
    char *text = ns_usbmux_request(fd, "<key>MessageType</key><string>ListDevices</string>");
    close(fd);
    if (!text) return;
    // Each attached device carries a Properties dict with its connection type,
    // DeviceID and SerialNumber (the UDID).
    const char *mark = "<key>Properties</key>";
    const char *at = strstr(text, mark);
    while (at && ns_device_count < NS_DEVICE_MAX) {
        const char *next = strstr(at + 1, mark);
        const char *end = next ? next : text + strlen(text);
        char type[32] = {0}, id[32] = {0}, serial[128] = {0};
        if (ns_plist_value(at, end, "DeviceID", id, sizeof(id))) {
            ns_plist_value(at, end, "ConnectionType", type, sizeof(type));
            ns_plist_value(at, end, "SerialNumber", serial, sizeof(serial));
            ns_device *d = &ns_devices[ns_device_count++];
            memset(d, 0, sizeof(*d));
            d->route = strcmp(type, "USB") == 0 ? NS_DEVICE_USB : NS_DEVICE_USBMUX_NETWORK;
            d->mux_id = atoi(id);
            snprintf(d->name, sizeof(d->name), "%s", serial[0] ? serial : id);
        }
        at = next;
    }
    free(text);
}

// A socket tunnelled to `port` on the device's loopback, or -1.
static int ns_usbmux_connect(i32 device_id, i32 port) {
    int fd = ns_usbmux_open();
    if (fd < 0) return -1;
    char body[256];
    // usbmuxd wants the port in network byte order, written as an integer.
    snprintf(body, sizeof(body),
             "<key>MessageType</key><string>Connect</string>"
             "<key>DeviceID</key><integer>%d</integer>"
             "<key>PortNumber</key><integer>%u</integer>",
             device_id, (unsigned)htons((u16)port));
    char *text = ns_usbmux_request(fd, body);
    char number[16] = {0};
    ns_bool ok = text && ns_plist_value(text, text + strlen(text), "Number", number, sizeof(number)) &&
                 atoi(number) == 0;
    free(text);
    if (!ok) {
        close(fd);
        return -1;
    }
    struct timeval none = {0, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    return fd;
}

// ---------------------------------------------------------------------------
// Bonjour

#if defined(__APPLE__)
typedef struct ns_bonjour_found {
    char name[128];
    char type[64];
    char domain[64];
    u32 iface;
} ns_bonjour_found;

static ns_bonjour_found ns_bonjour_list[NS_DEVICE_MAX];
static i32 ns_bonjour_count = 0;
static ns_device *ns_bonjour_target = ns_null;
static ns_bool ns_bonjour_done = false;

static void DNSSD_API ns_bonjour_browsed(DNSServiceRef ref, DNSServiceFlags flags, u32 iface, DNSServiceErrorType err,
                                         const char *name, const char *type, const char *domain, void *ctx) {
    (void)ref;
    (void)ctx;
    if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd)) return;
    for (i32 i = 0; i < ns_bonjour_count; i++) {
        if (strcmp(ns_bonjour_list[i].name, name) == 0) return; // one entry per interface otherwise
    }
    if (ns_bonjour_count >= NS_DEVICE_MAX) return;
    ns_bonjour_found *f = &ns_bonjour_list[ns_bonjour_count++];
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(f->type, sizeof(f->type), "%s", type);
    snprintf(f->domain, sizeof(f->domain), "%s", domain);
    f->iface = iface;
}

static void DNSSD_API ns_bonjour_resolved(DNSServiceRef ref, DNSServiceFlags flags, u32 iface, DNSServiceErrorType err,
                                          const char *fullname, const char *host, u16 port, u16 txt_len,
                                          const unsigned char *txt, void *ctx) {
    (void)ref;
    (void)flags;
    (void)iface;
    (void)fullname;
    (void)ctx;
    ns_bonjour_done = true;
    if (err != kDNSServiceErr_NoError || !ns_bonjour_target) return;
    snprintf(ns_bonjour_target->host, sizeof(ns_bonjour_target->host), "%s", host);
    ns_bonjour_target->port = ntohs(port);
    u8 app_len = 0;
    const void *app = TXTRecordGetValuePtr(txt_len, txt, "app", &app_len);
    if (app && app_len > 0) {
        szt n = app_len < sizeof(ns_bonjour_target->app) - 1 ? app_len : sizeof(ns_bonjour_target->app) - 1;
        memcpy(ns_bonjour_target->app, app, n);
        ns_bonjour_target->app[n] = 0;
    }
}

// Run `ref` until `done` or `ms` pass.
static void ns_bonjour_pump(DNSServiceRef ref, i32 ms, ns_bool *done) {
    int fd = DNSServiceRefSockFD(ref);
    f64 end = ns_profile_now_ms() + ms;
    for (;;) {
        f64 left = end - ns_profile_now_ms();
        if (left <= 0.0 || (done && *done)) return;
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, (int)left) <= 0) return;
        if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) return;
    }
}

static void ns_bonjour_scan(i32 ms) {
    ns_bonjour_count = 0;
    DNSServiceRef browse = ns_null;
    if (DNSServiceBrowse(&browse, 0, 0, NS_PROFILE_LIVE_SERVICE, ns_null, ns_bonjour_browsed, ns_null) !=
        kDNSServiceErr_NoError) {
        return;
    }
    ns_bonjour_pump(browse, ms, ns_null);
    DNSServiceRefDeallocate(browse);
    for (i32 i = 0; i < ns_bonjour_count && ns_device_count < NS_DEVICE_MAX; i++) {
        ns_bonjour_found *f = &ns_bonjour_list[i];
        ns_device *d = &ns_devices[ns_device_count];
        memset(d, 0, sizeof(*d));
        d->route = NS_DEVICE_BONJOUR;
        snprintf(d->name, sizeof(d->name), "%s", f->name);
        DNSServiceRef resolve = ns_null;
        ns_bonjour_target = d;
        ns_bonjour_done = false;
        if (DNSServiceResolve(&resolve, 0, f->iface, f->name, f->type, f->domain, ns_bonjour_resolved, ns_null) ==
            kDNSServiceErr_NoError) {
            ns_bonjour_pump(resolve, 1500, &ns_bonjour_done);
            DNSServiceRefDeallocate(resolve);
        }
        ns_bonjour_target = ns_null;
        if (d->port > 0) ns_device_count++;
    }
}
#else
static void ns_bonjour_scan(i32 ms) { (void)ms; }
#endif

static void ns_devices_scan(void) {
    ns_device_count = 0;
    ns_usbmux_scan();
    ns_bonjour_scan(1200);
}

// ---------------------------------------------------------------------------
// connecting

static int ns_tcp_connect(const char *host, i32 port) {
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *list = ns_null;
    if (getaddrinfo(host, service, &hints, &list) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(list);
    return fd;
}

static int ns_device_connect(const ns_device *d) {
    if (d->route == NS_DEVICE_BONJOUR) return ns_tcp_connect(d->host, d->port);
    // The app takes the first free port of the range; a refused port is
    // answered by usbmuxd at once, so trying them in turn is quick.
    for (i32 i = 0; i < NS_PROFILE_LIVE_DEVICE_PORTS; i++) {
        int fd = ns_usbmux_connect(d->mux_id, NS_PROFILE_LIVE_DEVICE_PORT + i);
        if (fd >= 0) return fd;
    }
    return -1;
}

// "", "1" and "any" take any device; "usb" and "wifi" pick a route; anything
// else matches part of the name or UDID.
static ns_bool ns_device_matches(const ns_device *d, const char *selector) {
    if (!selector || !selector[0] || strcmp(selector, "1") == 0 || strcmp(selector, "any") == 0) return true;
    if (strcmp(selector, "usb") == 0) return d->route == NS_DEVICE_USB;
    if (strcmp(selector, "wifi") == 0) return d->route != NS_DEVICE_USB;
    return strstr(d->name, selector) != ns_null || strstr(d->app, selector) != ns_null;
}

// The first matching device that accepts a connection, cable first.
static int ns_device_open(const char *selector, char *label, szt label_cap) {
    ns_devices_scan();
    for (i32 pass = 0; pass < 3; pass++) {
        for (i32 i = 0; i < ns_device_count; i++) {
            ns_device *d = &ns_devices[i];
            if ((i32)d->route != pass || !ns_device_matches(d, selector)) continue;
            int fd = ns_device_connect(d);
            if (fd >= 0) {
                snprintf(label, label_cap, "%s (%s)", d->name, ns_device_route_name(d->route));
                return fd;
            }
        }
    }
    return -1;
}

i32 ns_profile_device_list(void) {
    ns_devices_scan();
    if (ns_device_count == 0) {
        printf("no devices: connect one by USB, or start a Debug build of an ns app on this network.\n");
        return 0;
    }
    for (i32 i = 0; i < ns_device_count; i++) {
        ns_device *d = &ns_devices[i];
        if (d->route == NS_DEVICE_BONJOUR) {
            printf("  %-6s %s  %s:%d%s%s\n", ns_device_route_name(d->route), d->name, d->host, d->port,
                   d->app[0] ? "  app=" : "", d->app);
        } else {
            printf("  %-6s %s\n", ns_device_route_name(d->route), d->name);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// relay

static i32 ns_relay_pump(int from, int to) {
    u8 buf[65536];
    ssize_t n = read(from, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 1;
    if (n <= 0) return 0;
    return ns_io_all(to, buf, (szt)n, true) ? 1 : 0;
}

i32 ns_profile_device_relay(const char *selector, const char *host, i32 port) {
    signal(SIGPIPE, SIG_IGN);
    char label[300] = {0};
    int device = -1;
    // The app may still be launching: look for a while before giving up.
    for (i32 attempt = 0; attempt < 15 && device < 0; attempt++) {
        if (selector && strchr(selector, ':')) {
            char addr[256];
            snprintf(addr, sizeof(addr), "%s", selector);
            char *colon = strrchr(addr, ':');
            *colon = 0;
            device = ns_tcp_connect(addr, atoi(colon + 1));
            snprintf(label, sizeof(label), "%s", selector);
        } else {
            device = ns_device_open(selector, label, sizeof(label));
        }
        if (device < 0) {
            if (attempt == 0) ns_info("profile", "waiting for a device app (Debug build, in the foreground)...\n");
            sleep(1);
        }
    }
    if (device < 0) {
        ns_warn("profile", "no device app answered. Check `ns profiler --devices`.\n");
        return 1;
    }
    int one = 1;
    setsockopt(device, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int viewer = ns_tcp_connect(host && host[0] ? host : "127.0.0.1", port);
    if (viewer < 0) {
        close(device);
        ns_warn("profile", "viewer not listening on port %d.\n", port);
        return 1;
    }
    setsockopt(viewer, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ns_info("profile", "relaying %s to the viewer\n", label);
    for (;;) {
        struct pollfd p[2] = {{device, POLLIN, 0}, {viewer, POLLIN, 0}};
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && !ns_relay_pump(device, viewer)) break;
        if ((p[1].revents & (POLLIN | POLLHUP | POLLERR)) && !ns_relay_pump(viewer, device)) break;
    }
    close(device);
    close(viewer);
    ns_info("profile", "device session ended\n");
    return 0;
}
#endif
