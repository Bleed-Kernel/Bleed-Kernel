#include <stdint.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <sched/scheduler.h>

volatile uint64_t timer_ticks = 0;

cpu_context_t *timer_handle(cpu_context_t *ctx) {
    cpu_context_t *next = sched_tick(ctx);
    timer_ticks++;
    return next;
}

void irq_handler(uint8_t irq) {
    switch (irq) {
        case 0:
            break;
        case 1:
            PS2_Keyboard_Interrupt(irq);
            break;
        default:
            break;
    }
}
