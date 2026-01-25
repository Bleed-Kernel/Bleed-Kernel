/*
    This file has been dramatically changed:
        Before, there was an issue that made kernel panics
        useless, becuase RIP would always point to the 
        save cpu registers function, this reform
        aims to completly fix that silly issue
*/

#include <stdint.h>
#include <stdio.h>
#include <ansii.h>
#include <drivers/serial/serial.h>
#include <cpu/stack_trace.h>
#include <drivers/framebuffer/framebuffer.h>

struct isr_stackframe {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} __attribute__((packed));

const char* exception_name(uint8_t vector) {
    static const char* names[32] = {
        "Divide Error", "Debug", "Non-maskable Interrupt", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid tss_t", "Segment Not Present",
        "Stack Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 FPU Floating-Point Error", "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
        "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Security Exception", "Reserved"
    };
    return vector < 32 ? names[vector] : "Unknown";
}

extern void ke_exception_handler(void *frame){
    __asm__ volatile ("cli");
    struct isr_stackframe *f = (struct isr_stackframe *)frame;

    uint64_t vector = f->vector;
    uint64_t err    = f->error_code;
    uint64_t rip    = f->rip;
    uint64_t cs     = f->cs;
    uint64_t rflags = f->rflags;

    uint64_t sym_addr;
    const char *name = stack_trace_symbol_lookup(rip, &sym_addr);

    serial_write("Kernel Panic!\n");
    serial_write_hex(vector);
    serial_write("\n");
    serial_write_hex(f->rip);
    serial_write("\n");

    kprintf("\n");
    kprintf(RED_FG "===============================================\n");
    kprintf("   !!!  FATAL KERNEL EXCEPTION OCCURRED  !!!\n");
    kprintf("===============================================\n" RESET);

    kprintf(ORANGE_FG " (CPU Raised) EXCEPTION: %s\n" RESET, exception_name(vector));
    kprintf(GREEN_FG " VECTOR: %llu   ERROR: 0x%p\n", vector, (void *)err);

    kprintf("\n" ORANGE_FG " CPU STATE:" RESET "\n");   // todo: i didnt know you could litterally do this to use colour codes, ill change this across the rest of this file later
    if (name) {
        kprintf(" RIP: " GREEN_FG "0x%p" RESET "   CS: 0x%p   RFLAGS: 0x%p\n",
                (void*)rip, (void*)cs, (void*)rflags);
        kprintf("      " ORANGE_FG "<%s+0x%llu>%s\n", name, rip - sym_addr, RESET);
    } else {
        kprintf(" RIP: " GREEN_FG "0x%p" RESET "   CS: 0x%p   RFLAGS: 0x%p\n",
                (void*)rip, (void*)cs, (void*)rflags);
    }


    kprintf("\n" ORANGE_FG " Registers:" RESET "\n");
    kprintf(" RAX: %s0x%p%s\t  RBX: %s0x%p%s\t  RCX: %s0x%p%s\t  RDX: %s0x%p%s\t\n",
            GREEN_FG, (void *)f->rax, RESET, GREEN_FG, (void *)f->rbx, RESET, GREEN_FG, (void *)f->rcx, RESET, GREEN_FG, (void *)f->rdx, RESET);

    kprintf(" RSI: %s0x%p%s\t  RDI: %s0x%p%s\t  RBP: %s0x%p%s\t\n",
            GREEN_FG, (void *)f->rsi, RESET, GREEN_FG, (void *)f->rdi, RESET, GREEN_FG, (void *)f->rbp, RESET);

    kprintf(" R8 : %s0x%p%s\t  R9 : %s0x%p%s\t  R10: %s0x%p%s\t  R11: %s0x%p%s\t\n",
            GREEN_FG, (void *)f->r8, RESET, GREEN_FG, (void *)f->r9, RESET, GREEN_FG, (void *)f->r10, RESET, GREEN_FG, (void *)f->r11, RESET);

    kprintf(" R12: %s0x%p%s\t  R13: %s0x%p%s\t  R14: %s0x%p%s\t  R15: %s0x%p%s\t\n",
            GREEN_FG, (void *)f->r12, RESET, GREEN_FG, (void *)f->r13, RESET, GREEN_FG, (void *)f->r14, RESET, GREEN_FG, (void *)f->r15, RESET);

    if (vector == 14){
        kprintf("\n" ORANGE_FG " PAGE FAULT DETAILS\n" RESET);
        uint64_t cr2 = 0; 
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        kprintf("  Faulting address: " GREEN_FG "0x%p" RESET "\n", (void*)cr2);
        kprintf("  Error code: %s 0x%p%s\n",GREEN_FG, (void*)err, RESET);
        kprintf("   P  = %s%u%s\n", GREEN_FG, (unsigned)((err >> 0) & 1), RESET);
        kprintf("   W/R= %s%u%s\n", GREEN_FG, (unsigned)((err >> 1) & 1), RESET);
        kprintf("   U/S= %s%u%s\n", GREEN_FG, (unsigned)((err >> 2) & 1), RESET);
        kprintf("   RSV= %s%u%s\n", GREEN_FG, (unsigned)((err >> 3) & 1), RESET);
        kprintf("   I/D= %s%u%s\n", GREEN_FG, (unsigned)((err >> 4) & 1), RESET);
    }

    uint64_t cur_rbp;
    __asm__ volatile ("mov %%rbp, %0" : "=r"(cur_rbp));
    stack_trace_print((uint64_t*)cur_rbp);
    kprintf("\n" LOG_INFO " SYSTEM HALTED\n" RESET);
    __asm__ volatile ("hlt");
    for(;;){}
}
