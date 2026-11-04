/* wc <file> - count lines, words and bytes. Demonstrates open()/read(). */
#include "../lib/claudeos.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: wc <file>\n"); return 1; }
    int fd = open(argv[1]);
    if (fd < 0) { printf("wc: cannot open %s\n", argv[1]); return 1; }
    char buf[512];
    int n, lines = 0, words = 0, bytes = 0, inword = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') inword = 0;
            else if (!inword) { inword = 1; words++; }
        }
        bytes += n;
    }
    close(fd);
    printf("%7d %7d %7d %s\n", lines, words, bytes, argv[1]);
    return 0;
}
