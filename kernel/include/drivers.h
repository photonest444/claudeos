/* Hardware drivers: serial, PIT, PS/2 keyboard+mouse, RTC, ATA, power. */
#ifndef DRIVERS_H
#define DRIVERS_H
#include "types.h"

/* Serial port COM1: kernel log mirror + optional input for the CLI console. */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
void serial_enable_input(void);       /* after the console exists */

/* Programmable interval timer. */
#define TIMER_HZ 1000
void     pit_init(void);
uint32_t timer_ticks(void);            /* milliseconds since boot */

/* Keyboard: special (non-ASCII) key codes start at 0x100. */
enum {
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};
void keyboard_init(void);
void keyboard_route(int key);          /* send a key to the active display (CLI/GUI) */

/* PS/2 mouse: events are queued for the GUI. */
typedef struct { int x, y, buttons; } mouse_event_t;
void mouse_init(void);
int  mouse_poll(mouse_event_t *ev);    /* 1 if an event was dequeued */
void mouse_set_bounds(int w, int h);
void mouse_get(int *x, int *y, int *buttons);

/* CMOS real-time clock. */
typedef struct { int year, month, day, hour, min, sec; } rtc_time_t;
void rtc_read(rtc_time_t *t);

/* Power management (ACPI with emulator fallbacks). */
void acpi_init(void);
void power_reboot(void) __attribute__((noreturn));
void power_shutdown(void) __attribute__((noreturn));

/* Block devices (ATA PIO disk and RAM disk). */
typedef struct blockdev {
    char     name[8];
    uint32_t sectors;               /* 512-byte sectors */
    int (*read)(struct blockdev *d, uint32_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf);
    void    *priv;
} blockdev_t;

#define MAX_BLOCKDEVS 4
void        ata_init(void);
void        ramdisk_init(uint32_t start, uint32_t end);
void        blockdev_register(blockdev_t *d);
blockdev_t *blockdev_get(int i);
blockdev_t *blockdev_find(const char *name);
#endif
