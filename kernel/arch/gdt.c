/* Global Descriptor Table: flat kernel/user segments plus one TSS.
 * The TSS only exists so the CPU knows which kernel stack to use when an
 * interrupt arrives while a user program (ring 3) is running. */
#include "../include/arch.h"
#include "../include/string.h"

typedef struct __attribute__((packed)) {
    uint16_t limit_lo, base_lo;
    uint8_t  base_mid, access, gran, base_hi;
} gdt_entry_t;

typedef struct __attribute__((packed)) { uint16_t limit; uint32_t base; } gdt_ptr_t;

typedef struct __attribute__((packed)) {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} tss_t;

static gdt_entry_t gdt[6];
static gdt_ptr_t   gdt_ptr;
static tss_t       tss;

extern void gdt_flush(gdt_ptr_t *p);

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_lo  = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_hi  = (base >> 24) & 0xFF;
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].gran     = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access   = access;
}

void gdt_init(void) {
    set_entry(0, 0, 0, 0, 0);                    /* null */
    set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);     /* kernel code, ring 0 */
    set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);     /* kernel data */
    set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);     /* user code, ring 3 */
    set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);     /* user data */
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = KERNEL_DS;
    tss.iomap = sizeof(tss);                     /* no I/O permission bitmap */
    set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);  /* 32-bit TSS */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint32_t)&gdt;
    gdt_flush(&gdt_ptr);
}

void tss_set_kernel_stack(uint32_t esp0) { tss.esp0 = esp0; }
