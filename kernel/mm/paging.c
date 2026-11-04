/* Paging. The kernel maps the first 1 GiB 1:1 with 4 MiB pages (PSE) and
 * without the USER bit, so ring-3 code cannot touch kernel memory. Each user
 * process gets its own page directory: a copy of the kernel entries plus
 * 4 KiB user pages in the range USER_BASE..USER_LIMIT (1..2 GiB). */
#include "../include/mm.h"
#include "../include/string.h"
#include "../include/io.h"

static uint32_t kernel_pd[1024] __attribute__((aligned(4096)));

void paging_map_identity_range(uint32_t phys, uint32_t size) {
    uint32_t a = ALIGN_DOWN(phys, 0x400000);
    uint32_t end = phys + size;
    for (;;) {
        kernel_pd[a >> 22] = a | PTE_P | PTE_W | PTE_PS;
        a += 0x400000;
        if (a == 0 || a >= end) break;           /* done (or wrapped at 4 GiB) */
    }
}

void paging_init(void) {
    memset(kernel_pd, 0, sizeof(kernel_pd));
    for (uint32_t i = 0; i < IDENTITY_LIMIT / 0x400000; i++)
        kernel_pd[i] = (i * 0x400000) | PTE_P | PTE_W | PTE_PS;
    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1 << 4;                                /* PSE: enable 4 MiB pages */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pd));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000;                            /* PG + WP (honour read-only in ring 0) */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}

uint32_t *paging_kernel_dir(void) { return kernel_pd; }

void paging_switch(uint32_t *pd) {
    if (!pd) pd = kernel_pd;
    uint32_t cur;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
    if (cur != (uint32_t)pd) __asm__ volatile("mov %0, %%cr3" :: "r"(pd) : "memory");
}

uint32_t *paging_new_dir(void) {
    uint32_t *pd = (uint32_t *)pmm_alloc();
    if (!pd) return NULL;
    memcpy(pd, kernel_pd, PAGE_SIZE);   /* kernel part is shared, user part empty */
    return pd;
}

int paging_map_user(uint32_t *pd, uint32_t va) {
    uint32_t pdi = va >> 22, pti = (va >> 12) & 0x3FF;
    if (va < USER_BASE || va >= USER_LIMIT) return -1;
    if (!(pd[pdi] & PTE_P)) {
        uint32_t pt = pmm_alloc();
        if (!pt) return -1;
        memset((void *)pt, 0, PAGE_SIZE);
        pd[pdi] = pt | PTE_P | PTE_W | PTE_U;
    }
    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFF);
    if (pt[pti] & PTE_P) return 0;               /* already mapped */
    uint32_t frame = pmm_alloc();
    if (!frame) return -1;
    memset((void *)frame, 0, PAGE_SIZE);         /* frames are identity-mapped */
    pt[pti] = frame | PTE_P | PTE_W | PTE_U;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    return 0;
}

uint32_t paging_virt_to_phys(uint32_t *pd, uint32_t va) {
    uint32_t pde = pd[va >> 22];
    if (!(pde & PTE_P)) return 0;
    if (pde & PTE_PS) return (pde & 0xFFC00000) | (va & 0x3FFFFF);
    uint32_t pte = ((uint32_t *)(pde & ~0xFFF))[(va >> 12) & 0x3FF];
    if (!(pte & PTE_P)) return 0;
    return (pte & ~0xFFF) | (va & 0xFFF);
}

uint32_t paging_count_user_pages(uint32_t *pd) {
    uint32_t n = 0;
    if (!pd) return 0;
    for (uint32_t i = USER_BASE >> 22; i < USER_LIMIT >> 22; i++) {
        if (!(pd[i] & PTE_P)) continue;
        uint32_t *pt = (uint32_t *)(pd[i] & ~0xFFF);
        for (int j = 0; j < 1024; j++) if (pt[j] & PTE_P) n++;
    }
    return n;
}

void paging_free_dir(uint32_t *pd) {
    if (!pd || pd == kernel_pd) return;
    for (uint32_t i = USER_BASE >> 22; i < USER_LIMIT >> 22; i++) {
        if (!(pd[i] & PTE_P)) continue;
        uint32_t *pt = (uint32_t *)(pd[i] & ~0xFFF);
        for (int j = 0; j < 1024; j++) if (pt[j] & PTE_P) pmm_free(pt[j] & ~0xFFF);
        pmm_free((uint32_t)pt);
    }
    pmm_free((uint32_t)pd);
}
