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