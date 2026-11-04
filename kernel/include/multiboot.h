/* Multiboot (v1) structures passed to us by GRUB. */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H
#include "types.h"

#define MULTIBOOT_MAGIC        0x2BADB002
#define MB_FLAG_MEM            (1 << 0)
#define MB_FLAG_CMDLINE        (1 << 2)
#define MB_FLAG_MODS           (1 << 3)
#define MB_FLAG_MMAP           (1 << 6)
#define MB_FLAG_FRAMEBUFFER    (1 << 12)

typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch, framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;      /* 0 = indexed, 1 = RGB, 2 = EGA text */
    uint8_t  color_info[6];
} multiboot_info_t;

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t addr, len;
    uint32_t type;                  /* 1 = usable RAM */
} mb_mmap_t;

typedef struct __attribute__((packed)) {
    uint32_t mod_start, mod_end, string, reserved;
} mb_module_t;

/* Information we copy out of the multiboot structure early in boot. */
typedef struct {
    char     cmdline[128];
    uint32_t initrd_start, initrd_end;      /* first boot module, 0 if none */
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height, fb_bpp, fb_type;
    uint32_t mem_kb;                        /* total usable memory in KB */
} boot_info_t;

extern boot_info_t boot_info;
#endif
