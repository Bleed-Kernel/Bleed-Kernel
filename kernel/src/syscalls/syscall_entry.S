.intel_syntax noprefix

.section .bss
.align 8
.globl syscall_saved_user_rsp
syscall_saved_user_rsp:
    .zero 8

.text
.extern syscall_dispatch
.extern tss

.equ USER_CS, 0x1b
.equ USER_SS, 0x23
.equ TSS_RSP0_OFFSET, 4

.globl syscall_entry
.type syscall_entry,@function
syscall_entry:
    mov [rip + syscall_saved_user_rsp], rsp
    mov rsp, [rip + tss + TSS_RSP0_OFFSET]

    // cpu_context_t frame
    push USER_SS
    push qword ptr [rip + syscall_saved_user_rsp]
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
    add rsp, 8          // skip saved rax
    add rsp, 16         // skip vector + error
    iretq
.size syscall_entry, .-syscall_entry
