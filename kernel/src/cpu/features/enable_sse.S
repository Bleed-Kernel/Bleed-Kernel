    .data
sse_result:
    .float 0.0

valA:
    .float 1.5
valB:
    .float 2.5

.text
.globl sse_enable
sse_enable:
    mov %cr0, %rax
    and $~0x4, %rax
    or  $0x2, %rax
    mov %rax, %cr0

    mov %cr4, %rax
    or  $(1 << 9) | (1 << 10), %rax
    mov %rax, %cr4

    movss valA(%rip), %xmm0
    movss valB(%rip), %xmm1
    addss %xmm1, %xmm0
    movss %xmm0, sse_result(%rip)

    ret
