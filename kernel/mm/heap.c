/* Kernel heap: first-fit allocator over one contiguous region.
 * Every block starts with a 16-byte header; adjacent free blocks are merged
 * lazily while searching and eagerly on free (forward direction). */
#include "../include/mm.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/kprintf.h"

#define HEAP_MAGIC 0xC0FFEE42u

typedef struct {
    uint32_t size;      /* whole block including header, multiple of 16 */
    uint32_t used;
    uint32_t magic;
    uint32_t pad;
} hdr_t;

static uint8_t *heap_start, *heap_end;

void heap_init(uint32_t start, uint32_t size) {
    heap_start = (uint8_t *)start;
    heap_end = heap_start + (size & ~15u);
    hdr_t *h = (hdr_t *)heap_start;
    h->size = heap_end - heap_start;
    h->used = 0;
    h->magic = HEAP_MAGIC;
}

static void merge_forward(hdr_t *h) {
    for (;;) {
        hdr_t *n = (hdr_t *)((uint8_t *)h + h->size);
        if ((uint8_t *)n >= heap_end || n->used) return;
        h->size += n->size;
    }
}

void *kmalloc(size_t n) {
    if (!n) return NULL;
    uint32_t need = ALIGN_UP(n, 16) + sizeof(hdr_t);
    uint32_t f = irq_save();
    for (uint8_t *p = heap_start; p < heap_end; p += ((hdr_t *)p)->size) {
        hdr_t *h = (hdr_t *)p;
        if (h->magic != HEAP_MAGIC) panic("heap corrupted at %p", p);
        if (h->used) continue;
        merge_forward(h);
        if (h->size < need) continue;
        if (h->size - need >= 64) {              /* split off the remainder */
            hdr_t *rest = (hdr_t *)(p + need);
            rest->size = h->size - need;
            rest->used = 0;
            rest->magic = HEAP_MAGIC;
            h->size = need;
        }
        h->used = 1;
        irq_restore(f);
        return p + sizeof(hdr_t);
    }
    irq_restore(f);
    kprintf("[heap] out of memory (%u bytes)\n", (uint32_t)n);
    return NULL;
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;
    hdr_t *h = (hdr_t *)((uint8_t *)ptr - sizeof(hdr_t));
    if (h->magic != HEAP_MAGIC || !h->used) panic("kfree: bad pointer %p", ptr);
    uint32_t f = irq_save();
    h->used = 0;
    merge_forward(h);
    irq_restore(f);
}

void heap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks) {
    uint32_t f = irq_save(), u = 0, b = 0;
    for (uint8_t *p = heap_start; p < heap_end; p += ((hdr_t *)p)->size) {
        hdr_t *h = (hdr_t *)p;
        if (h->used) { u += h->size; b++; }
    }
    irq_restore(f);
    *total = heap_end - heap_start; *used = u; *blocks = b;
}
