/* Reboot and shutdown.
 * Shutdown uses ACPI: find the FADT, read the \_S5_ sleep type from the DSDT
 * and write SLP_TYP|SLP_EN to PM1a_CNT. If that fails we try the well-known
 * emulator ports (QEMU, Bochs, VirtualBox). */
#include "../include/drivers.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/kprintf.h"
#include "../include/mm.h"

typedef struct __attribute__((packed)) {
    char sig[8]; uint8_t checksum; char oem[6]; uint8_t rev; uint32_t rsdt;
} rsdp_t;

typedef struct __attribute__((packed)) {
    char sig[4]; uint32_t length; uint8_t rev, checksum;
    char oem[6], oem_table[8]; uint32_t oem_rev, creator, creator_rev;
} sdt_t;

static uint32_t pm1a_cnt, pm1b_cnt, smi_cmd;
static uint8_t  acpi_enable_val;
static uint16_t slp_typa, slp_typb;
static bool     acpi_ok;

static bool checksum_ok(const void *p, uint32_t n) {
    uint8_t s = 0; const uint8_t *b = p;
    while (n--) s += *b++;
    return s == 0;
}

static rsdp_t *find_rsdp(void) {
    uint32_t ebda;
    /* BIOS data area word 0x40E = EBDA segment (read via asm: GCC dislikes near-NULL pointers) */
    __asm__ volatile("movzwl 0x40E, %0" : "=r"(ebda));
    ebda <<= 4;
    uint32_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int r = 0; r < 2; r++)
        for (uint32_t a = ranges[r][0]; a && a < ranges[r][1]; a += 16)
            if (!memcmp((void *)a, "RSD PTR ", 8) && checksum_ok((void *)a, 20))
                return (rsdp_t *)a;
    return NULL;
}

void acpi_init(void) {
    rsdp_t *rsdp = find_rsdp();
    if (!rsdp || rsdp->rsdt >= IDENTITY_LIMIT) return;
    sdt_t *rsdt = (sdt_t *)rsdp->rsdt;
    if (memcmp(rsdt->sig, "RSDT", 4)) return;
    uint32_t n = (rsdt->length - sizeof(sdt_t)) / 4;
    uint32_t *entries = (uint32_t *)(rsdt + 1);
    for (uint32_t i = 0; i < n; i++) {
        sdt_t *t = (sdt_t *)entries[i];
        if ((uint32_t)t >= IDENTITY_LIMIT || memcmp(t->sig, "FACP", 4)) continue;
        uint8_t *fadt = (uint8_t *)t;
        uint32_t dsdt = *(uint32_t *)(fadt + 40);
        smi_cmd = *(uint32_t *)(fadt + 48);
        acpi_enable_val = fadt[52];
        pm1a_cnt = *(uint32_t *)(fadt + 64);
        pm1b_cnt = *(uint32_t *)(fadt + 68);
        if (dsdt >= IDENTITY_LIMIT || memcmp((void *)dsdt, "DSDT", 4)) return;
        /* Scan the AML for: NameOp "_S5_" PackageOp PkgLength NumElements ... */
        uint8_t *p = (uint8_t *)dsdt + sizeof(sdt_t);
        uint32_t len = ((sdt_t *)dsdt)->length - sizeof(sdt_t);
        for (uint32_t k = 0; k + 4 < len; k++, p++) {
            if (memcmp(p, "_S5_", 4)) continue;
            if (!((p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\')) && p[4] == 0x12)) continue;
            p += 5;
            p += ((*p & 0xC0) >> 6) + 2;      /* skip PkgLength and NumElements */
            if (*p == 0x0A) p++;              /* BytePrefix */
            slp_typa = (uint16_t)(*p << 10);
            p++;
            if (*p == 0x0A) p++;
            slp_typb = (uint16_t)(*p << 10);
            acpi_ok = true;
            kprintf("[acpi] PM1a_CNT=%x S5 found, ACPI shutdown available\n", pm1a_cnt);
            return;
        }
    }
}

void power_shutdown(void) {
    __asm__ volatile("cli");
    if (acpi_ok && pm1a_cnt) {
        if (smi_cmd && acpi_enable_val && !(inw(pm1a_cnt) & 1)) {   /* SCI_EN off */
            outb(smi_cmd, acpi_enable_val);
            for (int i = 0; i < 300 && !(inw(pm1a_cnt) & 1); i++) io_wait();
        }
        outw(pm1a_cnt, slp_typa | (1 << 13));
        if (pm1b_cnt) outw(pm1b_cnt, slp_typb | (1 << 13));
    }
    outw(0x604, 0x2000);    /* QEMU (newer) */
    outw(0xB004, 0x2000);   /* Bochs / older QEMU */
    outw(0x4004, 0x3400);   /* VirtualBox */
    kprintf("\nIt is now safe to turn off your computer.\n");
    for (;;) cpu_halt();
}

void power_reboot(void) {
    __asm__ volatile("cli");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);       /* pulse the CPU reset line via the keyboard controller */
    /* Fallback: load an empty IDT and fault -> triple fault -> reset. */
    static const struct __attribute__((packed)) { uint16_t l; uint32_t b; } null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int $3" :: "m"(null_idt));
    for (;;) cpu_halt();
}
