#include <drivers/pit/pit.h>
#include <cpu/io.h>

// only channel 2 is ever touched. channel 0 is the one on irq 0, nothing here programs it or
// unmasks anything, and the lapic timer is calibrated off the hpet so it never sees any of this
// we litterally only use it for the splash, I dont even like the fact it's here
bool pit_wait_ms(uint32_t ms) {
    uint8_t control = inb(PIT_CH2_CONTROL);
    bool    counted = true;

    // counting, but not out of the speaker
    outb(PIT_CH2_CONTROL, (control & ~PIT_CH2_SPEAKER) | PIT_CH2_GATE);

    for (uint32_t i = 0; i < ms && counted; i++) {
        outb(PIT_COMMAND, PIT_CMD_CH2_ONESHOT);
        outb(PIT_CH2_DATA, PIT_TICKS_PER_MS & 0xFF);
        outb(PIT_CH2_DATA, PIT_TICKS_PER_MS >> 8);

        uint32_t spins = 0;
        while (!(inb(PIT_CH2_CONTROL) & PIT_CH2_OUT) && spins < PIT_SPIN_MAX)
            spins++;
        if (spins == PIT_SPIN_MAX)
            counted = false;
    }

    // the gate and the speaker go back to how they were found
    outb(PIT_CH2_CONTROL, control & (PIT_CH2_GATE | PIT_CH2_SPEAKER));
    return counted;
}
