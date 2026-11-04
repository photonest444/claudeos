/* Block device registry + RAM disk backed by the GRUB boot module (initrd). */
#include "../include/drivers.h"
#include "../include/string.h"
#include "../include/kprintf.h"

static blockdev_t *devs[MAX_BLOCKDEVS];
static blockdev_t ram0;

void blockdev_register(blockdev_t *d) {
    for (int i = 0; i < MAX_BLOCKDEVS; i++)
        if (!devs[i]) { devs[i] = d; return; }
}

blockdev_t *blockdev_get(int i) { return i >= 0 && i < MAX_BLOCKDEVS ? devs[i] : NULL; }

blockdev_t *blockdev_find(const char *name) {
    for (int i = 0; i < MAX_BLOCKDEVS; i++)
        if (devs[i] && !strcmp(devs[i]->name, name)) return devs[i];
    return NULL;
}

static int ram_read(blockdev_t *d, uint32_t lba, uint32_t count, void *buf) {
    if (lba + count > d->sectors) return -1;
    memcpy(buf, (uint8_t *)d->priv + lba * 512, count * 512);
    return 0;
}

static int ram_write(blockdev_t *d, uint32_t lba, uint32_t count, const void *buf) {
    if (lba + count > d->sectors) return -1;
    memcpy((uint8_t *)d->priv + lba * 512, buf, count * 512);
    return 0;
}

void ramdisk_init(uint32_t start, uint32_t end) {
    if (!start || end <= start) return;
    strlcpy(ram0.name, "ram0", sizeof(ram0.name));
    ram0.sectors = (end - start) / 512;
    ram0.priv = (void *)start;
    ram0.read = ram_read;
    ram0.write = ram_write;
    blockdev_register(&ram0);
    kprintf("[ramdisk] ram0: %u KB at %p\n", (end - start) / 1024, start);
}
