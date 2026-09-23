// ns-appimage-runtime: the ELF at the front of every AppImage `ns build`
// writes (see include/ns_appimage.h for the file layout). It is a standalone
// program, built by `make` on Linux and not linked into bin/ns.
//
// Instead of mounting the squashfs image through FUSE, the runtime extracts it
// once into `$XDG_CACHE_HOME/ns-appimage/<name>-<hash>` (falling back to
// ~/.cache, then /tmp) and executes that copy's AppRun. The trailer hash names
// the directory, so later launches start directly and a rebuilt app extracts
// afresh; older extractions of the same app are removed then.
//
//   <app>.AppImage [args...]          run the app
//   <app>.AppImage --appimage-extract extract into ./squashfs-root
//   <app>.AppImage --appimage-offset  print the image offset

#define _GNU_SOURCE

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(NS_ZLIB)
#include "zlib.h"
#endif

// Kept in sync with include/ns_appimage.h.
#define TRAILER_MAGIC "NSAPPIMG"
#define TRAILER_SIZE 64
#define NAME_MAX_LEN 48

#define SQFS_MAGIC 0x73717368u
#define SQFS_META_RAW 0x8000u
#define SQFS_BLOCK_RAW (1u << 24)
#define SQFS_COMP_GZIP 1
#define SQFS_DIR 1
#define SQFS_FILE 2
#define SQFS_SYMLINK 3
#define SQFS_LDIR 8
#define SQFS_LFILE 9

typedef struct table {
    uint8_t *data;       // uncompressed metadata, block after block
    size_t len;
    uint64_t *positions; // each block's position within the on-disk table
    size_t *offsets;     // each block's offset into `data`
    size_t count;
} table;

static const uint8_t *image;
static size_t image_len;
static uint32_t block_size;
static table inodes;
static table dirs;
static uint8_t *block_buffer;

static void die(const char *fmt, const char *arg) {
    fprintf(stderr, "ns-appimage: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(127);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static const uint8_t *image_at(uint64_t pos, uint64_t len) {
    if (pos > image_len || len > image_len - pos) die("%s", "corrupt image: read past its end");
    return image + pos;
}

// Decompress `len` bytes at `src` into `dst` (capacity `cap`). Returns the size.
static size_t inflate_to(uint8_t *dst, size_t cap, const uint8_t *src, size_t len) {
#if defined(NS_ZLIB)
    uLongf out = (uLongf)cap;
    if (uncompress(dst, &out, src, (uLong)len) != Z_OK) die("%s", "corrupt image: zlib data does not inflate");
    return (size_t)out;
#else
    (void)dst;
    (void)cap;
    (void)src;
    (void)len;
    die("%s", "this runtime was built without zlib and cannot read compressed data");
    return 0;
#endif
}

static void table_read(table *t, uint64_t start, uint64_t end) {
    uint8_t chunk[8192];
    uint64_t pos = start;
    while (pos < end) {
        uint16_t header = rd16(image_at(pos, 2));
        uint32_t size = header & 0x7FFFu;
        const uint8_t *src = image_at(pos + 2, size);
        size_t n = size;
        if (header & SQFS_META_RAW) {
            if (size > sizeof(chunk)) die("%s", "corrupt image: metadata block too large");
            memcpy(chunk, src, size);
        } else {
            n = inflate_to(chunk, sizeof(chunk), src, size);
        }
        t->positions = realloc(t->positions, (t->count + 1) * sizeof(uint64_t));
        t->offsets = realloc(t->offsets, (t->count + 1) * sizeof(size_t));
        t->positions[t->count] = pos - start;
        t->offsets[t->count] = t->len;
        t->count++;
        t->data = realloc(t->data, t->len + n);
        memcpy(t->data + t->len, chunk, n);
        t->len += n;
        pos += 2 + size;
    }
}

// Resolve a metadata reference (block position << 16 | offset) to bytes that
// stay valid for `len` bytes, crossing block boundaries.
static const uint8_t *table_at(const table *t, uint64_t block, uint32_t offset, size_t len) {
    for (size_t i = 0; i < t->count; ++i) {
        if (t->positions[i] != block) continue;
        size_t at = t->offsets[i] + offset;
        if (at > t->len || len > t->len - at) die("%s", "corrupt image: metadata reference out of range");
        return t->data + at;
    }
    die("%s", "corrupt image: unknown metadata block");
    return NULL;
}

static const uint8_t *inode_at(uint64_t ref, size_t len) {
    return table_at(&inodes, ref >> 16, (uint32_t)(ref & 0xFFFF), len);
}

static void write_all(int fd, const uint8_t *data, size_t len, const char *path) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("cannot write %s", path);
        }
        data += n;
        len -= (size_t)n;
    }
}

static void extract_inode(uint64_t ref, const char *path);

static void extract_file(const uint8_t *inode, uint16_t type, const char *path) {
    uint16_t mode = rd16(inode + 2);
    uint64_t start, size;
    const uint8_t *sizes;
    if (type == SQFS_FILE) {
        start = rd32(inode + 16);
        if (rd32(inode + 20) != 0xFFFFFFFFu) die("%s", "fragments are not supported");
        size = rd32(inode + 28);
        sizes = inode + 32;
    } else {
        start = rd64(inode + 16);
        size = rd64(inode + 24);
        if (rd32(inode + 44) != 0xFFFFFFFFu) die("%s", "fragments are not supported");
        sizes = inode + 56;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) die("cannot create %s", path);
    uint64_t blocks = (size + block_size - 1) / block_size;
    uint64_t pos = start;
    uint64_t remaining = size;
    for (uint64_t i = 0; i < blocks; ++i) {
        // The block size list may continue into the next metadata block;
        // table_at returned a contiguous copy, so it is read in place.
        uint32_t field = rd32(sizes + i * 4);
        uint32_t len = field & ~SQFS_BLOCK_RAW;
        size_t want = remaining < block_size ? (size_t)remaining : block_size;
        if (len == 0) {
            memset(block_buffer, 0, want);
        } else if (field & SQFS_BLOCK_RAW) {
            if (len != want) die("corrupt block in %s", path);
            memcpy(block_buffer, image_at(pos, len), len);
        } else if (inflate_to(block_buffer, block_size, image_at(pos, len), len) != want) {
            die("corrupt block in %s", path);
        }
        write_all(fd, block_buffer, want, path);
        pos += len;
        remaining -= want;
    }
    if (fchmod(fd, mode) != 0 || close(fd) != 0) die("cannot finish %s", path);
}

static void extract_dir(const uint8_t *inode, uint16_t type, const char *path) {
    uint16_t mode = rd16(inode + 2);
    uint64_t block;
    uint32_t offset, size;
    if (type == SQFS_DIR) {
        block = rd32(inode + 16);
        size = rd16(inode + 24);
        offset = rd16(inode + 26);
    } else {
        size = rd32(inode + 20);
        block = rd32(inode + 24);
        offset = rd16(inode + 34);
    }
    if (mkdir(path, 0700) != 0 && errno != EEXIST) die("cannot create %s", path);
    if (size < 3) die("%s", "corrupt image: short directory");
    size -= 3;
    const uint8_t *p = size ? table_at(&dirs, block, offset, size) : NULL;
    const uint8_t *end = p + size;
    size_t path_len = strlen(path);
    while (p < end) {
        if (end - p < 12) die("%s", "corrupt image: truncated directory header");
        uint32_t count = rd32(p) + 1;
        uint32_t inode_block = rd32(p + 4);
        p += 12;
        for (uint32_t i = 0; i < count; ++i) {
            if (end - p < 8) die("%s", "corrupt image: truncated directory entry");
            uint16_t inode_offset = rd16(p);
            uint32_t name_len = rd16(p + 6) + 1u;
            if ((size_t)(end - p - 8) < name_len) die("%s", "corrupt image: truncated name");
            const char *name = (const char *)p + 8;
            if (memchr(name, '/', name_len) || (name_len == 1 && name[0] == '.') ||
                (name_len == 2 && name[0] == '.' && name[1] == '.')) {
                die("%s", "corrupt image: unsafe entry name");
            }
            char *child = malloc(path_len + name_len + 2);
            memcpy(child, path, path_len);
            child[path_len] = '/';
            memcpy(child + path_len + 1, name, name_len);
            child[path_len + 1 + name_len] = '\0';
            extract_inode(((uint64_t)inode_block << 16) | inode_offset, child);
            free(child);
            p += 8 + name_len;
        }
    }
    if (chmod(path, mode) != 0) die("cannot set the mode of %s", path);
}

static void extract_inode(uint64_t ref, const char *path) {
    const uint8_t *inode = inode_at(ref, 16);
    uint16_t type = rd16(inode);
    switch (type) {
    case SQFS_DIR:
        extract_dir(inode_at(ref, 32), type, path);
        break;
    case SQFS_LDIR:
        extract_dir(inode_at(ref, 40), type, path);
        break;
    case SQFS_FILE:
    case SQFS_LFILE: {
        size_t fixed = type == SQFS_FILE ? 32 : 56;
        uint64_t size = type == SQFS_FILE ? rd32(inode_at(ref, 32) + 28) : rd64(inode_at(ref, 56) + 24);
        uint64_t blocks = (size + block_size - 1) / block_size;
        extract_file(inode_at(ref, fixed + blocks * 4), type, path);
        break;
    }
    case SQFS_SYMLINK: {
        uint32_t len = rd32(inode_at(ref, 24) + 20);
        const uint8_t *full = inode_at(ref, 24 + (size_t)len);
        char *target = malloc(len + 1);
        memcpy(target, full + 24, len);
        target[len] = '\0';
        unlink(path);
        if (symlink(target, path) != 0) die("cannot create link %s", path);
        free(target);
        break;
    }
    default:
        die("%s", "unsupported inode type in image");
    }
}

static void extract_image(const char *destination) {
    const uint8_t *sb = image_at(0, 96);
    if (rd32(sb) != SQFS_MAGIC || rd16(sb + 28) != 4) die("%s", "the embedded image is not squashfs 4.0");
    if (rd16(sb + 20) != SQFS_COMP_GZIP) die("%s", "the embedded image uses an unsupported compressor");
    if (rd32(sb + 16) != 0) die("%s", "fragments are not supported");
    block_size = rd32(sb + 12);
    if (block_size < 4096 || block_size > (1u << 20)) die("%s", "corrupt image: bad block size");
    uint64_t root = rd64(sb + 32);
    uint64_t id_table = rd64(sb + 48);
    uint64_t inode_table = rd64(sb + 64);
    uint64_t directory_table = rd64(sb + 72);
    uint64_t directory_end = rd64(image_at(id_table, 8));
    uint64_t fragment_table = rd64(sb + 80);
    if (fragment_table > directory_table && fragment_table < directory_end) directory_end = fragment_table;
    if (inode_table >= directory_table || directory_table > directory_end) die("%s", "corrupt image: table order");
    block_buffer = malloc(block_size);
    table_read(&inodes, inode_table, directory_table);
    table_read(&dirs, directory_table, directory_end);
    extract_inode(root, destination);
}

// snprintf into a PATH_MAX buffer, failing instead of truncating.
static void path_format(char *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out, PATH_MAX, fmt, args);
    va_end(args);
    if (n < 0 || n >= PATH_MAX) die("%s", "path too long");
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st;
    (void)flag;
    (void)ftw;
    remove(path);
    return 0;
}

static void remove_tree(const char *path) {
    nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static int mkdir_p(char *path) {
    for (char *p = path + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(path, 0755);
        *p = '/';
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

// The first writable cache base: $XDG_CACHE_HOME, ~/.cache, then /tmp.
static void cache_base(char *out) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg != NULL && xdg[0] == '/') {
        path_format(out, "%s/ns-appimage", xdg);
        if (mkdir_p(out) == 0 && access(out, W_OK) == 0) return;
    }
    if (home != NULL && home[0] == '/') {
        path_format(out, "%s/.cache/ns-appimage", home);
        if (mkdir_p(out) == 0 && access(out, W_OK) == 0) return;
    }
    path_format(out, "/tmp/ns-appimage-%u", (unsigned)getuid());
    if (mkdir_p(out) != 0) die("cannot create %s", out);
}

// Remove extractions of earlier builds of this app.
static void prune(const char *base, const char *name, const char *keep) {
    DIR *dir = opendir(base);
    if (dir == NULL) return;
    size_t name_len = strlen(name);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, name, name_len) != 0 || entry->d_name[name_len] != '-') continue;
        if (strlen(entry->d_name) != name_len + 17 || strcmp(entry->d_name, keep) == 0) continue;
        char path[PATH_MAX];
        path_format(path, "%s/%s", base, entry->d_name);
        remove_tree(path);
    }
    closedir(dir);
}

int main(int argc, char **argv) {
    char self[PATH_MAX];
    ssize_t self_len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (self_len <= 0) die("%s", "cannot locate /proc/self/exe");
    self[self_len] = '\0';
    int fd = open(self, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) die("cannot open %s", self);
    size_t file_len = (size_t)st.st_size;
    const uint8_t *file = mmap(NULL, file_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) die("cannot map %s", self);
    close(fd);

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file;
    uint64_t offset = ehdr->e_shoff + (uint64_t)ehdr->e_shentsize * ehdr->e_shnum;
    if (argc > 1 && strcmp(argv[1], "--appimage-offset") == 0) {
        printf("%llu\n", (unsigned long long)offset);
        return 0;
    }
    if (offset + TRAILER_SIZE > file_len) die("%s carries no image", self);
    const uint8_t *trailer = file + file_len - TRAILER_SIZE;
    if (memcmp(trailer, TRAILER_MAGIC, 8) != 0) die("%s carries no ns image trailer", self);
    image = file + offset;
    image_len = file_len - TRAILER_SIZE - offset;
    char name[NAME_MAX_LEN];
    memcpy(name, trailer + 16, NAME_MAX_LEN);
    name[NAME_MAX_LEN - 1] = '\0';
    if (name[0] == '\0' || name[0] == '.' || strchr(name, '/')) die("%s", "corrupt trailer name");

    if (argc > 1 && strcmp(argv[1], "--appimage-extract") == 0) {
        extract_image("squashfs-root");
        printf("squashfs-root\n");
        return 0;
    }

    char base[PATH_MAX];
    cache_base(base);
    char leaf[NAME_MAX_LEN + 20];
    snprintf(leaf, sizeof(leaf), "%s-%016llx", name, (unsigned long long)rd64(trailer + 8));
    char app_dir[PATH_MAX];
    path_format(app_dir, "%s/%s", base, leaf);
    char app_run[PATH_MAX];
    path_format(app_run, "%s/AppRun", app_dir);

    if (access(app_run, X_OK) != 0) {
        // Extract beside the final directory and rename it into place, so a
        // concurrent or interrupted launch never runs a partial copy.
        char staging[PATH_MAX];
        path_format(staging, "%s/.%s.%ld", base, leaf, (long)getpid());
        remove_tree(staging);
        extract_image(staging);
        if (rename(staging, app_dir) != 0) {
            remove_tree(staging);
            if (access(app_run, X_OK) != 0) die("cannot install %s", app_dir);
        }
        prune(base, name, leaf);
    }
    munmap((void *)file, file_len);

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) setenv("OWD", cwd, 1);
    setenv("APPIMAGE", self, 1);
    setenv("APPDIR", app_dir, 1);
    setenv("ARGV0", argv[0], 1);
    argv[0] = app_run;
    execv(app_run, argv);
    die("cannot execute %s", app_run);
    return 127;
}
