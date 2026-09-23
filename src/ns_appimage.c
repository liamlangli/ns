#include "ns_appimage.h"

#include <stdarg.h>

#if !defined(_WIN32)

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(NS_ZLIB)
#include "zlib.h"
#endif

/*
 * squashfs 4.0 writer. Every value is little-endian.
 *
 *   Offset  Content
 *   0       superblock (96 bytes)
 *   96      file data blocks, one run per regular file
 *   ...     inode table      (8 KiB metadata blocks)
 *   ...     directory table  (8 KiB metadata blocks)
 *   ...     id table         (one metadata block holding uid/gid 0)
 *   ...     id index         (u64 position of that block)
 *   ...     zero padding to 4 KiB
 *
 * Inodes are numbered and written in post-order, so a directory's listing can
 * name the inode references of every child and the root is written last.
 */

#define NS_SQFS_MAGIC 0x73717368u
#define NS_SQFS_BLOCK_SIZE 131072u
#define NS_SQFS_BLOCK_LOG 17
#define NS_SQFS_META_SIZE 8192u
#define NS_SQFS_META_RAW 0x8000u
#define NS_SQFS_BLOCK_RAW (1u << 24)
#define NS_SQFS_INVALID 0xFFFFFFFFFFFFFFFFull
#define NS_SQFS_COMP_GZIP 1

#define NS_SQFS_FLAG_UNCOMPRESSED_INODES 0x0001
#define NS_SQFS_FLAG_UNCOMPRESSED_DATA 0x0002
#define NS_SQFS_FLAG_NO_FRAGMENTS 0x0010
#define NS_SQFS_FLAG_NO_XATTRS 0x0200

#define NS_SQFS_DIR 1
#define NS_SQFS_FILE 2
#define NS_SQFS_SYMLINK 3
#define NS_SQFS_LDIR 8

#define NS_SQFS_DIR_HEADER_MAX 256

typedef struct ns_sqfs_buf {
    u8 *data;
    szt len;
    szt cap;
} ns_sqfs_buf;

typedef struct ns_sqfs_meta {
    ns_sqfs_buf out;
    u8 pending[NS_SQFS_META_SIZE];
    u32 pending_len;
} ns_sqfs_meta;

typedef struct ns_sqfs_node {
    char *name;
    char *path;
    u16 type;
    u16 mode;
    struct ns_sqfs_node *children;
    u32 child_count;
    u32 subdirs;
    u32 inode_number;
    u64 inode_ref;
    u64 size;
    u32 blocks_start;
    u32 *blocks;
    u32 block_count;
    char *target;
} ns_sqfs_node;

typedef struct ns_sqfs_writer {
    ns_sqfs_buf image;
    ns_sqfs_meta inodes;
    ns_sqfs_meta dirs;
    u32 inode_count;
    u8 *block_in;
    u8 *block_out;
    szt block_out_cap;
    char *error;
    szt error_len;
    ns_bool failed;
} ns_sqfs_writer;

static void ns_sqfs_fail(ns_sqfs_writer *w, const char *fmt, ...) {
    if (w->failed) return;
    w->failed = true;
    va_list args;
    va_start(args, fmt);
    vsnprintf(w->error, w->error_len, fmt, args);
    va_end(args);
}

static void ns_sqfs_put(ns_sqfs_buf *b, const void *data, szt len) {
    if (b->len + len > b->cap) {
        szt cap = b->cap ? b->cap : 4096;
        while (cap < b->len + len) cap *= 2;
        b->data = realloc(b->data, cap);
        if (b->data == ns_null) {
            fprintf(stderr, "ns: out of memory packing AppImage\n");
            exit(1);
        }
        b->cap = cap;
    }
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void ns_sqfs_u16(ns_sqfs_buf *b, u16 v) {
    u8 bytes[2] = {(u8)v, (u8)(v >> 8)};
    ns_sqfs_put(b, bytes, 2);
}

static void ns_sqfs_u32(ns_sqfs_buf *b, u32 v) {
    u8 bytes[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    ns_sqfs_put(b, bytes, 4);
}

static void ns_sqfs_u64(ns_sqfs_buf *b, u64 v) {
    ns_sqfs_u32(b, (u32)v);
    ns_sqfs_u32(b, (u32)(v >> 32));
}

static void ns_sqfs_patch_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void ns_sqfs_patch_u32(u8 *p, u32 v) {
    for (i32 i = 0; i < 4; ++i) p[i] = (u8)(v >> (8 * i));
}

static void ns_sqfs_patch_u64(u8 *p, u64 v) {
    for (i32 i = 0; i < 8; ++i) p[i] = (u8)(v >> (8 * i));
}

// Compress `len` bytes of `in` into w->block_out. Returns the compressed size,
// or 0 when compression is unavailable or does not shrink the data.
static szt ns_sqfs_compress(ns_sqfs_writer *w, const u8 *in, szt len) {
#if defined(NS_ZLIB)
    uLongf out_len = (uLongf)w->block_out_cap;
    if (compress2(w->block_out, &out_len, in, (uLong)len, Z_BEST_COMPRESSION) != Z_OK) return 0;
    return out_len < len ? (szt)out_len : 0;
#else
    (void)w;
    (void)in;
    (void)len;
    return 0;
#endif
}

static void ns_sqfs_meta_flush(ns_sqfs_writer *w, ns_sqfs_meta *m) {
    if (m->pending_len == 0) return;
    szt packed = ns_sqfs_compress(w, m->pending, m->pending_len);
    if (packed) {
        ns_sqfs_u16(&m->out, (u16)packed);
        ns_sqfs_put(&m->out, w->block_out, packed);
    } else {
        ns_sqfs_u16(&m->out, (u16)(m->pending_len | NS_SQFS_META_RAW));
        ns_sqfs_put(&m->out, m->pending, m->pending_len);
    }
    m->pending_len = 0;
}

// Reference to the next byte written: the metadata block's position within
// its table, shifted 16, plus the offset inside the uncompressed block.
static u64 ns_sqfs_meta_ref(ns_sqfs_meta *m) {
    return ((u64)m->out.len << 16) | m->pending_len;
}

static void ns_sqfs_meta_put(ns_sqfs_writer *w, ns_sqfs_meta *m, const u8 *data, szt len) {
    while (len) {
        szt n = NS_SQFS_META_SIZE - m->pending_len;
        if (n > len) n = len;
        memcpy(m->pending + m->pending_len, data, n);
        m->pending_len += (u32)n;
        data += n;
        len -= n;
        if (m->pending_len == NS_SQFS_META_SIZE) ns_sqfs_meta_flush(w, m);
    }
}

static char *ns_sqfs_strdup(const char *s) {
    szt len = strlen(s);
    char *copy = malloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

static char *ns_sqfs_join(const char *dir, const char *name) {
    szt a = strlen(dir), b = strlen(name);
    char *path = malloc(a + b + 2);
    memcpy(path, dir, a);
    path[a] = '/';
    memcpy(path + a + 1, name, b + 1);
    return path;
}

static int ns_sqfs_node_compare(const void *a, const void *b) {
    return strcmp(((const ns_sqfs_node *)a)->name, ((const ns_sqfs_node *)b)->name);
}

static void ns_sqfs_scan(ns_sqfs_writer *w, ns_sqfs_node *node) {
    struct stat st;
    if (lstat(node->path, &st) != 0) {
        ns_sqfs_fail(w, "cannot stat %s: %s", node->path, strerror(errno));
        return;
    }
    node->mode = (u16)(st.st_mode & 07777);
    if (S_ISLNK(st.st_mode)) {
        node->type = NS_SQFS_SYMLINK;
        szt cap = (szt)st.st_size + 1;
        node->target = malloc(cap);
        ssize_t n = readlink(node->path, node->target, cap);
        if (n < 0 || (szt)n >= cap) {
            ns_sqfs_fail(w, "cannot read link %s", node->path);
            node->target[0] = '\0';
            return;
        }
        node->target[n] = '\0';
        return;
    }
    if (S_ISREG(st.st_mode)) {
        node->type = NS_SQFS_FILE;
        node->size = (u64)st.st_size;
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        ns_sqfs_fail(w, "unsupported file type in AppDir: %s", node->path);
        return;
    }
    node->type = NS_SQFS_DIR;
    DIR *dir = opendir(node->path);
    if (dir == ns_null) {
        ns_sqfs_fail(w, "cannot open directory %s: %s", node->path, strerror(errno));
        return;
    }
    u32 cap = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != ns_null) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (node->child_count == cap) {
            cap = cap ? cap * 2 : 8;
            node->children = realloc(node->children, cap * sizeof(ns_sqfs_node));
        }
        ns_sqfs_node *child = &node->children[node->child_count++];
        memset(child, 0, sizeof(*child));
        child->name = ns_sqfs_strdup(entry->d_name);
        child->path = ns_sqfs_join(node->path, entry->d_name);
    }
    closedir(dir);
    if (node->child_count > 1) qsort(node->children, node->child_count, sizeof(ns_sqfs_node), ns_sqfs_node_compare);
    for (u32 i = 0; i < node->child_count && !w->failed; ++i) {
        ns_sqfs_scan(w, &node->children[i]);
        if (node->children[i].type == NS_SQFS_DIR) node->subdirs++;
    }
}

static void ns_sqfs_number(ns_sqfs_writer *w, ns_sqfs_node *node) {
    for (u32 i = 0; i < node->child_count; ++i) ns_sqfs_number(w, &node->children[i]);
    node->inode_number = ++w->inode_count;
}

static void ns_sqfs_write_data(ns_sqfs_writer *w, ns_sqfs_node *node) {
    if (node->type == NS_SQFS_DIR) {
        for (u32 i = 0; i < node->child_count && !w->failed; ++i) ns_sqfs_write_data(w, &node->children[i]);
        return;
    }
    if (node->type != NS_SQFS_FILE) return;
    if (node->size > 0xFFFFFFFFull || w->image.len > 0xFFFFFFFFull) {
        ns_sqfs_fail(w, "AppImage contents exceed 4 GiB at %s", node->path);
        return;
    }
    node->blocks_start = (u32)w->image.len;
    node->block_count = (u32)((node->size + NS_SQFS_BLOCK_SIZE - 1) / NS_SQFS_BLOCK_SIZE);
    if (node->block_count == 0) return;
    node->blocks = malloc(node->block_count * sizeof(u32));
    FILE *f = fopen(node->path, "rb");
    if (f == ns_null) {
        ns_sqfs_fail(w, "cannot open %s: %s", node->path, strerror(errno));
        return;
    }
    u64 remaining = node->size;
    for (u32 i = 0; i < node->block_count; ++i) {
        szt want = remaining < NS_SQFS_BLOCK_SIZE ? (szt)remaining : NS_SQFS_BLOCK_SIZE;
        if (fread(w->block_in, 1, want, f) != want) {
            ns_sqfs_fail(w, "cannot read %s", node->path);
            break;
        }
        szt packed = ns_sqfs_compress(w, w->block_in, want);
        if (packed) {
            ns_sqfs_put(&w->image, w->block_out, packed);
            node->blocks[i] = (u32)packed;
        } else {
            ns_sqfs_put(&w->image, w->block_in, want);
            node->blocks[i] = (u32)want | NS_SQFS_BLOCK_RAW;
        }
        remaining -= want;
    }
    fclose(f);
}

static void ns_sqfs_inode_header(ns_sqfs_buf *b, u16 type, ns_sqfs_node *node) {
    ns_sqfs_u16(b, type);
    ns_sqfs_u16(b, node->mode);
    ns_sqfs_u16(b, 0); // uid index
    ns_sqfs_u16(b, 0); // gid index
    ns_sqfs_u32(b, 0); // mtime: fixed, so identical inputs pack identically
    ns_sqfs_u32(b, node->inode_number);
}

// A listing is a run of headers, each followed by up to 256 entries that share
// one inode metadata block and lie within an i16 of the header's inode number.
static void ns_sqfs_write_listing(ns_sqfs_buf *b, ns_sqfs_node *dir) {
    u32 i = 0;
    while (i < dir->child_count) {
        u32 block = (u32)(dir->children[i].inode_ref >> 16);
        u32 base = dir->children[i].inode_number;
        u32 count = 0;
        while (i + count < dir->child_count && count < NS_SQFS_DIR_HEADER_MAX) {
            ns_sqfs_node *child = &dir->children[i + count];
            i64 delta = (i64)child->inode_number - (i64)base;
            if ((u32)(child->inode_ref >> 16) != block || delta < -32768 || delta > 32767) break;
            count++;
        }
        ns_sqfs_u32(b, count - 1);
        ns_sqfs_u32(b, block);
        ns_sqfs_u32(b, base);
        for (u32 k = 0; k < count; ++k) {
            ns_sqfs_node *child = &dir->children[i + k];
            szt name_len = strlen(child->name);
            ns_sqfs_u16(b, (u16)(child->inode_ref & 0xFFFF));
            ns_sqfs_u16(b, (u16)(i16)((i64)child->inode_number - (i64)base));
            ns_sqfs_u16(b, child->type);
            ns_sqfs_u16(b, (u16)(name_len - 1));
            ns_sqfs_put(b, child->name, name_len);
        }
        i += count;
    }
}

static void ns_sqfs_write_inode(ns_sqfs_writer *w, ns_sqfs_node *node, u32 parent) {
    ns_sqfs_buf b = {0};
    if (node->type == NS_SQFS_DIR) {
        for (u32 i = 0; i < node->child_count; ++i) ns_sqfs_write_inode(w, &node->children[i], node->inode_number);
        u64 listing_ref = ns_sqfs_meta_ref(&w->dirs);
        ns_sqfs_buf listing = {0};
        ns_sqfs_write_listing(&listing, node);
        ns_sqfs_meta_put(w, &w->dirs, listing.data, listing.len);
        // The recorded size counts three bytes for the implicit "." and "..".
        u64 size = listing.len + 3;
        free(listing.data);
        if (size <= 0xFFFF) {
            ns_sqfs_inode_header(&b, NS_SQFS_DIR, node);
            ns_sqfs_u32(&b, (u32)(listing_ref >> 16));
            ns_sqfs_u32(&b, 2 + node->subdirs);
            ns_sqfs_u16(&b, (u16)size);
            ns_sqfs_u16(&b, (u16)(listing_ref & 0xFFFF));
            ns_sqfs_u32(&b, parent);
        } else {
            ns_sqfs_inode_header(&b, NS_SQFS_LDIR, node);
            ns_sqfs_u32(&b, 2 + node->subdirs);
            ns_sqfs_u32(&b, (u32)size);
            ns_sqfs_u32(&b, (u32)(listing_ref >> 16));
            ns_sqfs_u32(&b, parent);
            ns_sqfs_u16(&b, 0); // no directory index
            ns_sqfs_u16(&b, (u16)(listing_ref & 0xFFFF));
            ns_sqfs_u32(&b, 0xFFFFFFFFu); // no xattrs
        }
    } else if (node->type == NS_SQFS_FILE) {
        ns_sqfs_inode_header(&b, NS_SQFS_FILE, node);
        ns_sqfs_u32(&b, node->blocks_start);
        ns_sqfs_u32(&b, 0xFFFFFFFFu); // no fragment
        ns_sqfs_u32(&b, 0);
        ns_sqfs_u32(&b, (u32)node->size);
        for (u32 i = 0; i < node->block_count; ++i) ns_sqfs_u32(&b, node->blocks[i]);
    } else {
        szt target_len = strlen(node->target);
        ns_sqfs_inode_header(&b, NS_SQFS_SYMLINK, node);
        ns_sqfs_u32(&b, 1);
        ns_sqfs_u32(&b, (u32)target_len);
        ns_sqfs_put(&b, node->target, target_len);
    }
    node->inode_ref = ns_sqfs_meta_ref(&w->inodes);
    ns_sqfs_meta_put(w, &w->inodes, b.data, b.len);
    free(b.data);
}

static void ns_sqfs_free_node(ns_sqfs_node *node) {
    for (u32 i = 0; i < node->child_count; ++i) ns_sqfs_free_node(&node->children[i]);
    free(node->children);
    free(node->name);
    free(node->path);
    free(node->blocks);
    free(node->target);
}

// Pack `dir` into w->image. The image is padded to a 4 KiB multiple.
static void ns_sqfs_pack(ns_sqfs_writer *w, const char *dir) {
    ns_sqfs_node root = {0};
    root.name = ns_sqfs_strdup("");
    root.path = ns_sqfs_strdup(dir);
    ns_sqfs_scan(w, &root);
    if (!w->failed && root.type != NS_SQFS_DIR) ns_sqfs_fail(w, "AppDir is not a directory: %s", dir);
    if (w->failed) {
        ns_sqfs_free_node(&root);
        return;
    }
    ns_sqfs_number(w, &root);

    u8 superblock[96] = {0};
    ns_sqfs_put(&w->image, superblock, sizeof(superblock));
    ns_sqfs_write_data(w, &root);
    if (w->failed) {
        ns_sqfs_free_node(&root);
        return;
    }
    ns_sqfs_write_inode(w, &root, w->inode_count + 1);
    ns_sqfs_meta_flush(w, &w->inodes);
    ns_sqfs_meta_flush(w, &w->dirs);

    u64 inode_table = w->image.len;
    ns_sqfs_put(&w->image, w->inodes.out.data, w->inodes.out.len);
    u64 directory_table = w->image.len;
    ns_sqfs_put(&w->image, w->dirs.out.data, w->dirs.out.len);
    // No fragments: the fragment table position only bounds the directory
    // table for readers that check table order.
    u64 fragment_table = w->image.len;
    ns_sqfs_meta ids = {0};
    u8 root_id[4] = {0};
    ns_sqfs_meta_put(w, &ids, root_id, sizeof(root_id));
    ns_sqfs_meta_flush(w, &ids);
    u64 id_block = w->image.len;
    ns_sqfs_put(&w->image, ids.out.data, ids.out.len);
    free(ids.out.data);
    u64 id_table = w->image.len;
    ns_sqfs_u64(&w->image, id_block);
    u64 bytes_used = w->image.len;

    u16 flags = NS_SQFS_FLAG_NO_FRAGMENTS | NS_SQFS_FLAG_NO_XATTRS;
#if !defined(NS_ZLIB)
    flags |= NS_SQFS_FLAG_UNCOMPRESSED_INODES | NS_SQFS_FLAG_UNCOMPRESSED_DATA;
#endif
    u8 *sb = w->image.data;
    ns_sqfs_patch_u32(sb + 0, NS_SQFS_MAGIC);
    ns_sqfs_patch_u32(sb + 4, w->inode_count);
    ns_sqfs_patch_u32(sb + 8, 0); // mkfs time
    ns_sqfs_patch_u32(sb + 12, NS_SQFS_BLOCK_SIZE);
    ns_sqfs_patch_u32(sb + 16, 0); // fragment count
    ns_sqfs_patch_u16(sb + 20, NS_SQFS_COMP_GZIP);
    ns_sqfs_patch_u16(sb + 22, NS_SQFS_BLOCK_LOG);
    ns_sqfs_patch_u16(sb + 24, flags);
    ns_sqfs_patch_u16(sb + 26, 1); // id count
    ns_sqfs_patch_u16(sb + 28, 4);
    ns_sqfs_patch_u16(sb + 30, 0);
    ns_sqfs_patch_u64(sb + 32, root.inode_ref);
    ns_sqfs_patch_u64(sb + 40, bytes_used);
    ns_sqfs_patch_u64(sb + 48, id_table);
    ns_sqfs_patch_u64(sb + 56, NS_SQFS_INVALID); // xattr table
    ns_sqfs_patch_u64(sb + 64, inode_table);
    ns_sqfs_patch_u64(sb + 72, directory_table);
    ns_sqfs_patch_u64(sb + 80, fragment_table);
    ns_sqfs_patch_u64(sb + 88, NS_SQFS_INVALID); // export table

    static const u8 zeros[4096] = {0};
    szt pad = (4096 - w->image.len % 4096) % 4096;
    ns_sqfs_put(&w->image, zeros, pad);
    ns_sqfs_free_node(&root);
}

// The image starts where the runtime ELF ends: after its section header
// table, the offset AppImage tools compute for a type 2 image.
static ns_bool ns_appimage_runtime_end(const u8 *elf, szt len, u64 *end) {
    if (len < 64 || memcmp(elf, "\x7f" "ELF", 4) != 0 || elf[4] != 2 || elf[5] != 1) return false;
    u64 shoff = 0;
    for (i32 i = 0; i < 8; ++i) shoff |= (u64)elf[40 + i] << (8 * i);
    u16 shentsize = (u16)(elf[58] | (elf[59] << 8));
    u16 shnum = (u16)(elf[60] | (elf[61] << 8));
    *end = shoff + (u64)shentsize * shnum;
    return *end == len;
}

static u8 *ns_appimage_read_file(const char *path, szt *len) {
    FILE *f = fopen(path, "rb");
    if (f == ns_null) return ns_null;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = size > 0 ? malloc((szt)size) : ns_null;
    if (data && fread(data, 1, (szt)size, f) != (szt)size) {
        free(data);
        data = ns_null;
    }
    fclose(f);
    *len = data ? (szt)size : 0;
    return data;
}

ns_bool ns_appimage_write(const char *runtime, const char *app_dir, const char *name,
                          const char *output, char *error, szt error_len) {
    szt runtime_len = 0;
    u8 *elf = ns_appimage_read_file(runtime, &runtime_len);
    u64 runtime_end = 0;
    if (elf == ns_null) {
        snprintf(error, error_len, "cannot read AppImage runtime %s", runtime);
        return false;
    }
    if (!ns_appimage_runtime_end(elf, runtime_len, &runtime_end)) {
        snprintf(error, error_len, "AppImage runtime %s is not an ELF64 file ending in its section headers", runtime);
        free(elf);
        return false;
    }
    // AppImage type 2 magic in the ELF identification padding.
    elf[8] = 'A';
    elf[9] = 'I';
    elf[10] = 2;

    ns_sqfs_writer w = {0};
    w.error = error;
    w.error_len = error_len;
    w.block_in = malloc(NS_SQFS_BLOCK_SIZE);
#if defined(NS_ZLIB)
    w.block_out_cap = compressBound(NS_SQFS_BLOCK_SIZE);
#else
    w.block_out_cap = NS_SQFS_BLOCK_SIZE;
#endif
    w.block_out = malloc(w.block_out_cap);
    ns_sqfs_pack(&w, app_dir);
    free(w.block_in);
    free(w.block_out);
    free(w.inodes.out.data);
    free(w.dirs.out.data);

    ns_bool ok = !w.failed;
    if (ok) {
        u8 trailer[NS_APPIMAGE_TRAILER_SIZE] = {0};
        memcpy(trailer, NS_APPIMAGE_TRAILER_MAGIC, 8);
        u64 hash = 1469598103934665603ull;
        for (szt i = 0; i < w.image.len; ++i) {
            hash ^= w.image.data[i];
            hash *= 1099511628211ull;
        }
        ns_sqfs_patch_u64(trailer + 8, hash);
        for (i32 i = 0; name[i] != '\0' && i < NS_APPIMAGE_NAME_MAX - 1; ++i) {
            char c = name[i];
            ns_bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '.' || c == '_' || c == '-';
            trailer[16 + i] = (u8)(plain && !(i == 0 && c == '.') ? c : '_');
        }
        if (trailer[16] == 0) memcpy(trailer + 16, "app", 3);

        FILE *f = fopen(output, "wb");
        ok = f != ns_null &&
             fwrite(elf, 1, runtime_len, f) == runtime_len &&
             fwrite(w.image.data, 1, w.image.len, f) == w.image.len &&
             fwrite(trailer, 1, sizeof(trailer), f) == sizeof(trailer);
        if (f != ns_null && fclose(f) != 0) ok = false;
        if (ok && chmod(output, 0755) != 0) ok = false;
        if (!ok) snprintf(error, error_len, "cannot write %s: %s", output, strerror(errno));
    }
    free(elf);
    free(w.image.data);
    return ok;
}

#else

ns_bool ns_appimage_write(const char *runtime, const char *app_dir, const char *name,
                          const char *output, char *error, szt error_len) {
    (void)runtime;
    (void)app_dir;
    (void)name;
    (void)output;
    snprintf(error, error_len, "AppImage packaging needs a POSIX host");
    return false;
}

#endif
