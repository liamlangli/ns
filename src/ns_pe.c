#include "ns_pe.h"
#include "asm/ns_pe.h"

#include <errno.h>
#include <time.h>

#if !NS_WIN
#include <sys/stat.h>
#endif

/*
 * PE/COFF executable emitter for AMD64 (Windows x64).
 *
 * Produces a minimal PE32+ executable:
 *
 *   Offset  Size   Content
 *   0       64     DOS header (e_lfanew = 64)
 *   64      4      PE signature "PE\0\0"
 *   68      20     COFF file header
 *   88      240    Optional header PE32+
 *   328     40     .text section header
 *   368     144    Padding to SizeOfHeaders (512 = 0x200)
 *   512     ...    .text code (aligned to FileAlignment = 512)
 *
 * Image layout in memory (ImageBase = 0x140000000):
 *   +0x0000  headers
 *   +0x1000  .text section (VirtualAddress)
 */

#define NS_PE_FILE_ALIGN     0x200U   /* 512 bytes */
#define NS_PE_SECT_ALIGN     0x1000U  /* 4 KiB */
#define NS_PE_IMAGE_BASE     0x140000000ULL
#define NS_PE_STACK_RESERVE  0x100000ULL
#define NS_PE_STACK_COMMIT   0x1000ULL
#define NS_PE_HEAP_RESERVE   0x100000ULL
#define NS_PE_HEAP_COMMIT    0x1000ULL

static u64 ns_pe_align_up(u64 n, u64 align) {
    if (align == 0) return n;
    u64 mask = align - 1;
    return (n + mask) & ~mask;
}

static ns_amd64_fn_bin *ns_pe_pick_entry(ns_amd64_module_bin *m) {
    ns_amd64_fn_bin *fallback = ns_null;
    for (i32 i = 0, l = (i32)ns_array_length(m->fns); i < l; ++i) {
        ns_amd64_fn_bin *fn = &m->fns[i];
        if (!fallback) fallback = fn;
        if (ns_str_equals_STR(fn->name, "main")) return fn;
    }
    for (i32 i = 0, l = (i32)ns_array_length(m->fns); i < l; ++i) {
        ns_amd64_fn_bin *fn = &m->fns[i];
        if (ns_str_equals_STR(fn->name, "__module_init")) return fn;
    }
    return fallback;
}

static ns_return_bool ns_pe_write_file(ns_str path, u8 *buf) {
    FILE *f = fopen(path.data, "wb");
    if (!f) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME,
            "failed to create pe output file");
    }
    szt len = ns_array_length(buf);
    szt written = fwrite(buf, 1, len, f);
    fclose(f);
    if (written != len) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME,
            "failed to write full pe output");
    }
#if !NS_WIN
    chmod(path.data, 0755);
#endif
    return ns_return_ok(bool, true);
}

ns_return_bool ns_pe_emit(ns_ssa_module *ssa, ns_str output_path) {
    if (!ssa) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "null ssa module");
    }
    if (ns_str_is_empty(output_path)) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "empty output path");
    }

    /* ── codegen: SSA → AMD64 binary ─────────────────────────────────────── */
    ns_return_ptr amd64_ret = ns_amd64_from_ssa(ssa);
    if (ns_return_is_error(amd64_ret)) {
        return ns_return_change_type(bool, amd64_ret);
    }
    ns_amd64_module_bin *amd64 = amd64_ret.r;

    ns_amd64_fn_bin *entry = ns_pe_pick_entry(amd64);
    if (!entry || ns_array_length(entry->text) == 0) {
        ns_amd64_free(amd64);
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME,
            "no entry function for pe emit");
    }

    /* ── layout: compute file offsets for all functions ──────────────────── */
    i32 nfns = (i32)ns_array_length(amd64->fns);

    /*
     * Header block occupies 368 bytes:
     *   64  DOS header
     *    4  PE signature
     *   20  COFF header
     *  240  Optional header
     *   40  .text section header
     *  ───
     *  368  total
     *
     * Padded to FileAlignment (512) → SizeOfHeaders = 512.
     */
    const u32 HDR_SIZE_RAW   = 368;
    const u32 SIZE_OF_HEADERS = (u32)ns_pe_align_up(HDR_SIZE_RAW, NS_PE_FILE_ALIGN);

    /* functions are laid out contiguously after the headers */
    u64 *fn_off = ns_null;
    ns_array_set_length(fn_off, nfns);

    u64 code_cursor = 0; /* relative to start of .text raw data */
    for (i32 i = 0; i < nfns; ++i) {
        /* 16-byte align each function within the section */
        code_cursor = ns_pe_align_up(code_cursor, 16);
        fn_off[i] = code_cursor;
        code_cursor += ns_array_length(amd64->fns[i].text);
    }
    u64 code_size_raw  = code_cursor;
    u32 size_of_raw    = (u32)ns_pe_align_up(code_size_raw, NS_PE_FILE_ALIGN);

    /* virtual address of .text section */
    u32 text_rva = (u32)ns_pe_align_up(SIZE_OF_HEADERS, NS_PE_SECT_ALIGN);

    /* entry point RVA */
    u32 entry_fn_rva_off = 0;
    for (i32 i = 0; i < nfns; ++i) {
        if (&amd64->fns[i] == entry) {
            entry_fn_rva_off = (u32)fn_off[i];
            break;
        }
    }
    u32 entry_rva = text_rva + entry_fn_rva_off;

    /* SizeOfImage: must be a multiple of SectionAlignment */
    u32 size_of_image = (u32)ns_pe_align_up(text_rva + size_of_raw, NS_PE_SECT_ALIGN);

    /* total file size */
    u64 file_size = SIZE_OF_HEADERS + size_of_raw;

    /* ── build PE structures ─────────────────────────────────────────────── */

    /* DOS header */
    ns_pe_dos_header dos = {0};
    dos.e_magic    = 0x5A4DU; /* MZ */
    dos.e_cblp     = 0x90U;
    dos.e_cp       = 3U;
    dos.e_cparhdr  = 4U;
    dos.e_maxalloc = 0xFFFFU;
    dos.e_sp       = 0xB8U;
    dos.e_lfarlc   = 0x40U;
    dos.e_lfanew   = (i32)sizeof(ns_pe_dos_header); /* = 64 */

    /* COFF file header */
    ns_pe_file_header coff = {0};
    coff.machine               = NS_PE_MACHINE_AMD64;
    coff.number_of_sections    = 1;
    coff.time_date_stamp       = (u32)time(ns_null);
    coff.size_of_optional_header = sizeof(ns_pe_optional_header);
    coff.characteristics       = NS_PE_FILE_EXECUTABLE_IMAGE |
                                 NS_PE_FILE_LARGE_ADDRESS_AWARE;

    /* Optional header PE32+ */
    ns_pe_optional_header opt = {0};
    opt.magic                    = NS_PE_OPT_MAGIC_PE32PLUS;
    opt.major_linker_version     = 1;
    opt.size_of_code             = size_of_raw;
    opt.address_of_entry_point   = entry_rva;
    opt.base_of_code             = text_rva;
    opt.image_base               = NS_PE_IMAGE_BASE;
    opt.section_alignment        = NS_PE_SECT_ALIGN;
    opt.file_alignment           = NS_PE_FILE_ALIGN;
    opt.major_os_version         = 6;
    opt.major_subsystem_version  = 6;
    opt.size_of_image            = size_of_image;
    opt.size_of_headers          = SIZE_OF_HEADERS;
    opt.subsystem                = NS_PE_SUBSYSTEM_CUI;
    opt.dll_characteristics      = NS_PE_DLLCHAR_HIGH_ENTROPY_VA |
                                   NS_PE_DLLCHAR_DYNAMIC_BASE    |
                                   NS_PE_DLLCHAR_NX_COMPAT       |
                                   NS_PE_DLLCHAR_TERMINAL_SERVER;
    opt.size_of_stack_reserve    = NS_PE_STACK_RESERVE;
    opt.size_of_stack_commit     = NS_PE_STACK_COMMIT;
    opt.size_of_heap_reserve     = NS_PE_HEAP_RESERVE;
    opt.size_of_heap_commit      = NS_PE_HEAP_COMMIT;
    opt.number_of_rva_and_sizes  = NS_PE_NUM_DATA_DIRS;

    /* .text section header */
    ns_pe_section_header text_sec = {0};
    memcpy(text_sec.name, ".text\0\0\0", 8);
    text_sec.virtual_size        = (u32)code_size_raw;
    text_sec.virtual_address     = text_rva;
    text_sec.size_of_raw_data    = size_of_raw;
    text_sec.pointer_to_raw_data = SIZE_OF_HEADERS;
    text_sec.characteristics     = NS_PE_SCN_CNT_CODE    |
                                   NS_PE_SCN_MEM_EXECUTE |
                                   NS_PE_SCN_MEM_READ;

    /* ── assemble output buffer ──────────────────────────────────────────── */
    u8 *out = ns_null;
    ns_array_set_length(out, file_size);
    memset(out, 0, file_size);

    u64 off = 0;
    /* DOS header */
    memcpy(&out[off], &dos, sizeof(dos)); off += sizeof(dos);
    /* PE signature */
    u32 pe_sig = NS_PE_SIGNATURE;
    memcpy(&out[off], &pe_sig, 4); off += 4;
    /* COFF header */
    memcpy(&out[off], &coff, sizeof(coff)); off += sizeof(coff);
    /* Optional header */
    memcpy(&out[off], &opt, sizeof(opt)); off += sizeof(opt);
    /* Section header */
    memcpy(&out[off], &text_sec, sizeof(text_sec));
    /* (padding to SIZE_OF_HEADERS already zeroed) */

    /* copy function code into the .text region */
    u64 text_file_base = SIZE_OF_HEADERS;
    for (i32 i = 0; i < nfns; ++i) {
        u64 fn_text_size = ns_array_length(amd64->fns[i].text);
        if (fn_text_size > 0) {
            memcpy(&out[text_file_base + fn_off[i]],
                   amd64->fns[i].text, fn_text_size);
        }
    }

    /* ── patch inter-function CALL fixups ────────────────────────────────── */
    for (i32 fi = 0; fi < nfns; ++fi) {
        ns_amd64_fn_bin *fn = &amd64->fns[fi];
        i32 ncf = (i32)ns_array_length(fn->call_fixups);
        for (i32 ci = 0; ci < ncf; ++ci) {
            ns_amd64_call_fixup *cf = &fn->call_fixups[ci];
            /* find callee */
            i32 callee_idx = -1;
            for (i32 k = 0; k < nfns; ++k) {
                if (ns_str_equals(amd64->fns[k].name, cf->callee)) {
                    callee_idx = k;
                    break;
                }
            }
            if (callee_idx < 0) continue;
            /*
             * CALL rel32: the rel32 field is at file offset
             *   text_file_base + fn_off[fi] + cf->off
             * The CALL instruction is 5 bytes total (E8 + 4-byte rel32).
             * rel32 = target_va - (call_site_va + 5)
             *
             * Using file offsets (base addresses cancel out):
             *   call_field_end_file = text_file_base + fn_off[fi] + cf->off + 4
             *   target_file         = text_file_base + fn_off[callee_idx]
             *   rel32 = target_file - call_field_end_file
             */
            i64 call_end = (i64)(text_file_base + fn_off[fi] + cf->off + 4);
            i64 target   = (i64)(text_file_base + fn_off[callee_idx]);
            i32 rel32    = (i32)(target - call_end);
            u64 patch_off = text_file_base + fn_off[fi] + cf->off;
            out[patch_off + 0] = (u8)(rel32 & 0xFF);
            out[patch_off + 1] = (u8)((rel32 >> 8) & 0xFF);
            out[patch_off + 2] = (u8)((rel32 >> 16) & 0xFF);
            out[patch_off + 3] = (u8)((rel32 >> 24) & 0xFF);
        }
    }

    ns_array_free(fn_off);
    ns_return_bool wr = ns_pe_write_file(output_path, out);
    ns_array_free(out);
    ns_amd64_free(amd64);
    return wr;
}
