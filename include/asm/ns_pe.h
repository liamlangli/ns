#pragma once

#include "ns_type.h"

/* ── DOS header (64 bytes) ─────────────────────────────────────────────────── */
typedef struct {
    u16 e_magic;        /* 0x5A4D "MZ"                              */
    u16 e_cblp;
    u16 e_cp;
    u16 e_crlc;
    u16 e_cparhdr;
    u16 e_minalloc;
    u16 e_maxalloc;
    u16 e_ss;
    u16 e_sp;
    u16 e_csum;
    u16 e_ip;
    u16 e_cs;
    u16 e_lfarlc;
    u16 e_ovno;
    u16 e_res[4];
    u16 e_oemid;
    u16 e_oeminfo;
    u16 e_res2[10];
    i32 e_lfanew;       /* file offset of the PE signature          */
} ns_pe_dos_header;     /* sizeof = 64                              */

/* ── PE signature ──────────────────────────────────────────────────────────── */
#define NS_PE_SIGNATURE 0x00004550U /* "PE\0\0" little-endian */

/* ── COFF file header (20 bytes) ───────────────────────────────────────────── */
#define NS_PE_MACHINE_AMD64  0x8664U
#define NS_PE_FILE_EXECUTABLE_IMAGE    0x0002U
#define NS_PE_FILE_LARGE_ADDRESS_AWARE 0x0020U

typedef struct {
    u16 machine;
    u16 number_of_sections;
    u32 time_date_stamp;
    u32 pointer_to_symbol_table; /* 0 (deprecated) */
    u32 number_of_symbols;       /* 0 (deprecated) */
    u16 size_of_optional_header;
    u16 characteristics;
} ns_pe_file_header;    /* sizeof = 20 */

/* ── Data directory entry (8 bytes) ────────────────────────────────────────── */
typedef struct {
    u32 virtual_address;
    u32 size;
} ns_pe_data_directory; /* sizeof = 8 */

#define NS_PE_NUM_DATA_DIRS 16

/* ── Optional header PE32+ (240 bytes) ─────────────────────────────────────── */
#define NS_PE_OPT_MAGIC_PE32PLUS 0x020BU

/* Subsystem values */
#define NS_PE_SUBSYSTEM_CUI 3U  /* console */
#define NS_PE_SUBSYSTEM_GUI 2U  /* windowed */

/* DllCharacteristics flags */
#define NS_PE_DLLCHAR_HIGH_ENTROPY_VA  0x0020U
#define NS_PE_DLLCHAR_DYNAMIC_BASE     0x0040U
#define NS_PE_DLLCHAR_NX_COMPAT        0x0100U
#define NS_PE_DLLCHAR_NO_SEH           0x0400U
#define NS_PE_DLLCHAR_TERMINAL_SERVER  0x8000U

typedef struct {
    /* standard fields */
    u16 magic;
    u8  major_linker_version;
    u8  minor_linker_version;
    u32 size_of_code;
    u32 size_of_initialized_data;
    u32 size_of_uninitialized_data;
    u32 address_of_entry_point;     /* RVA */
    u32 base_of_code;               /* RVA */
    /* Windows-specific fields */
    u64 image_base;
    u32 section_alignment;
    u32 file_alignment;
    u16 major_os_version;
    u16 minor_os_version;
    u16 major_image_version;
    u16 minor_image_version;
    u16 major_subsystem_version;
    u16 minor_subsystem_version;
    u32 win32_version_value;        /* 0 */
    u32 size_of_image;
    u32 size_of_headers;
    u32 checksum;
    u16 subsystem;
    u16 dll_characteristics;
    u64 size_of_stack_reserve;
    u64 size_of_stack_commit;
    u64 size_of_heap_reserve;
    u64 size_of_heap_commit;
    u32 loader_flags;               /* 0 */
    u32 number_of_rva_and_sizes;
    ns_pe_data_directory data_dir[NS_PE_NUM_DATA_DIRS];
} ns_pe_optional_header; /* sizeof = 240 */

/* ── Section header (40 bytes) ──────────────────────────────────────────────── */
/* Section characteristic flags */
#define NS_PE_SCN_CNT_CODE               0x00000020U
#define NS_PE_SCN_MEM_EXECUTE            0x20000000U
#define NS_PE_SCN_MEM_READ               0x40000000U
#define NS_PE_SCN_MEM_WRITE              0x80000000U

typedef struct {
    i8  name[8];
    u32 virtual_size;
    u32 virtual_address;        /* RVA */
    u32 size_of_raw_data;
    u32 pointer_to_raw_data;    /* file offset */
    u32 pointer_to_relocations; /* 0 */
    u32 pointer_to_linenumbers; /* 0 */
    u16 number_of_relocations;  /* 0 */
    u16 number_of_linenumbers;  /* 0 */
    u32 characteristics;
} ns_pe_section_header;  /* sizeof = 40 */
