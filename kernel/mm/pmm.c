/* Physical memory manager: one bit per 4 KiB frame (1 = used).
 * Only the first 1 GiB of RAM is managed because that is what the kernel
 * identity-maps; this is plenty for the VMs this OS targets. */
#include "../include/mm.h"
#include "../include/multiboot.h"
#include "../include/string.h"
#include "../include/kprintf.h"

#define MAX_FRAMES (IDENTITY_LIMIT / PAGE_SIZE)

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total_frames, free_frames, search_hint;

extern char kernel_start[], kernel_end[];

static inline void set_used(uint32_t f) { bitmap[f / 32] |= 1u << (f % 32); }
static inline void set_free(uint32_t f) { bitmap[f / 32] &= ~(1u << (f % 32)); }
static inline bool is_used(uint32_t f) { return bitmap[f / 32] & (1u << (f % 32)); }

static void reserve_range(uint32_t start, uint32_t end) {
    for (uint32_t f = start / PAGE_SIZE; f < ALIGN_UP(end, PAGE_SIZE) / PAGE_SIZE && f < MAX_FRAMES; f++)
        if (!is_used(f)) { set_used(f); free_frames--; }
}

void pmm_init(void *mbi_ptr) {
    multiboot_info_t *mbi = mbi_ptr;
    memset(bitmap, 0xFF, sizeof(bitmap));        /* everything used by default */

    if (mbi->flags & MB_FLAG_MMAP) {
        uint32_t p = mbi->mmap_addr;
        while (p < mbi->mmap_addr + mbi->mmap_length) {
            mb_mmap_t *e = (mb_mmap_t *)p;
            if (e->type == 1 && e->addr < IDENTITY_LIMIT) {
                uint64_t end = e->addr + e->len;
                if (end > IDENTITY_LIMIT) end = IDENTITY_LIMIT;
                for (uint32_t f = ALIGN_UP((uint32_t)e->addr, PAGE_SIZE) / PAGE_SIZE;
                     f < (uint32_t)end / PAGE_SIZE; f++)
                    if (is_used(f)) { set_free(f); free_frames++; total_frames++; }
            }
            p += e->size + 4;
        }
    } else {                                     /* fallback: mem_upper (KB above 1 MiB) */
        uint32_t end = 0x100000 + mbi->mem_upper * 1024;
        if (end > IDENTITY_LIMIT) end = IDENTITY_LIMIT;
        for (uint32_t f = 0x100000 / PAGE_SIZE; f < end / PAGE_SIZE; f++) {
            set_free(f); free_frames++; total_frames++;
        }
    }
    boot_info.mem_kb = total_frames * 4;

    reserve_range(0, 0x100000);                                  /* BIOS, VGA, ... */
    reserve_range((uint32_t)kernel_start, (uint32_t)kernel_end); /* kernel image */
    if (boot_info.initrd_start)
        reserve_range(boot_info.initrd_start, boot_info.initrd_end);
    search_hint = 0;
}

uint32_t pmm_alloc(void) {
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        uint32_t f = (search_hint + i) % MAX_FRAMES;
        if (bitmap[f / 32] == 0xFFFFFFFF) { i += 31 - (f % 32); continue; }
        if (!is_used(f)) {
            set_used(f); free_frames--; search_hint = f + 1;
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

uint32_t pmm_alloc_contiguous(uint32_t n) {
    uint32_t run = 0;
    for (uint32_t f = 0x100000 / PAGE_SIZE; f < MAX_FRAMES; f++) {
        run = is_used(f) ? 0 : run + 1;
        if (run == n) {
            uint32_t first = f + 1 - n;
            for (uint32_t k = first; k <= f; k++) set_used(k);
            free_frames -= n;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint32_t addr) {
    uint32_t f = addr / PAGE_SIZE;
    if (f < MAX_FRAMES && is_used(f)) { set_free(f); free_frames++; }
}

uint32_t pmm_free_frames(void) { return free_frames; }
uint32_t pmm_total_frames(void) { return total_frames; }
