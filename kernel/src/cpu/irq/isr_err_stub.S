.intel_syntax noprefix
.text

.extern ke_processor_exception
.extern irq_handler
.extern timer_handle
.extern syscall_dispatch
.extern apic_eoi

.macro ISR_NO_ERR v
.globl isr_\v
.type  isr_\v,@function
isr_\v:
    push 0
    push \v
    jmp  isr_common
.size isr_\v, .-isr_\v
.endm

.macro ISR_ERR v
.globl isr_\v
.type  isr_\v,@function
isr_\v:
    push \v
    jmp  isr_common
.size isr_\v, .-isr_\v
.endm

.macro IRQ_STUB irq vector
.globl irq\irq
.type  irq\irq,@function
irq\irq:
    push \irq        /* IRQ number — pushed BEFORE any register clobber */
    push \vector
    jmp  irq_common
.size irq\irq, .-irq\irq
.endm

ISR_NO_ERR 0
ISR_NO_ERR 1
ISR_NO_ERR 2
ISR_NO_ERR 3
ISR_NO_ERR 4
ISR_NO_ERR 5
ISR_NO_ERR 6
ISR_NO_ERR 7
ISR_ERR    8
ISR_NO_ERR 9
ISR_ERR    10
ISR_ERR    11
ISR_ERR    12
ISR_ERR    13
ISR_ERR    14
ISR_NO_ERR 15
ISR_NO_ERR 16
ISR_ERR    17
ISR_NO_ERR 18
ISR_NO_ERR 19
ISR_NO_ERR 20
ISR_NO_ERR 21
ISR_NO_ERR 22
ISR_NO_ERR 23
ISR_NO_ERR 24
ISR_NO_ERR 25
ISR_NO_ERR 26
ISR_NO_ERR 27
ISR_NO_ERR 28
ISR_NO_ERR 29
ISR_NO_ERR 30
ISR_NO_ERR 31

.type isr_common,@function
isr_common:
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

    mov  rdi, rsp
    call ke_processor_exception
    mov  rsp, rax

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
    pop rax

    add  rsp, 16
    iretq
.size isr_common, .-isr_common

.align 8
.globl isr_stub_table
isr_stub_table:
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,\
            16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        .quad isr_\n
    .endr

.globl irq0
.type  irq0,@function
irq0:
    push 0
    push 32
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

    mov  rdi, rsp
    call timer_handle
    mov  rsp, rax
    call apic_eoi

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
    pop rax

    add  rsp, 16
    iretq
.size irq0, .-irq0

IRQ_STUB 1,  33
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

.type irq_common,@function
irq_common:
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
    
    mov  rdi, [rsp + 128]
    lea  rsi, [rsp]
    call irq_handler
    call apic_eoi

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
    pop rax

    add  rsp, 16
    iretq
.size irq_common, .-irq_common

.align 8
.globl irq_stub_table
irq_stub_table:
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        .quad irq\n
    .endr

.globl irq80
.type  irq80,@function
irq80:
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

    mov  rdi, rsp
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

    add  rsp, 16
    iretq
.size irq80, .-irq80