/* spin - burns CPU forever without any system call. Shows preemptive
 * scheduling: the system stays responsive; stop it with Ctrl+C or kill. */
#include "../lib/claudeos.h"

int main(void) {
    printf("Spinning in an endless loop (pid %d). Press Ctrl+C to stop.\n", getpid());
    volatile unsigned x = 0;
    for (;;) x++;
}
