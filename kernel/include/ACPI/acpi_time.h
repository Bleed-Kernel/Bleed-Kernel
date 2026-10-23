#pragma once

#include <stdint.h>
#include <cpu/io.h>
#include <stdbool.h>

typedef struct rtc_time {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t mon;
    uint16_t year;
} time_t;

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

// seconds since 1970, utc as far as the rtc knows
int64_t clock_now(void);
int64_t clock_from_civil(int year, int mon, int day, int hour, int min, int sec);
// the fat family date/time pair, 0 for a date thats never been set
int64_t clock_from_dos(uint16_t date, uint16_t time);
// and back, date in the high half the way exfat keeps it
uint32_t clock_to_dos(int64_t t);
