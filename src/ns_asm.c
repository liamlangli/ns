#include "ns_asm.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <stdbool.h>

    static ns_arch arch_from_win_word(WORD arch) {
        switch (arch) {
            case PROCESSOR_ARCHITECTURE_AMD64: return NS_ARCH_X64;
            case PROCESSOR_ARCHITECTURE_INTEL: return NS_ARCH_X86;
            case PROCESSOR_ARCHITECTURE_ARM:   return NS_ARCH_ARM;
            case PROCESSOR_ARCHITECTURE_ARM64: return NS_ARCH_AARCH64;
            default:                           return NS_ARCH_UNKNOWN;
        }
    }

    // Try IsWow64Process2 for accurate native arch
    static bool get_native_arch_word(WORD* native) {
        HMODULE h = GetModuleHandleA("kernel32.dll");
        if (!h) return false;
        typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT*, USHORT*);
        IsWow64Process2_t fn = (IsWow64Process2_t)GetProcAddress(h, "IsWow64Process2");
        if (!fn) return false;
        USHORT proc = 0, nat = 0;
        if (!fn(GetCurrentProcess(), &proc, &nat)) return false;
        if (native) *native = (WORD)nat;
        return true;
    }
#else
    #include <sys/utsname.h>
    #ifdef __APPLE__
        #include <sys/types.h>
        #include <sys/sysctl.h>
    #endif
#endif

void ns_asm_get_current_target(ns_asm_target *target) {
    if (!target) return;

    target->arch = NS_ARCH_UNKNOWN;
    target->os = NS_OS_UNKNOWN;

#ifdef _WIN32
    target->os = NS_OS_WINDOWS;

    WORD nativeArch = 0;
    if (get_native_arch_word(&nativeArch)) {
        target->arch = arch_from_win_word(nativeArch);
    } else {
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        target->arch = arch_from_win_word(si.wProcessorArchitecture);
    }

#else
    struct utsname u;
    if (uname(&u) == 0) {
        if (strcmp(u.sysname, "Linux") == 0)
            target->os = NS_OS_LINUX;
        else if (strcmp(u.sysname, "Darwin") == 0)
            target->os = NS_OS_DARWIN;

    #ifdef __APPLE__
        char buf[256] = {0};
        size_t len = sizeof(buf);
        if (sysctlbyname("hw.machine", buf, &len, NULL, 0) == 0) {
            if (strcmp(buf, "x86_64") == 0) target->arch = NS_ARCH_X64;
            else if (strcmp(buf, "arm64") == 0) target->arch = NS_ARCH_AARCH64;
        }
    #else
        if (strcmp(u.machine, "x86_64") == 0) target->arch = NS_ARCH_X64;
        else if (strcmp(u.machine, "i686") == 0 || strcmp(u.machine, "i386") == 0) target->arch = NS_ARCH_X86;
        else if (strcmp(u.machine, "armv7l") == 0 || strcmp(u.machine, "armv6l") == 0) target->arch = NS_ARCH_ARM;
        else if (strcmp(u.machine, "aarch64") == 0) target->arch = NS_ARCH_AARCH64;
    #endif
    }
#endif
}

ns_str ns_os_str(ns_os os) {
    switch (os) {
        case NS_OS_LINUX: return ns_str_cstr("linux");
        case NS_OS_DARWIN: return ns_str_cstr("darwin");
        case NS_OS_WINDOWS: return ns_str_cstr("windows");
        default: return ns_str_cstr("unknown");
    }
}
ns_str ns_arch_str(ns_arch arch) {
    switch (arch) {
        case NS_ARCH_X64: return ns_str_cstr("x86_64");
        case NS_ARCH_X86: return ns_str_cstr("x86");
        case NS_ARCH_ARM: return ns_str_cstr("arm");
        case NS_ARCH_AARCH64: return ns_str_cstr("aarch64");
        default: return ns_str_cstr("unknown");
    }
}