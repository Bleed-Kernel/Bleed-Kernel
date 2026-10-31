#include <stdint.h>
#include <stdbool.h>
#include <cpu/irq.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <drivers/ps2/PS2_mouse.h>
#include <drivers/serial/serial.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <sched/bkl.h>
#include <cpu/percpu.h>
#include <user/errno.h>
#include <ansii.h>

volatile uint64_t timer_ticks = 0;

typedef struct {
    bool     used;
    uint8_t  irq;
    irq_fn_t fn;
    void    *ctx;
} irq_slot_t;

typedef struct {
    bool      used;
    tick_fn_t fn;
    void     *ctx;
} tick_slot_t;

static irq_slot_t  irq_slots[IRQ_HANDLERS];
static tick_slot_t tick_slots[TICK_HANDLERS];

int irq_register(uint8_t irq, irq_fn_t fn, void *ctx) {
    if (irq >= IRQ_LINES || !fn)
        return -EINVAL;

    unsigned long flags = irq_push();
    for (int i = 0; i < IRQ_HANDLERS; i++) {
        if (irq_slots[i].used)
            continue;
        irq_slots[i] = (irq_slot_t){ .used = true, .irq = irq, .fn = fn, .ctx = ctx };
        irq_restore(flags);
        return 0;
    }
    irq_restore(flags);
    serial_printf(LOG_ERROR "irq: no room for another handler on irq %u\n", irq);
    return -ENOMEM;
}

void irq_unregister(uint8_t irq, irq_fn_t fn, void *ctx) {
    unsigned long flags = irq_push();
    for (int i = 0; i < IRQ_HANDLERS; i++) {
        irq_slot_t *s = &irq_slots[i];
        if (s->used && s->irq == irq && s->fn == fn && s->ctx == ctx)
            s->used = false;
    }
    irq_restore(flags);
}

int tick_register(tick_fn_t fn, void *ctx) {
    if (!fn)
        return -EINVAL;

    unsigned long flags = irq_push();
    for (int i = 0; i < TICK_HANDLERS; i++) {
        if (tick_slots[i].used)
            continue;
        tick_slots[i] = (tick_slot_t){ .used = true, .fn = fn, .ctx = ctx };
        irq_restore(flags);
        return 0;
    }
    irq_restore(flags);
    serial_printf(LOG_ERROR "irq: no room for another tick handler\n");
    return -ENOMEM;
}

void tick_unregister(tick_fn_t fn, void *ctx) {
    unsigned long flags = irq_push();
    for (int i = 0; i < TICK_HANDLERS; i++) {
        if (tick_slots[i].used && tick_slots[i].fn == fn && tick_slots[i].ctx == ctx)
            tick_slots[i].used = false;
    }
    irq_restore(flags);
}

size_t irq_drop_range(uintptr_t start, uintptr_t end) {
    size_t dropped = 0;

    unsigned long flags = irq_push();
    for (int i = 0; i < IRQ_HANDLERS; i++) {
        uintptr_t fn = (uintptr_t)irq_slots[i].fn;
        if (irq_slots[i].used && fn >= start && fn < end) {
            irq_slots[i].used = false;
            dropped++;
        }
    }
    for (int i = 0; i < TICK_HANDLERS; i++) {
        uintptr_t fn = (uintptr_t)tick_slots[i].fn;
        if (tick_slots[i].used && fn >= start && fn < end) {
            tick_slots[i].used = false;
            dropped++;
        }
    }
    irq_restore(flags);
    return dropped;
}

cpu_context_t *timer_handle(cpu_context_t *ctx) {
    int bsp = this_cpu()->id == 0;

    if (bsp) {
        bkl_lock();

        // hardware we poll instead of taking its interrupt
        for (int i = 0; i < TICK_HANDLERS; i++) {
            if (tick_slots[i].used)
                tick_slots[i].fn(tick_slots[i].ctx);
        }

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

    // the built in ones
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
        default:
            break;
    }

    for (int i = 0; i < IRQ_HANDLERS; i++) {
        if (irq_slots[i].used && irq_slots[i].irq == irq)
            irq_slots[i].fn(irq, irq_slots[i].ctx);
    }
}
