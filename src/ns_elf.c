#include "ns_elf.h"

#include <errno.h>

/*
 * ELF64 object emitter for AMD64 (System V hosts).
 *
 * The file holds one .text section with every compiled function, the
 * relocations that name the runtime helpers and FFI symbols the code calls,
 * and a symbol table that exports each function. Calls inside the module are
 * patched here, so only the symbols a linker has to resolve get a relocation.
 *
 *   Offset  Content
 *   0       ELF header
 *   ...     .text        (16-byte aligned, one entry per function)
 *   ...     .rela.text   (8-byte aligned)
 *   ...     .symtab      (8-byte aligned)
 *   ...     .strtab      (symbol names)
 *   ...     .shstrtab    (section names)
 *   ...     section header table
 */

#define NS_ELF_SHN_UNDEF 0
#define NS_ELF_SHN_TEXT 1

#define NS_ELF_SHT_PROGBITS 1
#define NS_ELF_SHT_SYMTAB 2
#define NS_ELF_SHT_STRTAB 3
#define NS_ELF_SHT_RELA 4

#define NS_ELF_SHF_ALLOC 0x2
#define NS_ELF_SHF_EXECINSTR 0x4
#define NS_ELF_SHF_INFO_LINK 0x40

#define NS_ELF_STB_GLOBAL 1
#define NS_ELF_STT_NOTYPE 0
#define NS_ELF_STT_FUNC 2

/* x86-64 relocation types. */
#define NS_ELF_R_X86_64_PC32 2
#define NS_ELF_R_X86_64_PLT32 4

typedef struct {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} ns_elf_header;

typedef struct {
    u32 sh_name;
    u32 sh_type;
    u64 sh_flags;
    u64 sh_addr;
    u64 sh_offset;
    u64 sh_size;
    u32 sh_link;
    u32 sh_info;
    u64 sh_addralign;
    u64 sh_entsize;
} ns_elf_section;

typedef struct {
    u32 st_name;
    u8 st_info;
    u8 st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} ns_elf_sym;

typedef struct {
    u64 r_offset;
    u64 r_info;
    i64 r_addend;
} ns_elf_rela;

static u64 ns_elf_align_up(u64 n, u64 align) {
    if (align == 0) return n;
    u64 mask = align - 1;
    return (n + mask) & ~mask;
}

static u64 ns_elf_reloc_info(u32 sym, u32 type) {
    return ((u64)sym << 32) | (u64)type;
}

static ns_return_bool ns_elf_write_file(ns_str path, u8 *buf) {
    FILE *f = fopen(path.data, "wb");
    if (!f) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "failed to create elf output");
    }

    szt len = ns_array_length(buf);
    szt written = fwrite(buf, 1, len, f);
    fclose(f);
    if (written != len) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "failed to write full elf output");
    }
    return ns_return_ok(bool, true);
}

/* Append a name to a string table buffer and return its offset. */
static u32 ns_elf_str_push(u8 **tab, const char *name) {
    u32 off = (u32)ns_array_length(*tab);
    for (const char *p = name; *p; ++p) ns_array_push(*tab, (u8)*p);
    ns_array_push(*tab, 0);
    return off;
}

static u32 ns_elf_str_push_ns(u8 **tab, ns_str name) {
    u32 off = (u32)ns_array_length(*tab);
    for (i32 i = 0; i < name.len; ++i) ns_array_push(*tab, (u8)name.data[i]);
    ns_array_push(*tab, 0);
    return off;
}

ns_return_bool ns_elf_emit_object(ns_ssa_module *ssa, ns_str output_path) {
    if (!ssa) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "null ssa module");
    }
    if (ns_str_is_empty(output_path)) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "empty output path");
    }

    ns_return_ptr amd64_ret = ns_amd64_from_ssa(ssa);
    if (ns_return_is_error(amd64_ret)) {
        return ns_return_change_type(bool, amd64_ret);
    }

    ns_amd64_module_bin *amd64 = amd64_ret.r;
    i32 nfns = (i32)ns_array_length(amd64->fns);
    if (nfns == 0) {
        ns_amd64_free(amd64);
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "no functions for elf object emit");
    }

    /* ── lay the functions out inside one .text section ──────────────────── */
    u64 *fn_off = ns_null; /* section-relative offset of each function */
    ns_array_set_length(fn_off, nfns);
    u64 cursor = 0;
    for (i32 i = 0; i < nfns; ++i) {
        cursor = ns_elf_align_up(cursor, 16);
        fn_off[i] = cursor;
        cursor += ns_array_length(amd64->fns[i].text);
    }
    u64 text_size = cursor;

    /* ── resolve every fixup to a symbol, collecting the undefined ones ──── */
    typedef struct { i32 fi; i32 ci; i32 sym; u8 kind; } ns_elf_fixup;
    ns_str *undef_names = ns_null;
    ns_elf_fixup *relocs = ns_null;
    for (i32 fi = 0; fi < nfns; ++fi) {
        ns_amd64_fn_bin *fn = &amd64->fns[fi];
        for (i32 ci = 0, ncf = (i32)ns_array_length(fn->call_fixups); ci < ncf; ++ci) {
            ns_amd64_call_fixup *cf = &fn->call_fixups[ci];
            i32 local = -1;
            for (i32 k = 0; k < nfns; ++k) {
                if (ns_str_equals(amd64->fns[k].name, cf->callee)) { local = k; break; }
            }
            if (local >= 0 && cf->kind == 0) continue; /* patched in place below */
            i32 sym;
            if (local >= 0) {
                sym = 1 + local;
            } else {
                i32 ui = -1;
                for (i32 u = 0, ul = (i32)ns_array_length(undef_names); u < ul; ++u) {
                    if (ns_str_equals(undef_names[u], cf->callee)) { ui = u; break; }
                }
                if (ui < 0) {
                    ui = (i32)ns_array_length(undef_names);
                    ns_array_push(undef_names, cf->callee);
                }
                sym = 1 + nfns + ui;
            }
            ns_elf_fixup rel = {.fi = fi, .ci = ci, .sym = sym, .kind = cf->kind};
            ns_array_push(relocs, rel);
        }
    }
    i32 nundef = (i32)ns_array_length(undef_names);
    i32 nreloc = (i32)ns_array_length(relocs);

    /* ── string tables ───────────────────────────────────────────────────── */
    u8 *strtab = ns_null;
    ns_array_push(strtab, 0);
    u32 *fn_name_off = ns_null;
    ns_array_set_length(fn_name_off, nfns);
    for (i32 i = 0; i < nfns; ++i) fn_name_off[i] = ns_elf_str_push_ns(&strtab, amd64->fns[i].name);
    u32 *undef_name_off = ns_null;
    if (nundef > 0) ns_array_set_length(undef_name_off, nundef);
    for (i32 i = 0; i < nundef; ++i) undef_name_off[i] = ns_elf_str_push_ns(&strtab, undef_names[i]);

    u8 *shstrtab = ns_null;
    ns_array_push(shstrtab, 0);
    u32 name_text = ns_elf_str_push(&shstrtab, ".text");
    u32 name_rela = ns_elf_str_push(&shstrtab, ".rela.text");
    u32 name_symtab = ns_elf_str_push(&shstrtab, ".symtab");
    u32 name_strtab = ns_elf_str_push(&shstrtab, ".strtab");
    u32 name_shstrtab = ns_elf_str_push(&shstrtab, ".shstrtab");
    /* An empty, non-executable .note.GNU-stack keeps the linker from marking
     * the program's stack executable. */
    u32 name_note = ns_elf_str_push(&shstrtab, ".note.GNU-stack");

    /* ── file layout ─────────────────────────────────────────────────────── */
    u32 nsyms = 1 + (u32)nfns + (u32)nundef;
    u64 text_off = ns_elf_align_up(sizeof(ns_elf_header), 16);
    u64 rela_off = ns_elf_align_up(text_off + text_size, 8);
    u64 rela_size = (u64)nreloc * sizeof(ns_elf_rela);
    u64 symtab_off = ns_elf_align_up(rela_off + rela_size, 8);
    u64 symtab_size = (u64)nsyms * sizeof(ns_elf_sym);
    u64 strtab_off = symtab_off + symtab_size;
    u64 strtab_size = ns_array_length(strtab);
    u64 shstrtab_off = strtab_off + strtab_size;
    u64 shstrtab_size = ns_array_length(shstrtab);
    u64 shdr_off = ns_elf_align_up(shstrtab_off + shstrtab_size, 8);
    const u16 nsections = 7;
    u64 file_size = shdr_off + (u64)nsections * sizeof(ns_elf_section);

    u8 *out = ns_null;
    ns_array_set_length(out, file_size);
    memset(out, 0, file_size);

    /* ── ELF header ──────────────────────────────────────────────────────── */
    ns_elf_header header = {0};
    header.e_ident[0] = 0x7F;
    header.e_ident[1] = 'E';
    header.e_ident[2] = 'L';
    header.e_ident[3] = 'F';
    header.e_ident[4] = 2; /* ELFCLASS64 */
    header.e_ident[5] = 1; /* ELFDATA2LSB */
    header.e_ident[6] = 1; /* EV_CURRENT */
    header.e_type = 1;     /* ET_REL */
    header.e_machine = 62; /* EM_X86_64 */
    header.e_version = 1;
    header.e_shoff = shdr_off;
    header.e_ehsize = (u16)sizeof(ns_elf_header);
    header.e_shentsize = (u16)sizeof(ns_elf_section);
    header.e_shnum = nsections;
    header.e_shstrndx = 5;
    memcpy(&out[0], &header, sizeof(header));

    /* ── .text ───────────────────────────────────────────────────────────── */
    for (i32 i = 0; i < nfns; ++i) {
        u64 sz = ns_array_length(amd64->fns[i].text);
        if (sz > 0) memcpy(&out[text_off + fn_off[i]], amd64->fns[i].text, sz);
    }

    /* Calls that stay inside this object resolve to a plain rel32. */
    for (i32 fi = 0; fi < nfns; ++fi) {
        ns_amd64_fn_bin *fn = &amd64->fns[fi];
        for (i32 ci = 0, ncf = (i32)ns_array_length(fn->call_fixups); ci < ncf; ++ci) {
            ns_amd64_call_fixup *cf = &fn->call_fixups[ci];
            if (cf->kind != 0) continue;
            i32 callee = -1;
            for (i32 k = 0; k < nfns; ++k) {
                if (ns_str_equals(amd64->fns[k].name, cf->callee)) { callee = k; break; }
            }
            if (callee < 0) continue;
            i64 call_end = (i64)(fn_off[fi] + cf->off + 4);
            i64 target = (i64)fn_off[callee];
            i32 rel32 = (i32)(target - call_end);
            u64 patch = text_off + fn_off[fi] + cf->off;
            out[patch + 0] = (u8)((u32)rel32 & 0xFF);
            out[patch + 1] = (u8)(((u32)rel32 >> 8) & 0xFF);
            out[patch + 2] = (u8)(((u32)rel32 >> 16) & 0xFF);
            out[patch + 3] = (u8)(((u32)rel32 >> 24) & 0xFF);
        }
    }

    /* ── .rela.text ──────────────────────────────────────────────────────── */
    for (i32 i = 0; i < nreloc; ++i) {
        ns_elf_fixup *rel = &relocs[i];
        ns_amd64_call_fixup *cf = &amd64->fns[rel->fi].call_fixups[rel->ci];
        ns_elf_rela entry = {0};
        entry.r_offset = fn_off[rel->fi] + cf->off;
        /* Both forms name the end of the instruction, four bytes past the
         * field the linker writes. */
        entry.r_addend = -4;
        entry.r_info = ns_elf_reloc_info((u32)rel->sym,
            rel->kind == 1 ? NS_ELF_R_X86_64_PC32 : NS_ELF_R_X86_64_PLT32);
        memcpy(&out[rela_off + (u64)i * sizeof(entry)], &entry, sizeof(entry));
    }

    /* ── .symtab ─────────────────────────────────────────────────────────── */
    for (i32 i = 0; i < nfns; ++i) {
        ns_elf_sym sym = {0};
        sym.st_name = fn_name_off[i];
        sym.st_info = (u8)((NS_ELF_STB_GLOBAL << 4) | NS_ELF_STT_FUNC);
        sym.st_shndx = NS_ELF_SHN_TEXT;
        sym.st_value = fn_off[i];
        sym.st_size = ns_array_length(amd64->fns[i].text);
        memcpy(&out[symtab_off + (u64)(1 + i) * sizeof(sym)], &sym, sizeof(sym));
    }
    for (i32 i = 0; i < nundef; ++i) {
        ns_elf_sym sym = {0};
        sym.st_name = undef_name_off[i];
        sym.st_info = (u8)((NS_ELF_STB_GLOBAL << 4) | NS_ELF_STT_NOTYPE);
        sym.st_shndx = NS_ELF_SHN_UNDEF;
        memcpy(&out[symtab_off + (u64)(1 + nfns + i) * sizeof(sym)], &sym, sizeof(sym));
    }

    memcpy(&out[strtab_off], strtab, strtab_size);
    memcpy(&out[shstrtab_off], shstrtab, shstrtab_size);

    /* ── section headers ─────────────────────────────────────────────────── */
    ns_elf_section sections[7];
    memset(sections, 0, sizeof(sections));

    sections[1].sh_name = name_text;
    sections[1].sh_type = NS_ELF_SHT_PROGBITS;
    sections[1].sh_flags = NS_ELF_SHF_ALLOC | NS_ELF_SHF_EXECINSTR;
    sections[1].sh_offset = text_off;
    sections[1].sh_size = text_size;
    sections[1].sh_addralign = 16;

    sections[2].sh_name = name_rela;
    sections[2].sh_type = NS_ELF_SHT_RELA;
    sections[2].sh_flags = NS_ELF_SHF_INFO_LINK;
    sections[2].sh_offset = rela_off;
    sections[2].sh_size = rela_size;
    sections[2].sh_link = 3; /* .symtab */
    sections[2].sh_info = NS_ELF_SHN_TEXT;
    sections[2].sh_addralign = 8;
    sections[2].sh_entsize = sizeof(ns_elf_rela);

    sections[3].sh_name = name_symtab;
    sections[3].sh_type = NS_ELF_SHT_SYMTAB;
    sections[3].sh_offset = symtab_off;
    sections[3].sh_size = symtab_size;
    sections[3].sh_link = 4; /* .strtab */
    sections[3].sh_info = 1; /* one local symbol: the reserved null entry */
    sections[3].sh_addralign = 8;
    sections[3].sh_entsize = sizeof(ns_elf_sym);

    sections[4].sh_name = name_strtab;
    sections[4].sh_type = NS_ELF_SHT_STRTAB;
    sections[4].sh_offset = strtab_off;
    sections[4].sh_size = strtab_size;
    sections[4].sh_addralign = 1;

    sections[5].sh_name = name_shstrtab;
    sections[5].sh_type = NS_ELF_SHT_STRTAB;
    sections[5].sh_offset = shstrtab_off;
    sections[5].sh_size = shstrtab_size;
    sections[5].sh_addralign = 1;

    sections[6].sh_name = name_note;
    sections[6].sh_type = NS_ELF_SHT_PROGBITS;
    sections[6].sh_offset = shstrtab_off + shstrtab_size;
    sections[6].sh_size = 0;
    sections[6].sh_addralign = 1;

    memcpy(&out[shdr_off], sections, sizeof(sections));

    ns_return_bool wr = ns_elf_write_file(output_path, out);

    ns_array_free(out);
    ns_array_free(fn_off);
    ns_array_free(fn_name_off);
    ns_array_free(undef_name_off);
    ns_array_free(undef_names);
    ns_array_free(relocs);
    ns_array_free(strtab);
    ns_array_free(shstrtab);
    ns_amd64_free(amd64);
    return wr;
}
