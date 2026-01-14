#include <stdint.h>
#include <stdio.h>
#include <devices/type/tty_device.h>
#include <ACPI/acpi_time.h>
#include <ACPI/acpi.h>
#include <ansii.h>
#include "kmain.h"

static const char *weekday_name(int w) {
    static const char *names[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    return names[w % 7];
}

static int calc_weekday(int y, int m, int d) {
    if (m < 3) {
        m += 12;
        y--;
    }
    int K = y % 100;
    int J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K/4 + J/4 + 5*J) % 7;
    return (h + 6) % 7;
}

void prototype_clock() {
    const uint64_t y = 0;

    tty_fb_backend_t *b = (tty_fb_backend_t *)tty0.backend;
    if (!b || !b->fb.font) return;

    uint64_t font_width = b->fb.font->width;
    uint64_t fb_width_pixels = framebuffer_get_width(0);
    uint64_t cols = fb_width_pixels / font_width;

    struct rtc_time t;
    rtc_get_time(&t);

    int wday = calc_weekday(t.year, t.mon, t.day);
    const char *wname = weekday_name(wday);

    char buf[64];
    char *p = buf;

    const char *red = RGB_FG(212, 44, 44);
    for (const char *s = red; *s; s++) *p++ = *s;

    for (const char *s = wname; *s; s++) *p++ = *s;

    const char *rst = RESET;
    for (const char *s = rst; *s; s++) *p++ = *s;

    *p++ = ' ';

    *p++ = '0' + (t.day / 10);
    *p++ = '0' + (t.day % 10);
    *p++ = '/';
    *p++ = '0' + (t.mon / 10);
    *p++ = '0' + (t.mon % 10);
    *p++ = '/';

    *p++ = '0' + ((t.year / 1000) % 10);
    *p++ = '0' + ((t.year / 100) % 10);
    *p++ = '0' + ((t.year / 10) % 10);
    *p++ = '0' + (t.year % 10);
    *p++ = ' ';

    *p++ = '0' + (t.hour / 10);
    *p++ = '0' + (t.hour % 10);
    *p++ = ':';
    *p++ = '0' + (t.min / 10);
    *p++ = '0' + (t.min % 10);
    *p++ = ':';
    *p++ = '0' + (t.sec / 10);
    *p++ = '0' + (t.sec % 10);
    *p = '\0';

    size_t text_len = strlen(wname) + 1 + 10 + 1 + 8;
    uint64_t x = (cols > text_len) ? (cols - text_len) / 2 : 0;

    kprintf_at(0, y, "                                        ");
    kprintf_at(x, y, "%s", buf);
}
