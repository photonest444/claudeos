/* CPU-level structures: GDT/TSS, IDT, interrupt frame, PIC. */
#ifndef ARCH_H
#define ARCH_H
#include "types.h"

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B   /* GDT entry 3, RPL 3 */
#define USER_DS   0x23   /* GDT entry 4, RPL 3 */
#define TSS_SEL   0x28

/* Register frame built by the assembly stubs in isr.asm (lowest address first). */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;   /* pusha */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;                                /* pushed by CPU */
    uint32_t useresp, ss;                                    /* only on ring change */
} regs_t;

typedef void (*irq_handler_t)(regs_t *r);

void gdt_init(void);
void tss_set_kernel_stack(uint32_t esp0);
void idt_init(void);
void irq_install(int irq, irq_handler_t h);
void irq_unmask(int irq);

/* Set by drivers/scheduler to request a task switch at the end of the interrupt. */
extern volatile int need_resched;
#endif
