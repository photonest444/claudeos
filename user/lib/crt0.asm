; C runtime start-up for ClaudeOS programs.
; The kernel starts us with [esp] = argc, [esp+4] = argv.
section .text.start
global _start
extern main
extern exit
_start:
    mov eax, [esp]          ; argc
    mov ebx, [esp + 4]      ; argv
    and esp, 0xFFFFFFF0     ; 16-byte align the stack for the C ABI
    sub esp, 8
    push ebx
    push eax
    call main
    push eax
    call exit               ; never returns
.hang:
    jmp .hang
