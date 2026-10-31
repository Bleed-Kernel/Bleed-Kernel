#pragma once

#include <stdint.h>
#include <stddef.h>

#define IRQ_LINES       16      // legacy lines on vectors 32-47, the ioapic routes gsis onto them
#define IRQ_HANDLERS    32      // across every line, they can share
#define TICK_HANDLERS   32

typedef void (*irq_fn_t)(uint8_t irq, void *ctx);
typedef void (*tick_fn_t)(void *ctx);

// runs on the bsp with the bkl held whenever the line fires, 0 on success
int  irq_register(uint8_t irq, irq_fn_t fn, void *ctx);
void irq_unregister(uint8_t irq, irq_fn_t fn, void *ctx);

// runs off every timer tick on the bsp with the bkl held, for hardware with no interrupt we use
int  tick_register(tick_fn_t fn, void *ctx);
void tick_unregister(tick_fn_t fn, void *ctx);

// throw out every handler whose code sits in [start, end), how many went
size_t irq_drop_range(uintptr_t start, uintptr_t end);
