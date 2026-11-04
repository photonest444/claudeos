/* hello - the classic first program. Shows arguments and the process id. */
#include "../lib/claudeos.h"

int main(int argc, char **argv) {
    printf("\x1b[92mHello from user space!\x1b[0m I am process %d running in ring 3.\n", getpid());
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
