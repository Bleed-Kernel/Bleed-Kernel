.intel_syntax noprefix
#include <cpu/percpu.h>

.text
.extern syscall_dispatch
.extern sched_return

.equ USER_CS, 0x1b
.equ USER_SS, 0x23

.globl syscall_entry
.type syscall_entry,@function
syscall_entry:
    swapgs
    mov gs:[CPU_USER_RSP], rsp
    mov rsp, gs:[CPU_KERNEL_RSP]

    // cpu_context_t frame
    push USER_SS
    push qword ptr gs:[CPU_USER_RSP]
    push r11
    push USER_CS
    push rcx
    push 0
    push 0x80

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call syscall_dispatch
    mov rdi, rsp
    call sched_return
    mov rax, [rsp + 112]

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx

    // Switch back to the User context
    cli
    add rsp, 24 // skip saved rax. vector and error.

    // sigreturn can hand back a different frame
    test qword ptr [rsp + 8], 3
    jz   1f
    swapgs
1:
    iretq
.size syscall_entry, .-syscall_entry
