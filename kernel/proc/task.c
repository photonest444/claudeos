/* Tasks and the round-robin preemptive scheduler.
 *
 * Every task has its own kernel stack. A task that is not running is
 * represented by the stack pointer saved in task->esp, which points at a
 * complete regs_t interrupt frame. Switching tasks = returning a different
 * ESP from interrupt_dispatch(); the iret in isr.asm then "resumes" it.
 * New tasks get a hand-made frame so their first switch starts them. */
#include "../include/task.h"
#include "../include/mm.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/drivers.h"
#include "../include/kprintf.h"

#define QUANTUM 10                 /* ms per time slice */

static task_t tasks[MAX_TASKS];
task_t *current;
static int next_pid = 1;
static int quantum = QUANTUM;

void tasking_init(void) {
    memset(tasks, 0, sizeof(tasks));
    /* The boot code becomes task 0, the idle task. */
    tasks[0].pid = 0;
    strlcpy(tasks[0].name, "idle", sizeof(tasks[0].name));
    tasks[0].state = T_RUNNING;
    tasks[0].system = true;
    strlcpy(tasks[0].cwd, "/", sizeof(tasks[0].cwd));
    current = &tasks[0];
}

static task_t *alloc_task(const char *name, tty_t *tty) {
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != T_UNUSED) continue;
        task_t *t = &tasks[i];
        memset(t, 0, sizeof(*t));
        t->kstack = kmalloc(KSTACK_SIZE);
        if (!t->kstack) return NULL;
        t->pid = next_pid++;
        strlcpy(t->name, name, sizeof(t->name));
        strlcpy(t->cwd, "/", sizeof(t->cwd));
        t->tty = tty;
        t->start_tick = timer_ticks();
        return t;
    }
    return NULL;
}

static void kthread_return(void) { task_exit(0); }

int task_create_kernel(const char *name, void (*fn)(void *), void *arg, tty_t *tty) {
    uint32_t f = irq_save();
    task_t *t = alloc_task(name, tty);
    if (!t) { irq_restore(f); return -1; }
    uint32_t *sp = (uint32_t *)(t->kstack + KSTACK_SIZE);
    *--sp = (uint32_t)arg;                     /* fn(arg) ...                */
    *--sp = (uint32_t)kthread_return;          /* ... returns into task_exit */
    /* Ring 0 -> ring 0 iret does not pop useresp/ss, so the frame is 8 bytes shorter. */
    regs_t *r = (regs_t *)((uint8_t *)sp - (sizeof(regs_t) - 8));
    memset(r, 0, sizeof(regs_t) - 8);
    r->ds = r->es = r->fs = r->gs = KERNEL_DS;
    r->cs = KERNEL_CS;
    r->eflags = 0x202;                         /* IF=1 */
    r->eip = (uint32_t)fn;
    t->esp = (uint32_t)r;
    t->detached = true;
    t->state = T_READY;
    int pid = t->pid;
    irq_restore(f);
    return pid;
}

int task_create_user(const char *name, uint32_t *pd, uint32_t entry, uint32_t user_esp,
                     uint32_t brk, tty_t *tty, bool detached) {
    uint32_t f = irq_save();
    task_t *t = alloc_task(name, tty);
    if (!t) { irq_restore(f); return -1; }
    regs_t *r = (regs_t *)(t->kstack + KSTACK_SIZE - sizeof(regs_t));
    memset(r, 0, sizeof(regs_t));
    r->ds = r->es = r->fs = r->gs = USER_DS;
    r->cs = USER_CS;
    r->ss = USER_DS;
    r->useresp = user_esp;
    r->eflags = 0x202;
    r->eip = entry;
    t->esp = (uint32_t)r;
    t->pd = pd;
    t->is_user = true;
    t->brk = brk;
    t->detached = detached;
    t->state = T_READY;
    int pid = t->pid;
    irq_restore(f);
    return pid;
}

static void free_task(task_t *t) {
    kfree(t->kstack);
    paging_free_dir(t->pd);
    t->kstack = NULL;
    t->pd = NULL;
    t->state = T_UNUSED;
}

/* Called on every timer tick (1 ms) with interrupts off. */
void scheduler_tick(void) {
    uint32_t now = timer_ticks();
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == T_SLEEPING && (int32_t)(now - tasks[i].wake_at) >= 0) {
            tasks[i].state = T_READY;
            if (current == &tasks[0]) need_resched = 1;   /* leave idle quickly */
        }
    if (current) {
        current->cpu_ticks++;
        if (--quantum <= 0) need_resched = 1;
    }
}

uint32_t schedule(uint32_t esp) {
    current->esp = esp;
    if (current->state == T_RUNNING) current->state = T_READY;

    /* Free detached zombies. Never the current one: we are on its stack. */
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == T_ZOMBIE && tasks[i].detached && &tasks[i] != current)
            free_task(&tasks[i]);

    int ci = current - tasks;
    task_t *next = NULL;
    for (int i = 1; i <= MAX_TASKS; i++) {
        task_t *t = &tasks[(ci + i) % MAX_TASKS];
        if (t != &tasks[0] && t->state == T_READY) { next = t; break; }
    }
    if (!next) next = &tasks[0];               /* nothing to do: idle */

    next->state = T_RUNNING;
    current = next;
    tss_set_kernel_stack(next->kstack ? (uint32_t)next->kstack + KSTACK_SIZE : 0);
    paging_switch(next->pd);
    need_resched = 0;
    quantum = QUANTUM;
    return next->esp;
}

void task_yield(void) { __asm__ volatile("int $0x81"); }

void task_sleep(uint32_t ms) {
    uint32_t f = irq_save();
    current->wake_at = timer_ticks() + (ms ? ms : 1);
    current->state = T_SLEEPING;
    irq_restore(f);
    task_yield();
}

void task_exit(int code) {
    __asm__ volatile("cli");
    current->exit_code = code;
    current->state = T_ZOMBIE;
    task_yield();
    for (;;) cpu_halt();                       /* never reached */
}

task_t *task_get(int pid) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != T_UNUSED && tasks[i].pid == pid) return &tasks[i];
    return NULL;
}

task_t *task_by_index(int i) { return (i >= 0 && i < MAX_TASKS) ? &tasks[i] : NULL; }

int task_kill(int pid) {
    uint32_t f = irq_save();
    task_t *t = task_get(pid);
    if (!t || t->system || t->state == T_ZOMBIE) { irq_restore(f); return -1; }
    t->state = T_ZOMBIE;
    t->exit_code = -1;
    bool self = t == current;
    irq_restore(f);
    if (self) {
        need_resched = 1;
        if (f & 0x200) task_yield();           /* task context: switch away now */
    }
    return 0;
}

void task_kill_tty(tty_t *tty) {
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state != T_UNUSED && tasks[i].tty == tty && &tasks[i] != current)
            task_kill(tasks[i].pid);
}

int task_wait(int pid) {
    for (;;) {
        uint32_t f = irq_save();
        task_t *t = task_get(pid);
        if (!t) { irq_restore(f); return -1; }
        if (t->state == T_ZOMBIE) {
            int code = t->exit_code;
            free_task(t);
            irq_restore(f);
            return code;
        }
        irq_restore(f);
        task_sleep(10);
    }
}

const char *task_state_name(int s) {
    static const char *names[] = { "unused", "ready", "running", "sleeping", "zombie" };
    return s >= 0 && s <= T_ZOMBIE ? names[s] : "?";
}
