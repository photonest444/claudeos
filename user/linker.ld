/* User programs are loaded at 1 GiB (USER_BASE in the kernel). */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text ALIGN(4K) : { *(.text.start) *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : { *(.data*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
