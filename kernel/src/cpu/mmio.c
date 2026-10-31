#include <cpu/mmio.h>
#include <ACPI/acpi_hpet.h>

bool mmio_wait32(uint64_t base, uint32_t reg, uint32_t mask, bool want_set, uint64_t timeout_ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        if (!!(mmio_read32(base, reg) & mask) == want_set)
            return true;
        if (hpet_timed_out(start, spins, timeout_ms))
            return false;
        asm volatile("pause");
    }
}
