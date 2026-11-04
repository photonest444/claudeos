; ---------------------------------------------------------------------------
; Interrupt entry stubs. Every vector pushes (error code, vector number), then
; jumps to isr_common which saves all registers and calls the C dispatcher.
; The dispatcher returns the stack pointer to resume from: if the scheduler
; picked another task, this is that task's saved frame -> context switch.
; ---------------------------------------------------------------------------
extern interrupt_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1                ; CPU already pushed an error code
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
%assign i 22
%rep 10
ISR_NOERR i
%assign i i+1
%endrep
; hardware IRQs 0-15 remapped to vectors 32-47
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep
ISR_NOERR 128                   ; int 0x80: system call
ISR_NOERR 129                   ; int 0x81: yield (kernel-internal)

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10                ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp                    ; argument: regs_t*
    call interrupt_dispatch
    mov esp, eax                ; maybe a different task's stack now
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                  ; drop vector number + error code
    iret

; Load the GDT and reload all segment registers.
global gdt_flush
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload_cs
.reload_cs:
    mov ax, 0x28                ; TSS selector
    ltr ax
    ret

global idt_flush
idt_flush:
    mov eax, [esp + 4]
    lidt [eax]
    ret
