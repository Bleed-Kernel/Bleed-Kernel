#include <gdt/gdt.h>
#include <ansii.h>
#include <stdint.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <cpu/percpu.h>

extern void gdt_load(void *);


/// @brief set GDT entries by index
/// @param gdt the gdt to write into
/// @param idx inxed
/// @param base base address
/// @param limit limit
/// @param access granularity
/// @param flags flags
static void set_gdt_gate(gdt_t *gdt, uint8_t idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags){
    gdt->entries[idx].base_low     = (base >> 0);
    gdt->entries[idx].base_middle  = (base >> 16);
    gdt->entries[idx].base_high    = (base >> 24);

    gdt->entries[idx].limit_16     = (limit >> 0);     // 16 bits of the first limit
    gdt->entries[idx].limit_8      = (limit >> 16);    // last bit of the limit

    gdt->entries[idx].access       = access;
    gdt->entries[idx].flags        = flags;
}

/// @brief initalise the better gdt, replacing the one from LIMINE
void *gdt_init(cpu_t *cpu){
    gdt_t *gdt = &cpu->gdt;
    set_gdt_gate(gdt, 0, 0, 0, 0, 0);
    set_gdt_gate(gdt, 1, 0, 0xFFFFF, 0x9A, 0xA);
    set_gdt_gate(gdt, 2, 0, 0xFFFFF, 0x92, 0x0);
    set_gdt_gate(gdt, 3, 0, 0xFFFFF, 0xFA, 0xA);
    set_gdt_gate(gdt, 4, 0, 0xFFFFF, 0xF2, 0x0);

    // lgdt copies this, it can live on the stack
    struct gdt_ptr_t gdt_ptr;
    gdt_ptr.address = gdt;
    gdt_ptr.length  = sizeof(gdt_t) - 1;

    gdt_load(&gdt_ptr);
    return gdt_ptr.address;
}