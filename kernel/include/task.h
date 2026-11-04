/* Tasks (kernel threads and user processes) and the preemptive scheduler. */
#ifndef TASK_H
#define TASK_H
#include "types.h"
#include "arch.h"
#include "tty.h"

#define MAX_TASKS     32
#define MAX_FDS       8
#define KSTACK_SIZE   16384

enum { T_UNUSED = 0, T_READY, T_RUNNING, T_SLEEPING, T_ZOMBIE };

typedef struct {
    bool     used;
    char     path[128];
    uint32_t pos, size;
} ufile_t;

typedef struct task {
    int       pid;
    char      name[32];
    int       state;
    uint32_t  esp;              /* saved kernel stack pointer (points to regs_t) */
    uint8_t  *kstack;
    uint32_t *pd;               /* page directory (NULL = kernel's) */
    bool      is_user;
    bool      detached;         /* nobody waits: reaped automatically */
    bool      system;           /* essential kernel task, cannot be killed */
    char      cwd[128];         /* working directory (user programs) */
    uint32_t  wake_at;
    int       exit_code;
    uint32_t  cpu_ticks;
    uint32_t  start_tick;
    tty_t    *tty;
    uint32_t  brk;              /* end of user heap */
    ufile_t   files[MAX_FDS];
} task_t;

extern task_t *current;

void    tasking_init(void);
int     task_create_kernel(const char *name, void (*fn)(void *), void *arg, tty_t *tty);
int     task_create_user(const char *name, uint32_t *pd, uint32_t entry,
                         uint32_t user_esp, uint32_t brk, tty_t *tty, bool detached);
void    task_exit(int code) __attribute__((noreturn));
int     task_kill(int pid);
void    task_kill_tty(tty_t *tty);  /* kill every task attached to a tty */
int     task_wait(int pid);         /* blocks until pid exits, returns exit code */
void    task_sleep(uint32_t ms);
void    task_yield(void);
task_t *task_get(int pid);
task_t *task_by_index(int i);
const char *task_state_name(int state);
uint32_t schedule(uint32_t esp);   /* called from the interrupt dispatcher */
void    scheduler_tick(void);

/* ELF loader: creates a user process, returns pid or negative error. */
int  elf_exec(const char *path, int argc, char **argv, tty_t *tty, bool detached, const char *cwd);

/* System calls (int 0x80). */
void syscall_handler(regs_t *r);
#endif
