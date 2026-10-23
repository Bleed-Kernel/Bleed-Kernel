#include <ACPI/acpi_time.h>
#include <ACPI/acpi.h>
#include <ACPI/acpi_hpet.h>

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

// days since 1970-01-01, howard hinnants days_from_civil
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t clock_from_civil(int year, int mon, int day, int hour, int min, int sec) {
    return days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

int64_t clock_from_dos(uint16_t date, uint16_t time) {
    if (!date)
        return 0;
    return clock_from_civil(1980 + (date >> 9), (date >> 5) & 0xF, date & 0x1F,
                            time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2);
}

uint32_t clock_to_dos(int64_t t) {
    int64_t days = t / 86400, rem = t % 86400;

    // civil_from_days
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp  = (5 * doy + 2) / 153;
    int64_t d   = doy - (153 * mp + 2) / 5 + 1;
    int64_t m   = mp < 10 ? mp + 3 : mp - 9;
    int64_t y   = yoe + era * 400 + (m <= 2);

    if (y < 1980)
        return 0;
    uint16_t date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    uint16_t time = (uint16_t)(((rem / 3600) << 11) | ((rem / 60 % 60) << 5) | (rem % 60 / 2));
    return ((uint32_t)date << 16) | time;
}

// the rtc only ticks whole seconds and is slow to read
int64_t clock_now(void) {
    static int64_t  base;
    static uint64_t base_fs;
    uint64_t fs = hpet_get_femtoseconds();

    if (!base_fs) {
        struct rtc_time t;
        rtc_get_time(&t);
        base    = clock_from_civil(t.year, t.mon, t.day, t.hour, t.min, t.sec);
        base_fs = fs;
        return base;
    }

    return base + (int64_t)((fs - base_fs) / 1000000000000000ULL);
}
