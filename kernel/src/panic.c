#include <stdio.h>
#include <stdint.h>
#include <ansii.h>
#include <cpu/stack_trace.h>
#include <drivers/serial/serial.h>

/// @brief Raise an exception and halt the system
/// @param reason the kernel can choose a reason to relay to the user
__attribute__((noreturn))
void ke_panic(const char* reason){
    __asm__ volatile ("cli");

    serial_write("Kernel Panic!\n");

    kprintf("\n");
    kprintf(RED_FG "===============================================\n");
    kprintf("   !!!  FATAL KERNEL EXCEPTION OCCURRED  !!!\n");
    kprintf("===============================================\n" RESET);

    kprintf(ORANGE_FG "(Kernel Raised) EXCEPTION: %s\n", reason);

    uint64_t cur_rbp;
    __asm__ volatile ("mov %%rbp, %0" : "=r"(cur_rbp));
    stack_trace_print((uint64_t*)cur_rbp);
    kprintf("\n" LOG_INFO " SYSTEM HALTED\n" RESET);
    __asm__ volatile ("hlt");
    for(;;){}
}