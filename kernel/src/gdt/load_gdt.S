.code64
reload_segments:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %ss
    
    push    $0x08
    leaq    .flush_cs, %rax
    pushq   %rax
    retfq
    
.flush_cs:
    ret

.globl gdt_load
gdt_load:
    lgdt    (%rdi)
    call    reload_segments
    ret