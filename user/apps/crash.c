/* crash - tries to write into kernel memory. Memory protection (paging,
 * ring 3) makes the CPU raise a page fault and the kernel kills only this
 * process; the rest of the system keeps running. */
#include "../lib/claudeos.h"

int main(void) {
    printf("Writing to kernel memory at 0x00100000...\n");
    volatile int *kernel = (int *)0x00100000;
    *kernel = 0xDEADBEEF;
    printf("This line is never printed.\n");
    return 0;
}
