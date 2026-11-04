/* ELF32 loader: maps PT_LOAD segments into a fresh address space, builds the
 * user stack with argc/argv and starts the program in ring 3. */
#include "../include/task.h"
#include "../include/fs.h"
#include "../include/mm.h"
#include "../include/string.h"
#include "../include/kprintf.h"

typedef struct __attribute__((packed)) {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} elf_phdr_t;

#define PT_LOAD 1
#define MAX_ARGS 16

/* Copy into another address space (frames are identity-mapped in the kernel). */
static int copy_to_pd(uint32_t *pd, uint32_t va, const void *src, uint32_t len) {
    const uint8_t *s = src;
    while (len) {
        uint32_t phys = paging_virt_to_phys(pd, va);
        if (!phys) return -1;
        uint32_t n = MIN(len, PAGE_SIZE - (va & 0xFFF));
        memcpy((void *)phys, s, n);
        s += n; va += n; len -= n;
    }
    return 0;
}

int elf_exec(const char *path, int argc, char **argv, tty_t *tty, bool detached, const char *cwd) {
    fs_entry_t st;
    int r = fs_stat(path, &st);
    if (r < 0) return r;
    if (st.is_dir) return FS_EISDIR;
    if (st.size < sizeof(elf_hdr_t)) return FS_EINVAL;
    uint8_t *img = kmalloc(st.size);
    if (!img) return FS_ENOSPC;
    uint32_t *pd = NULL;
    r = FS_EINVAL;
    if (fs_read(path, 0, img, st.size) != (int)st.size) goto fail;

    elf_hdr_t *eh = (elf_hdr_t *)img;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 1 || eh->type != 2 || eh->machine != 3)
        goto fail;                                  /* not a 32-bit x86 executable */
    if (eh->phoff + (uint32_t)eh->phnum * sizeof(elf_phdr_t) > st.size) goto fail;

    pd = paging_new_dir();
    if (!pd) goto fail;
    uint32_t top = USER_BASE;
    elf_phdr_t *ph = (elf_phdr_t *)(img + eh->phoff);
    for (int i = 0; i < eh->phnum; i++, ph++) {
        if (ph->type != PT_LOAD) continue;
        uint32_t end = ph->vaddr + ph->memsz;
        if (ph->vaddr < USER_BASE || end > USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE ||
            end < ph->vaddr || ph->offset + ph->filesz > st.size || ph->filesz > ph->memsz)
            goto fail;
        for (uint32_t va = ALIGN_DOWN(ph->vaddr, PAGE_SIZE); va < end; va += PAGE_SIZE)
            if (paging_map_user(pd, va) < 0) goto fail;
        if (copy_to_pd(pd, ph->vaddr, img + ph->offset, ph->filesz) < 0) goto fail;
        if (end > top) top = end;                    /* .bss is already zeroed */
    }

    /* Stack: argument strings, then argv[], then argv and argc for crt0. */
    for (uint32_t i = 1; i <= USER_STACK_PAGES; i++)
        if (paging_map_user(pd, USER_STACK_TOP - i * PAGE_SIZE) < 0) goto fail;
    uint32_t sp = USER_STACK_TOP, uargv[MAX_ARGS + 1];
    if (argc > MAX_ARGS) argc = MAX_ARGS;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = strlen(argv[i]) + 1;
        sp -= len;
        copy_to_pd(pd, sp, argv[i], len);
        uargv[i] = sp;
    }
    uargv[argc] = 0;
    sp &= ~3u;
    sp -= (argc + 1) * 4;
    copy_to_pd(pd, sp, uargv, (argc + 1) * 4);
    uint32_t argv_ptr = sp, argc_val = argc;
    sp -= 4; copy_to_pd(pd, sp, &argv_ptr, 4);
    sp -= 4; copy_to_pd(pd, sp, &argc_val, 4);

    const char *base = strrchr(path, '/');
    int pid = task_create_user(base ? base + 1 : path, pd, eh->entry, sp,
                               ALIGN_UP(top, PAGE_SIZE), tty, detached);
    if (pid < 0) { r = FS_ENOSPC; goto fail; }
    task_t *t = task_get(pid);
    if (t && cwd) strlcpy(t->cwd, cwd, sizeof(t->cwd));
    kfree(img);
    return pid;
fail:
    if (pd) paging_free_dir(pd);
    kfree(img);
    return r;
}
