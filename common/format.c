/* Small, dependency-free printf formatter (no floating point). */
#include "format.h"

typedef struct { char *buf; size_t size, pos; } out_t;

static void put(out_t *o, char c) {
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void put_padded(out_t *o, const char *s, int len, int width, int left, char pad) {
    if (!left) for (int i = len; i < width; i++) put(o, pad);
    for (int i = 0; i < len; i++) put(o, s[i]);
    if (left) for (int i = len; i < width; i++) put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    out_t o = { buf, size, 0 };
    char tmp[16];
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        int left = 0, width = 0; char pad = ' ';
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p': {
            unsigned int v; int neg = 0, base = 10, n = 0;
            const char *digits = "0123456789abcdef";
            if (*fmt == 'd' || *fmt == 'i') {
                int sv = va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (unsigned int)(-(sv + 1)) + 1u; } else v = (unsigned int)sv;
            } else {
                v = va_arg(ap, unsigned int);
                if (*fmt != 'u') base = 16;
                if (*fmt == 'X') digits = "0123456789ABCDEF";
                if (*fmt == 'p') { put(&o, '0'); put(&o, 'x'); width = 8; pad = '0'; }
            }
            do { tmp[n++] = digits[v % base]; v /= base; } while (v);
            char num[18]; int len = 0;
            if (neg && pad == '0') { put(&o, '-'); width--; }
            else if (neg) num[len++] = '-';
            while (n) num[len++] = tmp[--n];
            put_padded(&o, num, len, width, left, pad);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0; while (s[len]) len++;
            put_padded(&o, s, len, width, left, ' ');
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); put_padded(&o, &c, 1, width, left, ' '); break; }
        case '%': put(&o, '%'); break;
        case 0: fmt--; break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
