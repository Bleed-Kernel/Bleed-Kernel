#include <stdint.h>
#include <cpu/io.h>
#include <drivers/pit/pit.h>
#include <stdio.h>
#include <panic.h>
#include <ansii.h>

void pit_init(uint32_t frequency) {
    if (frequency == 0 || frequency > PIT_FREQUENCY) {
        ke_panic("Invalid PIT frequency");
    }

    uint32_t divisor = PIT_FREQUENCY / frequency;
    outb(0x43, 0x36);

    outb(0x40, divisor & 0xFF);       // Low byte of the divisor
    outb(0x40, (divisor >> 8) & 0xFF); // High byte of the divisor
}

/// @brief Current countdown value of PIT-0
/// @return PIT fires until countdown is 0
uint32_t pit_read_interval_remaining(){
    asm volatile("cli");
    unsigned pit_count = 0;

	outb(0x43, 0x00);
	
	pit_count = inb(0x40);		    // Low byte
	pit_count |= inb(0x40)<<8;		// High byte
	
	return pit_count;
}

/// @brief Sets the reload divisor for PIT-0
/// @param count PIT Clock Ticks per Period
void pit_set_event_counter(uint32_t count) {
    asm volatile("cli");
	
	outb(0x40,count&0xFF);		    // Low byte
	outb(0x40,(count&0xFF00)>>8);	// High byte
	return;
}

void pit_wait_ticks(uint32_t ticks) {
    uint32_t remaining = ticks;

    while (remaining > 0) {
        uint16_t current = pit_read_interval_remaining();
        uint16_t next;
        do {
            next = pit_read_interval_remaining();
        } while (next == current);

        remaining--;
    }
}