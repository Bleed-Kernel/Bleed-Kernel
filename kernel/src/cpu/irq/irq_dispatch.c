#include <stdint.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <drivers/ps2/PS2_mouse.h>
#include <drivers/usb/usb.h>
#include <drivers/ac97/ac97.h>
#include <drivers/e1000/e1000.h>
#include <drivers/i2c/i2c_trackpad.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <sched/bkl.h>
#include <cpu/percpu.h>

volatile uint64_t timer_ticks = 0;

cpu_context_t *timer_handle(cpu_context_t *ctx) {
    int bsp = this_cpu()->id == 0;

    if (bsp) {
        bkl_lock();

        // usb has no irq of its own here, it gets serviced off the tick instead
        usb_poll();
        ac97_poll();
        e1000_poll();

        // before the tick so a task whose sleep just ran out can be picked straight awa
        sched_event_tick();
    }

    cpu_context_t *next = sched_tick(ctx);
    if (bsp)
        timer_ticks++;
    return next;
}

// the ioapic only sends these to the bsp
void irq_handler(uint8_t irq) {
    bkl_lock();

    switch (irq) {
        case 1:
            PS2_Keyboard_Interrupt(irq);
            break;
        case 4:
            serial_interrupt();
            break;
        case 12:
            PS2_Mouse_Interrupt(irq);
            break;
        case I2C_TRACKPAD_IRQ:
            i2c_trackpad_interrupt();
            break;
        default:
            break;
    }
}
