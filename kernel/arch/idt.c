/* Interrupt Descriptor Table, 8259 PIC setup, exception handling and the
 * central interrupt dispatcher (which is also where task switches happen). */
#include "../include/arch.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/kprintf.h"
#include "../include/task.h"
#include "../include/mm.h"

typedef struct __attribute__((packed)) {
    uint16_t off_lo, sel;
    uint8_t  zero, flags;
    uint16_t off_hi;
} idt_entry_t;

typedef struct __attribute__((packed)) { uint16_t limit; uint32_t base; } idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t idt_ptr;
static irq_handler_t irq_handlers[16];
volatile int need_resched;

extern void idt_flush(idt_ptr_t *p);
#define X(n) extern void isr##n(void);
X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29)
X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43)
X(44) X(45) X(46) X(47) X(128) X(129)
#undef X

static void (*const stubs[48])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9, isr10, isr11, isr12,
    isr13, isr14, isr15, isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24,
    isr25, isr26, isr27, isr28, isr29, isr30, isr31, isr32, isr33, isr34, isr35, isr36,
    isr37, isr38, isr39, isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

static void set_gate(int n, void (*fn)(void), uint8_t flags) {
    uint32_t a = (uint32_t)fn;
    idt[n].off_lo = a & 0xFFFF;
    idt[n].off_hi = a >> 16;
    idt[n].sel = KERNEL_CS;
    idt[n].zero = 0;
    idt[n].flags = flags;
}

/* Remap the PICs so IRQ0-15 become vectors 32-47 (0-31 are CPU exceptions). */
static void pic_remap(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();   /* init */
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();   /* offsets */
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();   /* cascade */
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();   /* 8086 mode */
    outb(0x21, 0xFB); outb(0xA1, 0xFF);   /* mask all except cascade (IRQ2) */
}

void irq_unmask(int irq) {
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void irq_install(int irq, irq_handler_t h) {
    irq_handlers[irq] = h;
    irq_unmask(irq);
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    for (int i = 0; i < 48; i++) set_gate(i, stubs[i], 0x8E);   /* ring-0 interrupt gate */
    set_gate(0x80, isr128, 0xEE);   /* DPL 3: user programs may call int 0x80 */
    set_gate(0x81, isr129, 0x8E);
    pic_remap();
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)idt;
    idt_flush(&idt_ptr);
}

static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check", "Machine check",
    "SIMD FP exception", "Virtualization", "Control protection",
};

static void handle_exception(regs_t *r) {
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    const char *name = r->int_no < 32 && exc_names[r->int_no] ? exc_names[r->int_no] : "Exception";
    bool from_user = (r->cs & 3) == 3;
    /* Kernel touching a bad user pointer during a syscall also counts as the
     * program's fault. */
    bool user_ptr = !from_user && r->int_no == 14 && current && current->is_user &&
                    cr2 >= USER_BASE && cr2 < USER_LIMIT;
    if (from_user || user_ptr) {
        if (current->tty)
            tty_printf(current->tty, "\n\x1b[91m%s: %s at eip=%p addr=%p - process killed\x1b[0m\n",
                       current->name, name, r->eip, cr2);
        kprintf("[kernel] pid %d (%s): %s eip=%p cr2=%p err=%x\n",
                current->pid, current->name, name, r->eip, cr2, r->err_code);
        task_kill(current->pid);
        return;
    }
    panic("%s (vector %d, err=%x)\n eip=%p cs=%x eflags=%x cr2=%p\n"
          " eax=%p ebx=%p ecx=%p edx=%p\n esi=%p edi=%p ebp=%p task=%s",
          name, r->int_no, r->err_code, r->eip, r->cs, r->eflags, cr2,
          r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp,
          current ? current->name : "?");
}

/* Called from isr_common with interrupts disabled. Returns the ESP to resume. */
uint32_t interrupt_dispatch(regs_t *r) {
    uint32_t esp = (uint32_t)r;
    uint32_t n = r->int_no;

    if (n < 32) {
        handle_exception(r);
    } else if (n < 48) {
        int irq = n - 32;
        /* Spurious IRQ 7/15: the PIC's in-service bit is not set. */
        if (irq == 7 || irq == 15) {
            uint16_t cmd = irq == 7 ? 0x20 : 0xA0;
            outb(cmd, 0x0B);
            if (!(inb(cmd) & 0x80)) {
                if (irq == 15) outb(0x20, 0x20);
                return esp;
            }
        }
        if (irq_handlers[irq]) irq_handlers[irq](r);
        if (irq >= 8) outb(0xA0, 0x20);   /* end of interrupt */
        outb(0x20, 0x20);
    } else if (n == 0x80) {
        syscall_handler(r);
        __asm__ volatile("cli");          /* syscalls may have re-enabled IRQs */
    } else if (n == 0x81) {
        need_resched = 1;
    }

    if (current && (need_resched || current->state != T_RUNNING))
        esp = schedule(esp);
    return esp;
}
