#include <idt/idt.h>
#include <ansii.h>
#include <stdio.h>
#include <stdint.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>

#define DESCRIPTORS_COUNT       256
extern void* isr_stub_table[];
extern void* irq_stub_table[];
extern char irq80[];
extern char isr_yield[];

__attribute__((aligned(0x10)))
static idt_entry_t idt[DESCRIPTORS_COUNT];
static idt_ptr_t idt_ptr;

/// @brief set an idt descriptor, with a vector (index) isr and flags
/// @param vector vector index
/// @param isr address (isr stub table)
/// @param flags flags
static void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags){
    idt_entry_t* descriptor = &idt[vector];

    descriptor->offset16    = (uint64_t)isr & 0xFFFF;
    descriptor->selector    = 0x08;
    descriptor->ist         = 0;
    descriptor->attributes  = flags;
    descriptor->offset32    = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->offset64    = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->zero        = 0;
}

/// @brief initialise the new idt replacing the one from LIMINE
uint64_t idt_init(void){
    idt_ptr.address = (uintptr_t)&idt[0];
    idt_ptr.limit = (uint16_t)sizeof(idt_entry_t) * DESCRIPTORS_COUNT - 1;

    for (uint8_t vector = 0; vector < 32; vector++)
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);

    for (uint8_t irq = 0; irq < 16; irq++)
        idt_set_descriptor(32 + irq, irq_stub_table[irq], 0x8E);

    idt_set_descriptor(SCHED_YIELD_VECTOR, isr_yield, 0x8E);

    // interrupt gate (not a trap gate) so int 0x80 comes in with IF clear, same as SYSCALL does
    idt_set_descriptor(0x80, irq80, 0xEE);  // syscalls

    asm volatile ("lidt %0" : : "m"(idt_ptr));
    return idt_ptr.address;
}
