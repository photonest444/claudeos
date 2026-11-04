; ---------------------------------------------------------------------------
; Multiboot entry point. GRUB loads us in 32-bit protected mode with paging
; off, EAX = multiboot magic, EBX = physical address of the multiboot info.
; ---------------------------------------------------------------------------
MB_ALIGN    equ 1 << 0          ; align modules on page boundaries
MB_MEMINFO  equ 1 << 1          ; give us a memory map
MB_VIDEO    equ 1 << 2          ; give us a video mode (below)
MB_FLAGS    equ MB_ALIGN | MB_MEMINFO | MB_VIDEO
MB_MAGIC    equ 0x1BADB002

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd -(MB_MAGIC + MB_FLAGS)   ; checksum
    dd 0, 0, 0, 0, 0            ; a.out kludge fields (unused, we are ELF)
    dd 0                        ; mode_type: 0 = linear framebuffer
    dd 1024                     ; preferred width
    dd 768                      ; preferred height
    dd 32                       ; preferred depth

section .bss
align 16
global boot_stack_top
boot_stack_bottom:
    resb 16384
boot_stack_top:

section .text
global _start
extern kmain
_start:
    mov esp, boot_stack_top
    push 0                      ; clear EFLAGS
    popf
    push ebx                    ; multiboot info
    push eax                    ; magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang
