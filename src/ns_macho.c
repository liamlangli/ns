#include "ns_macho.h"

#include <errno.h>

#if !NS_WIN
#include <sys/stat.h>
#endif

#define NS_MACHO_MAGIC_64 0xfeedfacfU
#define NS_MACHO_CPU_TYPE_ARM64 0x0100000cU
#define NS_MACHO_CPU_SUBTYPE_ARM64_ALL 0U
#define NS_MACHO_FILETYPE_OBJECT 0x1U
#define NS_MACHO_FILETYPE_EXECUTE 0x2U

#define NS_MACHO_LC_SYMTAB 0x2U
#define NS_MACHO_LC_DYSYMTAB 0xbU
#define NS_MACHO_LC_SEGMENT_64 0x19U
#define NS_MACHO_LC_BUILD_VERSION 0x32U
#define NS_MACHO_LC_MAIN 0x80000028U

#define NS_MACHO_VM_PROT_READ 0x1
#define NS_MACHO_VM_PROT_EXECUTE 0x4
#define NS_MACHO_VM_PROT_WRITE 0x2

#define NS_MACHO_MH_NOUNDEFS 0x1
#define NS_MACHO_MH_DYLDLINK 0x4
#define NS_MACHO_MH_TWOLEVEL 0x80
#define NS_MACHO_MH_PIE 0x200000

#define NS_MACHO_SECTION_TYPE_REGULAR 0x0
#define NS_MACHO_SECTION_ATTR_SOME_INSTRUCTIONS 0x00000400U
#define NS_MACHO_SECTION_ATTR_PURE_INSTRUCTIONS 0x80000000U

#define NS_MACHO_PLATFORM_MACOS 1U
#define NS_MACHO_N_SECT 0x0e
#define NS_MACHO_N_EXT 0x01

typedef struct {
    u32 magic;
    u32 cputype;
    u32 cpusubtype;
    u32 filetype;
    u32 ncmds;
    u32 sizeofcmds;
    u32 flags;
    u32 reserved;
} ns_macho_header_64;

typedef struct {
    u32 cmd;
    u32 cmdsize;
    i8 segname[16];
    u64 vmaddr;
    u64 vmsize;
    u64 fileoff;
    u64 filesize;
    i32 maxprot;
    i32 initprot;
    u32 nsects;
    u32 flags;
} ns_macho_segment_command_64;

typedef struct {
    i8 sectname[16];
    i8 segname[16];
    u64 addr;
    u64 size;
    u32 offset;
    u32 align;
    u32 reloff;
    u32 nreloc;
    u32 flags;
    u32 reserved1;
    u32 reserved2;
    u32 reserved3;
} ns_macho_section_64;

typedef struct {
    u32 cmd;
    u32 cmdsize;
    u64 entryoff;
    u64 stacksize;
} ns_macho_entry_point_command;

typedef struct {
    u32 cmd;
    u32 cmdsize;
    u32 platform;
    u32 minos;
    u32 sdk;
    u32 ntools;
} ns_macho_build_version_command;

typedef struct {
    u32 cmd;
    u32 cmdsize;
    u32 symoff;
    u32 nsyms;
    u32 stroff;
    u32 strsize;
} ns_macho_symtab_command;

typedef struct {
    u32 cmd;
    u32 cmdsize;
    u32 ilocalsym;
    u32 nlocalsym;
    u32 iextdefsym;
    u32 nextdefsym;
    u32 iundefsym;
    u32 nundefsym;
    u32 tocoff;
    u32 ntoc;
    u32 modtaboff;
    u32 nmodtab;
    u32 extrefsymoff;
    u32 nextrefsyms;
    u32 indirectsymoff;
    u32 nindirectsyms;
    u32 extreloff;
    u32 nextrel;
    u32 locreloff;
    u32 nlocrel;
} ns_macho_dysymtab_command;

typedef struct {
    u32 n_strx;
    u8 n_type;
    u8 n_sect;
    u16 n_desc;
    u64 n_value;
} ns_macho_nlist_64;

static u64 ns_macho_align_up(u64 n, u64 align) {
    if (align == 0) return n;
    u64 mask = align - 1;
    return (n + mask) & ~mask;
}

static ns_aarch_fn_bin *ns_macho_pick_entry(ns_aarch_module_bin *m) {
    ns_aarch_fn_bin *fallback = ns_null;
    for (i32 i = 0, l = (i32)ns_array_length(m->fns); i < l; ++i) {
        ns_aarch_fn_bin *fn = &m->fns[i];
        if (!fallback) fallback = fn;
        if (ns_str_equals_STR(fn->name, "main")) return fn;
    }
    for (i32 i = 0, l = (i32)ns_array_length(m->fns); i < l; ++i) {
        ns_aarch_fn_bin *fn = &m->fns[i];
        if (ns_str_equals_STR(fn->name, "__module_init")) return fn;
    }
    return fallback;
}

static ns_return_bool ns_macho_write_file(ns_str path, u8 *buf) {
    FILE *f = fopen(path.data, "wb");
    if (!f) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "failed to create mach-o output");
    }

    szt len = ns_array_length(buf);
    szt written = fwrite(buf, 1, len, f);
    fclose(f);
    if (written != len) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "failed to write full mach-o output");
    }

#if !NS_WIN
    chmod(path.data, 0755);
#endif
    return ns_return_ok(bool, true);
}

static ns_str ns_macho_symbol_name(ns_str fn_name) {
    ns_str s = ns_str_null;
    ns_str_append_len(&s, "_", 1);
    ns_str_append_len(&s, fn_name.data, fn_name.len);
    s.dynamic = true;
    return s;
}

ns_return_bool ns_macho_emit(ns_ssa_module *ssa, ns_str output_path) {
    if (!ssa) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "null ssa module");
    }
    if (ns_str_is_empty(output_path)) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "empty output path");
    }

    ns_return_ptr aarch_ret = ns_aarch_from_ssa(ssa);
    if (ns_return_is_error(aarch_ret)) {
        return ns_return_change_type(bool, aarch_ret);
    }

    ns_aarch_module_bin *aarch = aarch_ret.r;
    ns_aarch_fn_bin *entry = ns_macho_pick_entry(aarch);
    if (!entry || ns_array_length(entry->text) == 0) {
        ns_aarch_free(aarch);
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "no entry text for mach-o emit");
    }

    u64 code_size = ns_array_length(entry->text);
    const u64 vmaddr_base = 0x0000000100000000ULL;
    const u32 ncmds = 2;

    u64 seg_cmd_size = sizeof(ns_macho_segment_command_64) + sizeof(ns_macho_section_64);
    u64 entry_cmd_size = sizeof(ns_macho_entry_point_command);
    u64 sizeofcmds = seg_cmd_size + entry_cmd_size;
    u64 header_and_cmds = sizeof(ns_macho_header_64) + sizeofcmds;
    u64 text_offset = ns_macho_align_up(header_and_cmds, 16);
    u64 file_size = text_offset + code_size;
    u64 vm_size = ns_macho_align_up(file_size, 0x1000);

    ns_macho_header_64 header = {0};
    header.magic = NS_MACHO_MAGIC_64;
    header.cputype = NS_MACHO_CPU_TYPE_ARM64;
    header.cpusubtype = NS_MACHO_CPU_SUBTYPE_ARM64_ALL;
    header.filetype = NS_MACHO_FILETYPE_EXECUTE;
    header.ncmds = ncmds;
    header.sizeofcmds = (u32)sizeofcmds;
    header.flags = NS_MACHO_MH_NOUNDEFS | NS_MACHO_MH_DYLDLINK | NS_MACHO_MH_TWOLEVEL | NS_MACHO_MH_PIE;

    ns_macho_segment_command_64 seg = {0};
    seg.cmd = NS_MACHO_LC_SEGMENT_64;
    seg.cmdsize = (u32)seg_cmd_size;
    memcpy(seg.segname, "__TEXT", 6);
    seg.vmaddr = vmaddr_base;
    seg.vmsize = vm_size;
    seg.fileoff = 0;
    seg.filesize = file_size;
    seg.maxprot = NS_MACHO_VM_PROT_READ | NS_MACHO_VM_PROT_EXECUTE;
    seg.initprot = NS_MACHO_VM_PROT_READ | NS_MACHO_VM_PROT_EXECUTE;
    seg.nsects = 1;

    ns_macho_section_64 sec = {0};
    memcpy(sec.sectname, "__text", 6);
    memcpy(sec.segname, "__TEXT", 6);
    sec.addr = vmaddr_base + text_offset;
    sec.size = code_size;
    sec.offset = (u32)text_offset;
    sec.align = 2; // 2^2 = 4-byte instruction alignment
    sec.flags = NS_MACHO_SECTION_TYPE_REGULAR;

    ns_macho_entry_point_command ep = {0};
    ep.cmd = NS_MACHO_LC_MAIN;
    ep.cmdsize = (u32)entry_cmd_size;
    ep.entryoff = text_offset;
    ep.stacksize = 0;

    u8 *out = ns_null;
    ns_array_set_length(out, file_size);
    memset(out, 0, file_size);

    u64 off = 0;
    memcpy(&out[off], &header, sizeof(header)); off += sizeof(header);
    memcpy(&out[off], &seg, sizeof(seg)); off += sizeof(seg);
    memcpy(&out[off], &sec, sizeof(sec)); off += sizeof(sec);
    memcpy(&out[off], &ep, sizeof(ep)); off += sizeof(ep);
    memcpy(&out[text_offset], entry->text, code_size);

    ns_return_bool wr = ns_macho_write_file(output_path, out);
    ns_array_free(out);
    ns_aarch_free(aarch);
    return wr;
}

ns_return_bool ns_macho_emit_object(ns_ssa_module *ssa, ns_str output_path) {
    if (!ssa) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "null ssa module");
    }
    if (ns_str_is_empty(output_path)) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "empty output path");
    }

    ns_return_ptr aarch_ret = ns_aarch_from_ssa(ssa);
    if (ns_return_is_error(aarch_ret)) {
        return ns_return_change_type(bool, aarch_ret);
    }

    ns_aarch_module_bin *aarch = aarch_ret.r;
    ns_aarch_fn_bin *entry = ns_macho_pick_entry(aarch);
    if (!entry || ns_array_length(entry->text) == 0) {
        ns_aarch_free(aarch);
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "no function text for mach-o object emit");
    }

    ns_str global_sym = ns_macho_symbol_name(entry->name);
    ns_str local_sym = ns_str_cstr("ltmp0");

    u64 code_size = ns_array_length(entry->text);
    const u32 ncmds = 4;

    u64 seg_cmd_size = sizeof(ns_macho_segment_command_64) + sizeof(ns_macho_section_64);
    u64 build_cmd_size = sizeof(ns_macho_build_version_command);
    u64 symtab_cmd_size = sizeof(ns_macho_symtab_command);
    u64 dysymtab_cmd_size = sizeof(ns_macho_dysymtab_command);
    u64 sizeofcmds = seg_cmd_size + build_cmd_size + symtab_cmd_size + dysymtab_cmd_size;

    u64 header_and_cmds = sizeof(ns_macho_header_64) + sizeofcmds;
    u64 text_offset = ns_macho_align_up(header_and_cmds, 16);
    u64 symoff = ns_macho_align_up(text_offset + code_size, 8);
    u32 nsyms = 2;
    u64 sym_bytes = (u64)nsyms * sizeof(ns_macho_nlist_64);
    u64 stroff = symoff + sym_bytes;
    u64 strsize = 1 + (u64)local_sym.len + 1 + (u64)global_sym.len + 1;
    u64 file_size = stroff + strsize;

    ns_macho_header_64 header = {0};
    header.magic = NS_MACHO_MAGIC_64;
    header.cputype = NS_MACHO_CPU_TYPE_ARM64;
    header.cpusubtype = NS_MACHO_CPU_SUBTYPE_ARM64_ALL;
    header.filetype = NS_MACHO_FILETYPE_OBJECT;
    header.ncmds = ncmds;
    header.sizeofcmds = (u32)sizeofcmds;
    header.flags = 0;

    ns_macho_segment_command_64 seg = {0};
    seg.cmd = NS_MACHO_LC_SEGMENT_64;
    seg.cmdsize = (u32)seg_cmd_size;
    seg.vmaddr = 0;
    seg.vmsize = code_size;
    seg.fileoff = text_offset;
    seg.filesize = code_size;
    seg.maxprot = NS_MACHO_VM_PROT_READ | NS_MACHO_VM_PROT_WRITE | NS_MACHO_VM_PROT_EXECUTE;
    seg.initprot = seg.maxprot;
    seg.nsects = 1;

    ns_macho_section_64 sec = {0};
    memcpy(sec.sectname, "__text", 6);
    memcpy(sec.segname, "__TEXT", 6);
    sec.addr = 0;
    sec.size = code_size;
    sec.offset = (u32)text_offset;
    sec.align = 2;
    sec.flags = NS_MACHO_SECTION_TYPE_REGULAR | NS_MACHO_SECTION_ATTR_SOME_INSTRUCTIONS | NS_MACHO_SECTION_ATTR_PURE_INSTRUCTIONS;

    ns_macho_build_version_command build = {0};
    build.cmd = NS_MACHO_LC_BUILD_VERSION;
    build.cmdsize = (u32)build_cmd_size;
    build.platform = NS_MACHO_PLATFORM_MACOS;
    build.minos = (16u << 16); // 16.0.0
    build.sdk = 0;
    build.ntools = 0;

    ns_macho_symtab_command symtab = {0};
    symtab.cmd = NS_MACHO_LC_SYMTAB;
    symtab.cmdsize = (u32)symtab_cmd_size;
    symtab.symoff = (u32)symoff;
    symtab.nsyms = nsyms;
    symtab.stroff = (u32)stroff;
    symtab.strsize = (u32)strsize;

    ns_macho_dysymtab_command dysym = {0};
    dysym.cmd = NS_MACHO_LC_DYSYMTAB;
    dysym.cmdsize = (u32)dysymtab_cmd_size;
    dysym.ilocalsym = 0;
    dysym.nlocalsym = 1;
    dysym.iextdefsym = 1;
    dysym.nextdefsym = 1;
    dysym.iundefsym = 2;
    dysym.nundefsym = 0;

    u8 *out = ns_null;
    ns_array_set_length(out, file_size);
    memset(out, 0, file_size);

    u64 off = 0;
    memcpy(&out[off], &header, sizeof(header)); off += sizeof(header);
    memcpy(&out[off], &seg, sizeof(seg)); off += sizeof(seg);
    memcpy(&out[off], &sec, sizeof(sec)); off += sizeof(sec);
    memcpy(&out[off], &build, sizeof(build)); off += sizeof(build);
    memcpy(&out[off], &symtab, sizeof(symtab)); off += sizeof(symtab);
    memcpy(&out[off], &dysym, sizeof(dysym)); off += sizeof(dysym);
    memcpy(&out[text_offset], entry->text, code_size);

    ns_macho_nlist_64 syms[2] = {0};
    syms[0].n_strx = 1;
    syms[0].n_type = NS_MACHO_N_SECT;
    syms[0].n_sect = 1;
    syms[0].n_desc = 0;
    syms[0].n_value = 0;

    syms[1].n_strx = 1 + (u32)local_sym.len + 1;
    syms[1].n_type = NS_MACHO_N_SECT | NS_MACHO_N_EXT;
    syms[1].n_sect = 1;
    syms[1].n_desc = 0;
    syms[1].n_value = 0;
    memcpy(&out[symoff], syms, sizeof(syms));

    u64 st = stroff;
    out[st++] = '\0';
    memcpy(&out[st], local_sym.data, local_sym.len); st += local_sym.len; out[st++] = '\0';
    memcpy(&out[st], global_sym.data, global_sym.len); st += global_sym.len; out[st++] = '\0';

    ns_return_bool wr = ns_macho_write_file(output_path, out);
    ns_array_free(out);
    ns_str_free(global_sym);
    ns_aarch_free(aarch);
    return wr;
}
