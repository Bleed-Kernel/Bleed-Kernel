#include <cpu/io.h>
#include <stdio.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <drivers/pit/pit.h>
#include <drivers/pic/pic.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <sched/scheduler.h>

volatile uint64_t timer_ticks;
volatile uint64_t pit_countdown = 0;

void pic_unmask(uint8_t interrupt) {
    if (interrupt < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << interrupt));
    }
    else {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (interrupt - 8)));
    }
}

/// @brief remaps the pic
/// @param master_offset remap offset for the master pic
/// @param slave_offset remap offset for the slave pic
void pic_init(int master_offset, int slave_offset){

    outb(PIC1_CMD, ICW1_INIT);
    outb(PIC2_CMD, ICW1_INIT);
    
    // Set vector offsets
    outb(PIC1_DATA, master_offset);
    outb(PIC2_DATA, slave_offset);

    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, 0xFF);    // kbd and timer mask
    outb(PIC2_DATA, 0xFF);    // disable all slave IRQs
}

cpu_context_t *timer_handle(cpu_context_t *r){
    cpu_context_t *rctx = sched_tick(r);
    timer_ticks++;
    if (pit_countdown > 0)
        pit_countdown--;
    return rctx;
}

int irq_handler_spurrious(uint8_t irq){
    if (irq == 7) {
        uint8_t isr = pic_read_isr(PIC1_CMD);
        if (!(isr & (1 << 7)))
            return 1;
    }

    if (irq == 15) {
        uint8_t isr = pic_read_isr(PIC2_CMD);
        if (!(isr & (1 << 7))) {
            PIC_EOI(irq);
            return 1;
        }
    }

    return 0;
}

void irq_handler(uint8_t irq) {
    if(irq_handler_spurrious(irq) == 1) return;

    switch (irq) {
        case 0:
            break;
        case 1:
            PS2_Keyboard_Interrupt(irq);
            PIC_EOI(irq);
            break;
        default:
            break;
    }
}