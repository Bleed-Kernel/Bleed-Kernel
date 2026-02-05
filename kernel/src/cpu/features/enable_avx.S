    .text
    .globl avx_enable
    .type avx_enable, @function
avx_enable:
    movq %cr4, %rax
    orq $0x40000, %rax
    movq %rax, %cr4

    xorq %rcx, %rcx
    xgetbv

    orl $7, %eax
    xsetbv
    ret