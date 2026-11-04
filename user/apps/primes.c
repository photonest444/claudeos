/* primes [N] - print prime numbers up to N (default 200) with a sieve.
 * Demonstrates malloc() (backed by the sbrk system call). */
#include "../lib/claudeos.h"

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 200;
    if (n < 2 || n > 1000000) { printf("usage: primes [2..1000000]\n"); return 1; }
    char *composite = malloc(n + 1);
    memset(composite, 0, n + 1);
    int count = 0;
    unsigned t0 = ticks_ms();
    for (int i = 2; i <= n; i++) {
        if (composite[i]) continue;
        count++;
        if (n <= 2000) printf("%d ", i);
        for (int j = i * 2; j <= n; j += i) composite[j] = 1;
    }
    printf("\n%d primes <= %d (computed in %u ms)\n", count, n, ticks_ms() - t0);
    free(composite);
    return 0;
}
