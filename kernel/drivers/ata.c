/* ATA (IDE) disk driver, PIO mode, LBA28, primary channel master only.
 * Polling is used instead of IRQs (nIEN is set) to keep things simple. */
#include "../include/drivers.h"
#include "../include/io.h"
#include "../include/kprintf.h"

#define ATA_DATA    0x1F0
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_CMD     0x1F7      /* write: command, read: status */
#define ATA_CTRL    0x3F6
#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

static blockdev_t hd0;

static int wait_ready(bool need_drq) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_CMD);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -1;
        if (!need_drq || (s & ST_DRQ)) return 0;
    }
    return -1;
}

static void setup(uint32_t lba, uint8_t count) {
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* master, LBA mode */
    outb(ATA_SECCNT, count);
    outb(ATA_LBA0, lba & 0xFF);
    outb(ATA_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_LBA2, (lba >> 16) & 0xFF);
}

static int ata_read(blockdev_t *d, uint32_t lba, uint32_t count, void *buf) {
    UNUSED(d);
    uint16_t *p = buf;
    uint32_t f = irq_save();
    while (count) {
        uint8_t n = count > 255 ? 255 : count;
        if (wait_ready(false)) goto fail;
        setup(lba, n);
        outb(ATA_CMD, 0x20);                         /* READ SECTORS */
        for (int s = 0; s < n; s++) {
            if (wait_ready(true)) goto fail;
            for (int i = 0; i < 256; i++) *p++ = inw(ATA_DATA);
        }
        lba += n; count -= n;
    }
    irq_restore(f);
    return 0;
fail:
    irq_restore(f);
    return -1;
}

static int ata_write(blockdev_t *d, uint32_t lba, uint32_t count, const void *buf) {
    UNUSED(d);
    const uint16_t *p = buf;
    uint32_t f = irq_save();
    while (count) {
        uint8_t n = count > 255 ? 255 : count;
        if (wait_ready(false)) goto fail;
        setup(lba, n);
        outb(ATA_CMD, 0x30);                         /* WRITE SECTORS */
        for (int s = 0; s < n; s++) {
            if (wait_ready(true)) goto fail;
            for (int i = 0; i < 256; i++) outw(ATA_DATA, *p++);
        }
        lba += n; count -= n;
    }
    outb(ATA_CMD, 0xE7);                             /* CACHE FLUSH */
    wait_ready(false);
    irq_restore(f);
    return 0;
fail:
    irq_restore(f);
    return -1;
}

void ata_init(void) {
    outb(ATA_CTRL, 0x02);                            /* disable device IRQs */
    if (inb(ATA_CMD) == 0xFF) return;                /* floating bus: no controller */
    outb(ATA_DRIVE, 0xA0);
    io_wait(); io_wait(); io_wait(); io_wait();
    outb(ATA_SECCNT, 0); outb(ATA_LBA0, 0); outb(ATA_LBA1, 0); outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);                             /* IDENTIFY */
    if (inb(ATA_CMD) == 0) return;                   /* no drive */
    for (int i = 0; i < 1000000 && (inb(ATA_CMD) & ST_BSY); i++) ;
    if (inb(ATA_LBA1) || inb(ATA_LBA2)) return;      /* ATAPI/SATA, not plain ATA */
    if (wait_ready(true)) return;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    hd0.sectors = id[60] | ((uint32_t)id[61] << 16);
    if (!hd0.sectors) return;
    hd0.name[0] = 'h'; hd0.name[1] = 'd'; hd0.name[2] = '0'; hd0.name[3] = 0;
    hd0.read = ata_read;
    hd0.write = ata_write;
    blockdev_register(&hd0);
    kprintf("[ata] hd0: %u sectors (%u MB)\n", hd0.sectors, hd0.sectors / 2048);
}
