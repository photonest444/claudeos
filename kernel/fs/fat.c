/* FAT12 / FAT16 file system driver (read + write).
 *
 * Layout of a FAT volume:
 *   [boot sector + reserved] [FAT #1] [FAT #2] [root directory] [data clusters]
 * Files and sub-directories are chains of clusters linked through the FAT.
 * Only 8.3 names are supported (long-file-name entries are skipped).
 *
 * All public functions run with interrupts disabled, which makes them atomic
 * with respect to the scheduler (we are a single-CPU kernel). */
#include "../include/fs.h"
#include "../include/string.h"
#include "../include/mm.h"
#include "../include/io.h"
#include "../include/kprintf.h"

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYS    0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

typedef struct __attribute__((packed)) {
    uint8_t  name[11];
    uint8_t  attr, ntres, crt_tenth;
    uint16_t crt_time, crt_date, acc_date, clus_hi, wrt_time, wrt_date, clus_lo;
    uint32_t size;
} dirent_t;

static struct {
    blockdev_t *dev;
    bool     mounted;
    int      type;              /* 12 or 16 */
    uint32_t part_lba;          /* partition offset on the device */
    uint32_t spc, nfats, root_entries, fat_size;
    uint32_t fat_lba, root_lba, root_secs, data_lba, nclusters;
    uint32_t alloc_hint;
} fs;

/* ---- sector cache (write-through) ------------------------------------- */
#define CACHE_N 32
static struct { bool valid; uint32_t lba; uint8_t data[512]; } cache[CACHE_N];
static int cache_next;
static bool io_error;

static uint8_t *bread(uint32_t lba) {
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].valid && cache[i].lba == lba) return cache[i].data;
    int i = cache_next; cache_next = (cache_next + 1) % CACHE_N;
    cache[i].valid = false;
    if (fs.dev->read(fs.dev, fs.part_lba + lba, 1, cache[i].data) < 0) {
        io_error = true;
        memset(cache[i].data, 0, 512);
        return cache[i].data;
    }
    cache[i].valid = true;
    cache[i].lba = lba;
    return cache[i].data;
}

static void bwrite(uint32_t lba, const void *data) {
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].valid && cache[i].lba == lba) memcpy(cache[i].data, data, 512);
    if (fs.dev->write(fs.dev, fs.part_lba + lba, 1, data) < 0) io_error = true;
}

/* ---- FAT table --------------------------------------------------------- */
static bool is_eoc(uint32_t v) { return v >= (fs.type == 12 ? 0xFF7u : 0xFFF7u); }
static uint32_t eoc(void) { return fs.type == 12 ? 0xFFF : 0xFFFF; }
static uint32_t cluster_lba(uint32_t cl) { return fs.data_lba + (cl - 2) * fs.spc; }

static uint8_t fat_byte(uint32_t off) { return bread(fs.fat_lba + off / 512)[off % 512]; }

static void fat_set_byte(uint32_t off, uint8_t v) {
    uint8_t buf[512];
    for (uint32_t n = 0; n < fs.nfats; n++) {          /* keep all FAT copies in sync */
        uint32_t lba = fs.fat_lba + n * fs.fat_size + off / 512;
        memcpy(buf, bread(lba), 512);
        buf[off % 512] = v;
        bwrite(lba, buf);
    }
}

static uint32_t fat_get(uint32_t cl) {
    if (fs.type == 16) return fat_byte(cl * 2) | (fat_byte(cl * 2 + 1) << 8);
    uint32_t off = cl + cl / 2;                         /* FAT12: 1.5 bytes per entry */
    uint16_t v = fat_byte(off) | (fat_byte(off + 1) << 8);
    return (cl & 1) ? v >> 4 : v & 0xFFF;
}

static void fat_set(uint32_t cl, uint32_t val) {
    if (fs.type == 16) {
        fat_set_byte(cl * 2, val & 0xFF);
        fat_set_byte(cl * 2 + 1, (val >> 8) & 0xFF);
        return;
    }
    uint32_t off = cl + cl / 2;
    uint16_t v = fat_byte(off) | (fat_byte(off + 1) << 8);
    if (cl & 1) v = (v & 0x000F) | (uint16_t)(val << 4);
    else        v = (v & 0xF000) | (uint16_t)(val & 0xFFF);
    fat_set_byte(off, v & 0xFF);
    fat_set_byte(off + 1, v >> 8);
}

static uint32_t alloc_cluster(void) {
    for (uint32_t i = 0; i < fs.nclusters; i++) {
        uint32_t cl = 2 + (fs.alloc_hint + i) % fs.nclusters;
        if (fat_get(cl) == 0) {
            fat_set(cl, eoc());
            fs.alloc_hint = cl - 1;
            return cl;
        }
    }
    return 0;
}

static void free_chain(uint32_t cl) {
    for (uint32_t n = 0; cl >= 2 && !is_eoc(cl) && n < fs.nclusters; n++) {
        uint32_t next = fat_get(cl);
        fat_set(cl, 0);
        cl = next;
    }
}

static void zero_cluster(uint32_t cl) {
    uint8_t z[512];
    memset(z, 0, sizeof(z));
    for (uint32_t s = 0; s < fs.spc; s++) bwrite(cluster_lba(cl) + s, z);
}

/* ---- names --------------------------------------------------------------- */
static bool valid_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'()-@^_`{}~", c);
}

/* "hello.txt" -> "HELLO   TXT" */
static bool to83(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    size_t elen = dot ? strlen(dot + 1) : 0;
    if (blen == 0 || blen > 8 || elen > 3) return false;
    for (size_t i = 0; i < blen; i++) {
        char c = toupper(name[i]);
        if (!valid_char(c)) return false;
        out[i] = c;
    }
    for (size_t i = 0; i < elen; i++) {
        char c = toupper(dot[1 + i]);
        if (!valid_char(c)) return false;
        out[8 + i] = c;
    }
    return true;
}

/* "HELLO   TXT" -> "hello.txt" */
static void from83(const uint8_t in[11], char *out) {
    int n = 0;
    for (int i = 0; i < 8 && in[i] != ' '; i++) out[n++] = tolower(i == 0 && in[0] == 0x05 ? 0xE5 : in[i]);
    if (in[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && in[i] != ' '; i++) out[n++] = tolower(in[i]);
    }
    out[n] = 0;
}

static void stamp(dirent_t *e) {
    rtc_time_t t;
    rtc_read(&t);
    e->wrt_date = (uint16_t)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    e->wrt_time = (uint16_t)((t.hour << 11) | (t.min << 5) | (t.sec / 2));
    e->acc_date = e->wrt_date;
}

/* ---- directories --------------------------------------------------------- */
/* A directory is identified by its first cluster; 0 means the fixed root. */
static int dir_slot(uint32_t dcl, uint32_t idx, uint32_t *lba, uint32_t *off) {
    if (dcl == 0) {
        if (idx >= fs.root_entries) return -1;
        *lba = fs.root_lba + idx / 16;
        *off = (idx % 16) * 32;
        return 0;
    }
    uint32_t per_cl = fs.spc * 16, cl = dcl;
    for (uint32_t n = idx / per_cl; n; n--) {
        cl = fat_get(cl);
        if (cl < 2 || is_eoc(cl)) return -1;
    }
    uint32_t r = idx % per_cl;
    *lba = cluster_lba(cl) + r / 16;
    *off = (r % 16) * 32;
    return 0;
}

static void read_entry(uint32_t lba, uint32_t off, dirent_t *e) { memcpy(e, bread(lba) + off, 32); }

static void write_entry(uint32_t lba, uint32_t off, const dirent_t *e) {
    uint8_t buf[512];
    memcpy(buf, bread(lba), 512);
    memcpy(buf + off, e, 32);
    bwrite(lba, buf);
}

static bool entry_visible(const dirent_t *e) {
    return e->name[0] != 0xE5 && e->attr != ATTR_LFN && !(e->attr & ATTR_VOLUME);
}

static int dir_find(uint32_t dcl, const uint8_t n83[11], dirent_t *out, uint32_t *lba, uint32_t *off) {
    for (uint32_t i = 0;; i++) {
        if (dir_slot(dcl, i, lba, off) < 0) return FS_ENOENT;
        read_entry(*lba, *off, out);
        if (out->name[0] == 0) return FS_ENOENT;          /* end of directory */
        if (entry_visible(out) && !memcmp(out->name, n83, 11)) return FS_OK;
    }
}

/* Store a new entry in the first free slot, growing sub-directories if needed. */
static int dir_add(uint32_t dcl, const dirent_t *e) {
    uint32_t lba, off;
    dirent_t cur;
    for (uint32_t i = 0;; i++) {
        if (dir_slot(dcl, i, &lba, &off) < 0) {
            if (dcl == 0) return FS_ENOSPC;               /* fixed-size root is full */
            uint32_t last = dcl;
            while (!is_eoc(fat_get(last))) last = fat_get(last);
            uint32_t ncl = alloc_cluster();
            if (!ncl) return FS_ENOSPC;
            zero_cluster(ncl);
            fat_set(last, ncl);
            i--;                                          /* retry slot i: it exists now */
            continue;
        }
        read_entry(lba, off, &cur);
        if (cur.name[0] == 0 || cur.name[0] == 0xE5) {
            write_entry(lba, off, e);
            return FS_OK;
        }
    }
}

/* ---- path resolution ----------------------------------------------------- */
typedef struct {
    bool     root;
    dirent_t e;
    uint32_t lba, off;          /* where the entry lives */
} node_t;

static uint32_t node_cluster(const node_t *n) { return n->root ? 0 : n->e.clus_lo; }
static bool node_is_dir(const node_t *n) { return n->root || (n->e.attr & ATTR_DIR); }

static int resolve(const char *path, node_t *n) {
    memset(n, 0, sizeof(*n));
    n->root = true;
    char comp[64];
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int len = 0;
        while (*path && *path != '/') { if (len < 63) comp[len++] = *path; path++; }
        comp[len] = 0;
        if (!node_is_dir(n)) return FS_ENOTDIR;
        uint8_t n83[11];
        if (!to83(comp, n83)) return FS_ENOENT;
        uint32_t dcl = node_cluster(n);
        if (dir_find(dcl, n83, &n->e, &n->lba, &n->off) < 0) return FS_ENOENT;
        n->root = false;
    }
    return FS_OK;
}

/* "/a/b/c.txt" -> parent "/a/b", name "c.txt" */
static void split(const char *path, char *parent, char *name) {
    const char *slash = strrchr(path, '/');
    if (!slash) { strlcpy(parent, "/", 128); strlcpy(name, path, 64); return; }
    size_t plen = slash - path;
    if (plen == 0) strlcpy(parent, "/", 128);
    else { memcpy(parent, path, MIN(plen, 127)); parent[MIN(plen, 127)] = 0; }
    strlcpy(name, slash + 1, 64);
}

/* Read up to len bytes from a cluster chain starting at byte offset. */
static uint32_t chain_read(uint32_t cl, uint32_t size, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (offset >= size) return 0;
    if (len > size - offset) len = size - offset;
    uint32_t cbytes = fs.spc * 512, done = 0;
    for (uint32_t skip = offset / cbytes; skip && cl >= 2 && !is_eoc(cl); skip--) cl = fat_get(cl);
    offset %= cbytes;
    while (done < len && cl >= 2 && !is_eoc(cl)) {
        for (uint32_t s = offset / 512; s < fs.spc && done < len; s++) {
            uint32_t so = offset % 512, n = MIN(512 - so, len - done);
            memcpy(buf + done, bread(cluster_lba(cl) + s) + so, n);
            done += n;
            offset += n;
        }
        offset = 0;
        cl = fat_get(cl);
    }
    return done;
}

/* Allocate a fresh chain and fill it with data. */
static int chain_write(const uint8_t *data, uint32_t len, uint32_t *first) {
    uint32_t prev = 0, done = 0;
    uint8_t buf[512];
    *first = 0;
    while (done < len) {
        uint32_t cl = alloc_cluster();
        if (!cl) { free_chain(*first); *first = 0; return FS_ENOSPC; }
        if (prev) fat_set(prev, cl); else *first = cl;
        prev = cl;
        for (uint32_t s = 0; s < fs.spc; s++) {
            uint32_t n = MIN(512u, len - done);
            memset(buf, 0, 512);
            if (n) memcpy(buf, data + done, n);
            bwrite(cluster_lba(cl) + s, buf);
            done += n;
        }
    }
    return FS_OK;
}

/* ---- public API ---------------------------------------------------------- */
#define LOCKED_BEGIN uint32_t _f = irq_save(); int _r; io_error = false; \
    if (!fs.mounted) { irq_restore(_f); return FS_ENOFS; }
#define LOCKED_END(v) do { _r = (v); if (io_error && _r >= 0) _r = FS_EIO; irq_restore(_f); return _r; } while (0)

int fs_mount(blockdev_t *dev) {
    uint32_t f = irq_save();
    uint8_t sec[512];
    memset(cache, 0, sizeof(cache));
    fs.mounted = false;
    int r = FS_EINVAL;
    if (dev->read(dev, 0, 1, sec) < 0) { r = FS_EIO; goto out; }
    fs.part_lba = 0;
    /* No BPB in sector 0? Maybe it is an MBR with a FAT partition. */
    if (*(uint16_t *)(sec + 11) != 512 && sec[510] == 0x55 && sec[511] == 0xAA) {
        uint8_t *pe = sec + 446;
        for (int i = 0; i < 4; i++, pe += 16)
            if (pe[4] == 0x01 || pe[4] == 0x04 || pe[4] == 0x06 || pe[4] == 0x0E) {
                fs.part_lba = *(uint32_t *)(pe + 8);
                break;
            }
        if (!fs.part_lba || dev->read(dev, fs.part_lba, 1, sec) < 0) goto out;
    }
    uint32_t bps = *(uint16_t *)(sec + 11);
    fs.spc = sec[13];
    uint32_t reserved = *(uint16_t *)(sec + 14);
    fs.nfats = sec[16];
    fs.root_entries = *(uint16_t *)(sec + 17);
    uint32_t total = *(uint16_t *)(sec + 19);
    if (!total) total = *(uint32_t *)(sec + 32);
    fs.fat_size = *(uint16_t *)(sec + 22);
    if (bps != 512 || !fs.spc || (fs.spc & (fs.spc - 1)) || !fs.nfats || fs.nfats > 2 ||
        !fs.fat_size || !fs.root_entries || !reserved)
        goto out;                                  /* not FAT12/16 (FAT32 has root_entries=0) */
    fs.fat_lba = reserved;
    fs.root_lba = reserved + fs.nfats * fs.fat_size;
    fs.root_secs = (fs.root_entries * 32 + 511) / 512;
    fs.data_lba = fs.root_lba + fs.root_secs;
    fs.nclusters = (total - fs.data_lba) / fs.spc;
    fs.type = fs.nclusters < 4085 ? 12 : 16;
    if (fs.nclusters >= 65525) goto out;
    fs.dev = dev;
    fs.alloc_hint = 0;
    fs.mounted = true;
    r = FS_OK;
    kprintf("[fs] mounted %s: FAT%d, %u clusters of %u bytes\n", dev->name, fs.type,
            fs.nclusters, fs.spc * 512);
out:
    irq_restore(f);
    return r;
}

const char *fs_device(void) { return fs.mounted ? fs.dev->name : NULL; }
const char *fs_type(void) { return fs.type == 12 ? "FAT12" : "FAT16"; }

int fs_stat(const char *path, fs_entry_t *out) {
    LOCKED_BEGIN
    node_t n;
    int r = resolve(path, &n);
    if (r == FS_OK) {
        if (n.root) strlcpy(out->name, "/", sizeof(out->name));
        else from83(n.e.name, out->name);
        out->is_dir = node_is_dir(&n);
        out->size = out->is_dir ? 0 : n.e.size;
    }
    LOCKED_END(r);
}

int fs_list(const char *path, fs_entry_t *out, int max) {
    LOCKED_BEGIN
    node_t n;
    int r = resolve(path, &n), count = 0;
    if (r == FS_OK && !node_is_dir(&n)) r = FS_ENOTDIR;
    if (r == FS_OK) {
        uint32_t dcl = node_cluster(&n), lba, off;
        dirent_t e;
        for (uint32_t i = 0; count < max && dir_slot(dcl, i, &lba, &off) == 0; i++) {
            read_entry(lba, off, &e);
            if (e.name[0] == 0) break;
            if (!entry_visible(&e) || e.name[0] == '.') continue;   /* skip . and .. */
            from83(e.name, out[count].name);
            out[count].is_dir = e.attr & ATTR_DIR;
            out[count].size = e.size;
            count++;
        }
        r = count;
    }
    LOCKED_END(r);
}

int fs_read(const char *path, uint32_t offset, void *buf, uint32_t len) {
    LOCKED_BEGIN
    node_t n;
    int r = resolve(path, &n);
    if (r == FS_OK && node_is_dir(&n)) r = FS_EISDIR;
    if (r == FS_OK) r = (int)chain_read(n.e.clus_lo, n.e.size, offset, buf, len);
    LOCKED_END(r);
}

int fs_write(const char *path, const void *buf, uint32_t len, bool append) {
    LOCKED_BEGIN
    char parent[128], name[64];
    uint8_t n83[11];
    node_t p, f;
    uint8_t *joined = NULL;
    const uint8_t *data = buf;
    int r;
    split(path, parent, name);
    if ((r = resolve(parent, &p)) < 0) goto done;
    if (!node_is_dir(&p)) { r = FS_ENOTDIR; goto done; }
    if (!to83(name, n83)) { r = FS_EINVAL; goto done; }
    bool exists = dir_find(node_cluster(&p), n83, &f.e, &f.lba, &f.off) == FS_OK;
    if (exists && (f.e.attr & ATTR_DIR)) { r = FS_EISDIR; goto done; }
    if (exists && append && f.e.size) {               /* old content + new data */
        joined = kmalloc(f.e.size + len + 1);
        if (!joined) { r = FS_ENOSPC; goto done; }
        chain_read(f.e.clus_lo, f.e.size, 0, joined, f.e.size);
        memcpy(joined + f.e.size, buf, len);
        len += f.e.size;
        data = joined;
    }
    if (exists) free_chain(f.e.clus_lo);
    else {
        memset(&f.e, 0, sizeof(f.e));
        memcpy(f.e.name, n83, 11);
        f.e.attr = ATTR_ARCH;
    }
    uint32_t first;
    r = chain_write(data, len, &first);
    f.e.clus_lo = (uint16_t)first;
    f.e.size = r == FS_OK ? len : 0;
    stamp(&f.e);
    if (exists) write_entry(f.lba, f.off, &f.e);
    else {
        int r2 = dir_add(node_cluster(&p), &f.e);
        if (r2 < 0) { free_chain(first); r = r2; }
    }
    if (r == FS_OK) r = (int)len;
done:
    if (joined) kfree(joined);
    LOCKED_END(r);
}

int fs_mkdir(const char *path) {
    LOCKED_BEGIN
    char parent[128], name[64];
    uint8_t n83[11];
    node_t p, f;
    int r;
    split(path, parent, name);
    if ((r = resolve(parent, &p)) < 0) goto done;
    if (!node_is_dir(&p)) { r = FS_ENOTDIR; goto done; }
    if (!to83(name, n83)) { r = FS_EINVAL; goto done; }
    if (dir_find(node_cluster(&p), n83, &f.e, &f.lba, &f.off) == FS_OK) { r = FS_EEXIST; goto done; }
    uint32_t cl = alloc_cluster();
    if (!cl) { r = FS_ENOSPC; goto done; }
    zero_cluster(cl);
    dirent_t e;
    memset(&e, 0, sizeof(e));
    stamp(&e);
    e.attr = ATTR_DIR;
    memset(e.name, ' ', 11); e.name[0] = '.';                     /* "."  -> itself */
    e.clus_lo = (uint16_t)cl;
    write_entry(cluster_lba(cl), 0, &e);
    e.name[1] = '.';                                              /* ".." -> parent */
    e.clus_lo = (uint16_t)node_cluster(&p);
    write_entry(cluster_lba(cl), 32, &e);
    memcpy(e.name, n83, 11);
    e.clus_lo = (uint16_t)cl;
    if ((r = dir_add(node_cluster(&p), &e)) < 0) free_chain(cl);
done:
    LOCKED_END(r);
}

int fs_remove(const char *path) {
    LOCKED_BEGIN
    node_t n;
    int r = resolve(path, &n);
    if (r == FS_OK && n.root) r = FS_EINVAL;
    if (r == FS_OK && (n.e.attr & ATTR_DIR)) {        /* only empty directories */
        uint32_t lba, off;
        dirent_t e;
        for (uint32_t i = 0; dir_slot(n.e.clus_lo, i, &lba, &off) == 0; i++) {
            read_entry(lba, off, &e);
            if (e.name[0] == 0) break;
            if (entry_visible(&e) && e.name[0] != '.') { r = FS_ENOTEMPTY; break; }
        }
    }
    if (r == FS_OK) {
        free_chain(n.e.clus_lo);
        n.e.name[0] = 0xE5;                           /* mark the slot deleted */
        write_entry(n.lba, n.off, &n.e);
    }
    LOCKED_END(r);
}

void fs_usage(uint32_t *total_bytes, uint32_t *free_bytes) {
    *total_bytes = *free_bytes = 0;
    if (!fs.mounted) return;
    uint32_t f = irq_save(), nfree = 0;
    for (uint32_t cl = 2; cl < fs.nclusters + 2; cl++) if (fat_get(cl) == 0) nfree++;
    irq_restore(f);
    *total_bytes = fs.nclusters * fs.spc * 512;
    *free_bytes = nfree * fs.spc * 512;
}

const char *fs_strerror(int err) {
    switch (err) {
    case FS_ENOENT: return "no such file or directory";
    case FS_EEXIST: return "already exists";
    case FS_ENOTDIR: return "not a directory";
    case FS_EISDIR: return "is a directory";
    case FS_ENOSPC: return "no space left on device";
    case FS_ENOTEMPTY: return "directory not empty";
    case FS_EINVAL: return "invalid name (use 8.3 names, e.g. notes.txt)";
    case FS_EIO: return "I/O error";
    case FS_ENOFS: return "no file system mounted";
    default: return "error";
    }
}

void path_normalize(const char *cwd, const char *path, char *out, size_t outsz) {
    char tmp[256];
    if (path[0] == '/') strlcpy(tmp, path, sizeof(tmp));
    else { strlcpy(tmp, cwd, sizeof(tmp)); strlcat(tmp, "/", sizeof(tmp)); strlcat(tmp, path, sizeof(tmp)); }
    /* Walk components, keeping a stack of where each one starts in out. */
    size_t len = 0;
    out[0] = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = p - start;
        if (clen == 1 && start[0] == '.') continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;   /* drop last component */
            if (len > 0) len--;
            out[len] = 0;
            continue;
        }
        if (len + clen + 2 >= outsz) break;
        out[len++] = '/';
        memcpy(out + len, start, clen);
        len += clen;
        out[len] = 0;
    }
    if (len == 0) { out[0] = '/'; out[1] = 0; }
}
