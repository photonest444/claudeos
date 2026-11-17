/* System-wide text clipboard shared by the GUI, terminals and the console. */
#ifndef CLIPBOARD_H
#define CLIPBOARD_H
#include "types.h"

#define CLIPBOARD_MAX 8192

void clipboard_set(const char *text, int len);
int  clipboard_get(char *buf, int max);        /* copies, NUL-terminates, returns length */
int  clipboard_len(void);
#endif
