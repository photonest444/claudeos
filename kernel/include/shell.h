#ifndef SHELL_H
#define SHELL_H
#include "tty.h"
/* Entry point of a shell task. arg = tty_t*. */
void shell_main(void *arg);
#endif
