/* Memory management: physical frames, paging, kernel heap. */
#ifndef MM_H
#define MM_H
#include "types.h"

#define PAGE_SIZE        4096
#define IDENTITY_LIMIT   0x40000000u   /* kernel identity-maps the first 1 GiB */
#define USER_BASE        0x40000000u   /* user programs live in 1..2 GiB */
#define USER_LIMIT       0x80000000u
#define USER_STACK_TOP   0x7FFF0000u
#define USER_STACK_PAGES 16

/* Physical memory manager (bitmap of 4 KiB frames). */
void     pmm_init(void *mb_info);
uint32_t pmm_alloc(void);                   /* returns physical address or 0 */
uint32_t pmm_alloc_contiguous(uint32_t n);  /* n frames, physically contiguous */
void     pmm_free(uint32_t addr);
uint32_t pmm_free_frames(void);
uint32_t pmm_total_frames(void);

/* Paging. */
#define PTE_P  0x001
#define PTE_W  0x002
#define PTE_U  0x004
#define PTE_PS 0x080

void      paging_init(void);
uint32_t *paging_kernel_dir(void);
uint32_t *paging_new_dir(void);                       /* copy of kernel mappings */
void      paging_free_dir(uint32_t *pd);              /* frees user pages too */
int       paging_map_user(uint32_t *pd, uint32_t va); /* allocate+map one zeroed page */
uint32_t  paging_virt_to_phys(uint32_t *pd, uint32_t va);  /* 0 if unmapped */
void      paging_switch(uint32_t *pd);
void      paging_map_identity_range(uint32_t phys, uint32_t size);
uint32_t  paging_count_user_pages(uint32_t *pd);

/* Kernel heap. */
void   heap_init(uint32_t start, uint32_t size);
void  *kmalloc(size_t n);
void  *kzalloc(size_t n);
void   kfree(void *p);
void   heap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks);
#endif
