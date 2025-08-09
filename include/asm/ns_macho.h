#pragma once

#include "ns_type.h"

typedef enum {
    NS_MACHO_CPU_TYPE_X86 = 0x00000007,
    NS_MACHO_CPU_TYPE_X86_64 = 0x01000007,
    NS_MACHO_CPU_TYPE_ARM = 0x0000000C,
    NS_MACHO_CPU_TYPE_ARM64 = 0x0000000D
} ns_macho_cpu_type;

typedef enum {
    NS_MACHO_CPU_SUB_TYPE_X86_64 = 0x00000003,
    NS_MACHO_CPU_SUB_TYPE_ARM64 = 0x0000000D
} ns_macho_cpu_sub_type;

typedef struct {
    u32 magic;
    ns_macho_cpu_type cputype;
    ns_macho_cpu_sub_type microtype;
    u32 filetype;
    u32 ncmds;
    u32 sizeofcmds;
    u32 flags;
} ns_macho_header;