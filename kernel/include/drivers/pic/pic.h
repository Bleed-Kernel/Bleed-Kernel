#pragma once
#include <cpu/io.h>

#define PIC1        0x20    // Master PIC
#define PIC2        0xA0    // Slave PIC
#define PIC1_CMD    PIC1
#define PIC1_DATA   (PIC1 + 1)
#define PIC2_CMD    PIC2
#define PIC2_DATA   (PIC2 + 1)

#define ICW1_INIT   0x11
#define ICW4_8086   0x01

#define PIC_EOI(irq)                    \
    do {                                \
        if ((irq) >= 8)                 \
            outb(PIC2_CMD, 0x20);       \
        outb(PIC1_CMD, 0x20);           \
    } while (0)

extern volatile uint64_t pit_countdown;

void pic_unmask(uint8_t interrupt);

/// @brief remaps the pic
/// @param master_offset remap offset for the master pic
/// @param slave_offset remap offset for the slave pic
void pic_init(int master_offset, int slave_offset);

/// @brief read the PIC ISR
/// @param cmd_port port
/// @return val
inline uint8_t pic_read_isr(uint16_t cmd_port) {
    outb(cmd_port, 0x0B);
    return inb(cmd_port);
}