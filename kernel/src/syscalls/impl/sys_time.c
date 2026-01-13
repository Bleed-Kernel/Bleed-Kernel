#include <ACPI/acpi_time.h>
#include <ACPI/acpi.h>
#include <user/user_copy.h>
#include <mm/kalloc.h>

int sys_time(struct rtc_time *user_buf) {
    if (!user_buf)
        return -1;

    struct rtc_time time;
    rtc_get_time(&time);

    if (from_kernel_copy_user(user_buf, &time, sizeof(time)) < 0)
        return -2;

    return 0;
}