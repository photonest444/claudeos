/* guess - number guessing game. Demonstrates keyboard input via read(). */
#include "../lib/claudeos.h"

int main(void) {
    srand(ticks_ms());
    int secret = rand() % 100 + 1, tries = 0;
    char line[32];
    printf("I'm thinking of a number between 1 and 100.\n");
    for (;;) {
        printf("Your guess: ");
        if (!gets_s(line, sizeof(line))) return 1;
        int g = atoi(line);
        tries++;
        if (g < secret) printf("Too low!\n");
        else if (g > secret) printf("Too high!\n");
        else { printf("\x1b[92mCorrect! You got it in %d tries.\x1b[0m\n", tries); return 0; }
    }
}
