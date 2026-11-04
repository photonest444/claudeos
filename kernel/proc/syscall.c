/* System calls: user programs execute `int 0x80` with
 *   EAX = number, EBX/ECX/EDX = arguments; the result is returned in EAX.
 * The numbers must match user/lib/syscall.h. */
#include "../include/task.h"
#include "../include/fs.h"
#include "../include/mm.h"
#include "../include/drivers.h"
#include "../include/string.h"
#include "../include/kprintf.h"

enum {
    SYS_EXIT = 0, SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_GETPID,
    SYS_SLEEP, SYS_TICKS, SYS_SBRK, SYS_YIELD, SYS_GETKEY, SYS_STAT,
};

/* Is [p, p+len) mapped user memory of the current process? */
static bool user_ok(uint32_t p, uint32_t len) {
    if (p < USER_BASE || p + len < p || p + len > USER_LIMIT) return false;
    for (uint32_t va = ALIGN_DOWN(p, PAGE_SIZE); va < p + len; va += PAGE_SIZE)
        if (!paging_virt_to_phys(current->pd, va)) return false;
    return true;
}

static bool user_str_ok(uint32_t p) {
    for (uint32_t i = 0; i < 256; i++) {
        if (!user_ok(p + i, 1)) return false;
        if (!*(char *)(p + i)) return true;
    }
    return false;
}

static int sys_open(const char *upath) {
    char path[128];
    path_normalize(current->cwd, upath, path, sizeof(path));
    fs_entry_t st;
    int r = fs_stat(path, &st);
    if (r < 0) return r;
    if (st.is_dir) return FS_EISDIR;
    for (int fd = 3; fd < MAX_FDS; fd++) {
        ufile_t *f = &current->files[fd];
        if (f->used) continue;
        f->used = true;
        strlcpy(f->path, path, sizeof(f->path));
        f->pos = 0;
        f->size = st.size;
        return fd;
    }
    return -1;
}

static int sys_read(int fd, char *buf, uint32_t len) {
    if (fd == 0) return current->tty ? tty_read_line(current->tty, buf, len) : 0;
    if (fd < 3 || fd >= MAX_FDS || !current->files[fd].used) return -1;
    ufile_t *f = &current->files[fd];
    int n = fs_read(f->path, f->pos, buf, len);
    if (n > 0) f->pos += n;
    return n;
}

static uint32_t sys_sbrk(int32_t inc) {
    uint32_t old = current->brk, new_brk = old + inc;
    if (new_brk < USER_BASE || new_brk > USER_STACK_TOP - (USER_STACK_PAGES + 16) * PAGE_SIZE)
        return (uint32_t)-1;
    for (uint32_t va = ALIGN_UP(old, PAGE_SIZE); va < new_brk; va += PAGE_SIZE)
        if (paging_map_user(current->pd, va) < 0) return (uint32_t)-1;
    current->brk = new_brk;
    return old;
}

void syscall_handler(regs_t *r) {
    uint32_t a = r->ebx, b = r->ecx, c = r->edx;
    int32_t ret = -1;
    if (r->eax != SYS_EXIT) __asm__ volatile("sti");   /* syscalls are preemptible */
    switch (r->eax) {
    case SYS_EXIT:
        task_exit((int)a);
        break;
    case SYS_WRITE:
        if ((a == 1 || a == 2) && user_ok(b, c) && current->tty) {
            tty_write(current->tty, (const char *)b, c);
            ret = c;
        }
        break;
    case SYS_READ:
        if (user_ok(b, c)) ret = sys_read((int)a, (char *)b, c);
        break;
    case SYS_OPEN:
        if (user_str_ok(a)) ret = sys_open((const char *)a);
        break;
    case SYS_CLOSE:
        if (a >= 3 && a < MAX_FDS && current->files[a].used) { current->files[a].used = false; ret = 0; }
        break;
    case SYS_GETPID: ret = current->pid; break;
    case SYS_SLEEP: task_sleep(a); ret = 0; break;
    case SYS_TICKS: ret = (int32_t)timer_ticks(); break;
    case SYS_SBRK: ret = (int32_t)sys_sbrk((int32_t)a); break;
    case SYS_YIELD: task_yield(); ret = 0; break;
    case SYS_GETKEY: ret = current->tty ? tty_trygetkey(current->tty) : -1; break;
    case SYS_STAT:          /* stat(path) -> size, or negative error / -4 for dirs */
        if (user_str_ok(a)) {
            char path[128];
            fs_entry_t st;
            path_normalize(current->cwd, (const char *)a, path, sizeof(path));
            ret = fs_stat(path, &st);
            if (ret == 0) ret = st.is_dir ? FS_EISDIR : (int32_t)st.size;
        }
        break;
    }
    r->eax = (uint32_t)ret;
}
