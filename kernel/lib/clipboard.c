/* System-wide text clipboard. Interrupts are disabled while copying so a
 * task switch can never observe a half-written clipboard. */
#include "../include/clipboard.h"
#include "../include/io.h"
#include "../include/string.h"

static char clip[CLIPBOARD_MAX];
static int  clip_len;

void clipboard_set(const char *text, int len) {
    if (len < 0) len = 0;
    if (len > CLIPBOARD_MAX - 1) len = CLIPBOARD_MAX - 1;
    uint32_t f = irq_save();
    memcpy(clip, text, len);
    clip[len] = 0;
    clip_len = len;
    irq_restore(f);
}

int clipboard_get(char *buf, int max) {
    if (max <= 0) return 0;
    uint32_t f = irq_save();
    int n = MIN(clip_len, max - 1);
    memcpy(buf, clip, n);
    buf[n] = 0;
    irq_restore(f);
    return n;
}

int clipboard_len(void) { return clip_len; }
