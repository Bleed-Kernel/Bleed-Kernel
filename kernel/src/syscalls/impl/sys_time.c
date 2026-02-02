#include <ACPI/acpi_time.h>
#include <ACPI/acpi.h>
#include <user/user_copy.h>
#include <mm/kalloc.h>
#include <sched/scheduler.h>

int sys_time(struct rtc_time *user_buf) {
    if (!user_buf)
        return -1;

    struct rtc_time time;
    rtc_get_time(&time);

    task_t *caller = get_current_task();
    if (copy_to_user(caller, user_buf, &time, sizeof(time)) < 0)
        return -2;

    return 0;
}
