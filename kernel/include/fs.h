/* File system API (FAT12/FAT16 on a block device). Paths are absolute. */
#ifndef FS_H
#define FS_H
#include "types.h"
#include "drivers.h"

enum {
    FS_OK = 0, FS_ENOENT = -1, FS_EEXIST = -2, FS_ENOTDIR = -3, FS_EISDIR = -4,
    FS_ENOSPC = -5, FS_ENOTEMPTY = -6, FS_EINVAL = -7, FS_EIO = -8, FS_ENOFS = -9,
};

typedef struct {
    char     name[13];          /* 8.3 name, lower-case */
    uint32_t size;
    bool     is_dir;
} fs_entry_t;

int         fs_mount(blockdev_t *dev);
const char *fs_device(void);            /* name of mounted device or NULL */
const char *fs_type(void);              /* "FAT12" / "FAT16" */
int         fs_stat(const char *path, fs_entry_t *out);
int         fs_list(const char *path, fs_entry_t *out, int max);
int         fs_read(const char *path, uint32_t offset, void *buf, uint32_t len);
int         fs_write(const char *path, const void *buf, uint32_t len, bool append);
int         fs_mkdir(const char *path);
int         fs_remove(const char *path);
void        fs_usage(uint32_t *total_bytes, uint32_t *free_bytes);
const char *fs_strerror(int err);
/* Turn "rel/../path" relative to cwd into a canonical absolute path. */
void        path_normalize(const char *cwd, const char *path, char *out, size_t outsz);
#endif
