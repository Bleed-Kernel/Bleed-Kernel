#include <ACPI/acpi_time.h>
#include <ACPI/acpi.h>

#include "../acpi_priv.h"

static inline uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool rtc_updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

void rtc_get_time(struct rtc_time *t) {
    if (!t) return;

    uint8_t last_sec;
    do {
        while (rtc_updating());

        t->sec  = cmos_read(0x00);
        t->min  = cmos_read(0x02);
        t->hour = cmos_read(0x04);
        t->day  = cmos_read(0x07);
        t->mon  = cmos_read(0x08);
        t->year = cmos_read(0x09);

        last_sec = t->sec;
    } while (last_sec != t->sec);

    outb(CMOS_ADDR, 0x0B);
    uint8_t reg_b = inb(CMOS_DATA);

    if (!(reg_b & 0x04)) {
        t->sec  = bcd_to_bin(t->sec);
        t->min  = bcd_to_bin(t->min);
        t->hour = bcd_to_bin(t->hour);
        t->day  = bcd_to_bin(t->day);
        t->mon  = bcd_to_bin(t->mon);
        t->year = bcd_to_bin(t->year);
    }

    if (fadt && fadt->century) {
        uint8_t cent = cmos_read(fadt->century);
        if (!(reg_b & 0x04))
            cent = bcd_to_bin(cent);
        t->year += cent * 100;
    } else {
        if (t->year < 70) t->year += 2000;
        else t->year += 1900;
    }
}
