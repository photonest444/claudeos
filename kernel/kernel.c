/* Kernel entry point: brings up the hardware and subsystems in dependency
 * order, starts the system tasks and then becomes the idle task. */
#include "include/types.h"
#include "include/multiboot.h"
#include "include/arch.h"
#include "include/mm.h"
#include "include/drivers.h"
#include "include/gfx.h"
#include "include/tty.h"
#include "include/task.h"
#include "include/fs.h"
#include "include/gui.h"
#include "include/shell.h"
#include "include/string.h"
#include "include/kprintf.h"
#include "include/io.h"

boot_info_t boot_info;

/* Copy what we need out of the multiboot structure before it can be overwritten. */
static void parse_multiboot(multiboot_info_t *mbi) {
    if ((mbi->flags & MB_FLAG_CMDLINE) && mbi->cmdline)
        strlcpy(boot_info.cmdline, (const char *)mbi->cmdline, sizeof(boot_info.cmdline));
    if ((mbi->flags & MB_FLAG_MODS) && mbi->mods_count) {
        mb_module_t *m = (mb_module_t *)mbi->mods_addr;
        boot_info.initrd_start = m->mod_start;
        boot_info.initrd_end = m->mod_end;
    }
    if (mbi->flags & MB_FLAG_FRAMEBUFFER) {
        boot_info.fb_addr = mbi->framebuffer_addr;
        boot_info.fb_pitch = mbi->framebuffer_pitch;
        boot_info.fb_width = mbi->framebuffer_width;
        boot_info.fb_height = mbi->framebuffer_height;
        boot_info.fb_bpp = mbi->framebuffer_bpp;
        boot_info.fb_type = mbi->framebuffer_type;
    } else {
        boot_info.fb_type = 2;                       /* EGA text mode */
    }
}

static void mount_root(void) {
    /* Prefer a persistent hard disk if it holds a FAT file system. */
    blockdev_t *hd = blockdev_find("hd0"), *ram = blockdev_find("ram0");
    if (hd && fs_mount(hd) == FS_OK) return;
    if (ram && fs_mount(ram) == FS_OK) return;
    kprintf("[fs] WARNING: no FAT file system found (no initrd module and no disk)\n");
}

void kmain(uint32_t magic, multiboot_info_t *mbi) {
    serial_init();
    if (magic != MULTIBOOT_MAGIC) {
        serial_write("not booted by a multiboot loader\n", 33);
        for (;;) cpu_halt();
    }
    parse_multiboot(mbi);

    gdt_init();                 /* segments + TSS */
    idt_init();                 /* exceptions, IRQs, syscalls; PIC remapped */
    pmm_init(mbi);              /* physical frame allocator from the memory map */
    paging_init();              /* identity map 0-1 GiB, enable paging */

    uint32_t heap_pages = MIN(4096u, pmm_free_frames() / 2);    /* up to 16 MiB */
    uint32_t heap = pmm_alloc_contiguous(heap_pages);
    if (!heap) panic("cannot allocate kernel heap");
    heap_init(heap, heap_pages * PAGE_SIZE);

    gfx_init();                 /* VESA framebuffer from GRUB (optional) */
    console_init();             /* from here on kprintf is visible */

    kprintf("\x1b[96mClaudeOS\x1b[0m kernel booting (cmdline: '%s')\n", boot_info.cmdline);
    kprintf("[mem] %u MB usable RAM, heap %u KB at %p\n", boot_info.mem_kb / 1024,
            heap_pages * 4, heap);
    if (!gfx_available()) kprintf("[gfx] no framebuffer: VGA text mode, GUI disabled\n");

    tasking_init();
    pit_init();
    keyboard_init();
    mouse_init();
    serial_enable_input();
    acpi_init();
    ata_init();
    ramdisk_init(boot_info.initrd_start, boot_info.initrd_end);
    mount_root();

    int gui_pid = task_create_kernel("display", gui_task, NULL, NULL);
    int sh_pid = task_create_kernel("shell", shell_main, console_tty, console_tty);
    task_get(gui_pid)->system = true;
    task_get(sh_pid)->system = true;
    kprintf("[boot] ready: %u KB free. Press F12 or type 'gui' for the desktop.\n\n",
            pmm_free_frames() * 4);
    if (strstr(boot_info.cmdline, "gui")) display_set_mode(DISPLAY_GUI);

    /* Idle task: sleep until the next interrupt, forever. */
    for (;;) __asm__ volatile("sti; hlt");
}
