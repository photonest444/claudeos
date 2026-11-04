/* counter [N] - prints a line every second. Try "counter 30 &" and then "ps",
 * or run it and press Ctrl+C. Demonstrates sleeping and multitasking. */
#include "../lib/claudeos.h"

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 10;
    for (int i = 1; i <= n; i++) {
        printf("[counter pid %d] tick %d/%d\n", getpid(), i, n);
        sleep_ms(1000);
    }
    return 0;
}
