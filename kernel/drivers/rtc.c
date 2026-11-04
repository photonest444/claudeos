/* CMOS real-time clock (date and time, as kept by the VM/BIOS, usually UTC). */
#include "../include/drivers.h"
#include "../include/io.h"

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_read(rtc_time_t *t) {
    uint32_t f = irq_save();
    while (cmos(0x0A) & 0x80) ;              /* wait while an update is in progress */
    int s = cmos(0x00), m = cmos(0x02), h = cmos(0x04);
    int d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    uint8_t b = cmos(0x0B);
    irq_restore(f);
    if (!(b & 0x04)) {                       /* BCD mode */
        s = bcd(s); m = bcd(m); d = bcd(d); mo = bcd(mo); y = bcd(y);
        h = bcd(h & 0x7F) | (h & 0x80);
    }
    if (!(b & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;   /* 12h mode */
    t->sec = s; t->min = m; t->hour = h; t->day = d; t->month = mo; t->year = 2000 + y;
}
