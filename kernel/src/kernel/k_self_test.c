#include <devices/type/tty_device.h>
#include <drivers/serial/serial.h>
#include <devices/devices.h>
#include <console/console.h>
#include <stdio.h>
#include <ansii.h>
#include "self-test/ktests.h"

void kernel_self_test(){
    /*
    tty_t tty = console_get_active_tty();

    tty_fb_backend_t *b = tty.backend;
    fb_console_t *fb = &b->fb;

    uint64_t cols = fb->width  / fb->font->width;
    uint64_t rows = fb->height / fb->font->height;

    const char *starting = "Starting Bleed";
    const char *kernel_tests = "Running Kernel Tests";

    size_t len1 = strlen(starting);

    uint64_t x1 = (cols > len1) ? (cols - len1) / 2 : 0;

    uint64_t y = (rows / 2) - 1;
    kprintf(CYAN_FG);
    kprintf_at(x1, y,     "   %s", starting);
    kprintf(RESET);
    kprintf_at(x1, y + 1,     "%s", kernel_tests);
    */

    paging_test_self_test();
    pmm_test_self_test();
    pit_test_self_test();
    vfs_test_self_test();
    vmm_test_self_test();
    //kprintf("\x1b[J");
}