#include <devices/devices.h>
#include <ACPI/acpi.h>
#include <fs/vfs.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <devices/type/power_device.h>
#include <drivers/serial/serial.h>
#include <string.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <kernel/exception/panic.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/signal.h>
#include <ACPI/acpi_hpet.h>
#include <fs/vfs_mount.h>
#include <mm/spinlock.h>
#include <stdio.h>

typedef struct {
    task_t *self;
    int     sig;    // 0 just counts them
    int     alive;
} power_sweep_t;

// every user task but the one that asked for the power to go
static void power_sweep_task(task_t *task, void *userdata) {
    power_sweep_t *sweep = userdata;
    if (task == sweep->self || task->task_privilege != PRIVILEGE_USER) return;
    if (task->state == TASK_ZOMBIE || task->state == TASK_DEAD || task->state == TASK_FREE) return;

    sweep->alive++;
    if (sweep->sig)
        signal_send(task, sweep->sig);
}

// send sig to everything and wait for it all to be gone. returns how many were still there at the end
static int power_signal_all(int sig, uint64_t wait) {
    power_sweep_t sweep = { .self = get_current_task(), .sig = sig };
    itterate_each_task(power_sweep_task, &sweep);

    uint64_t deadline = sched_deadline_ms(wait);
    sweep.sig = 0;
    do {
        wait_ms(POWER_POLL_MS);
        sweep.alive = 0;
        itterate_each_task(power_sweep_task, &sweep);
    } while (sweep.alive && !sched_deadline_passed(deadline));

    return sweep.alive;
}

static void power_stop_tasks(void) {
    int left = power_signal_all(SIGTERM, POWER_TERM_WAIT_MS);
    if (!left) {
        serial_printf(LOG_OK "power: every task exited when asked\n");
        return;
    }

    serial_printf(LOG_WARN "power: %d task(s) ignored SIGTERM, killing them\n", left);
    left = power_signal_all(SIGKILL, POWER_KILL_WAIT_MS);
    if (left)
        serial_printf(LOG_WARN "power: %d task(s) outlived SIGKILL, going down anyway\n", left);
}

void power_prepare(void) {
    power_stop_tasks();

    // whoever asked is the one task left, it could be sat in a directory on one of them
    vfs_chdir_root(get_current_task());

    int busy = vfs_umount_all();
    if (busy)
        serial_printf(LOG_WARN "power: %d filesystem(s) still busy, going down anyway\n", busy);
}

// the power button does what the start menu does. its looked at a few times a second from a
// task of its own, the shutdown waits on other tasks and cant be done from an interrupt
void power_button_watch(void) {
    acpi_power_button_enable();

    for (;;) {
        uint64_t deadline = sched_deadline_ms(POWER_BUTTON_POLL_MS);

        // a signal can still end it early, only the deadline is worth reading the port for
        while (!sched_deadline_passed(deadline)) {
            unsigned long flags = irq_push();
            sched_sleep_until(deadline);
            irq_restore(flags);
        }

        if (!acpi_power_button_pressed())
            continue;

        serial_printf(LOG_OK "Power button pressed, shutting down\n");
        power_prepare();
        acpi_shutdown();
    }
}

static power_battery_fn_t battery_source = NULL;

void power_set_battery_source(power_battery_fn_t fn) {
    battery_source = fn;
}

static void power_battery_read(power_battery_t *out) {
    memset(out, 0, sizeof(*out));
    if (battery_source && !battery_source(out))
        memset(out, 0, sizeof(*out));
}

// the same thing as text, so it can be looked at with cat
static size_t power_text(char *text, size_t max) {
    power_battery_t batt;
    power_battery_read(&batt);

    if (!batt.present)
        return (size_t)snprintf(text, max, "battery: none\n");
    return (size_t)snprintf(text, max, "battery: %u%% %s (%u/%u mAh, %u mV)\n", batt.percent,
                            batt.charging ? "charging" : batt.ac ? "full" : "discharging",
                            batt.capacity, batt.full, batt.voltage);
}

static long power_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode;
    if (!buf || len == 0) return 0;

    char   text[POWER_TEXT_MAX];
    size_t size = power_text(text, sizeof(text));

    if (offset >= size) return 0;
    if (len > size - offset) len = size - offset;

    memcpy(buf, text + offset, len);
    return (long)len;
}

// without a size a read never comes to an end
static size_t power_size(INode_t *inode) {
    (void)inode;
    char text[POWER_TEXT_MAX];
    return power_text(text, sizeof(text));
}

static int power_ioctl(INode_t *inode, unsigned long request, void *arg) {
    (void)inode;

    if (request == POWER_DEVICE_BATTERY) {
        if (!arg)
            return -EFAULT;

        power_battery_t batt;
        power_battery_read(&batt);

        task_t *task = get_current_task();
        if (!task || copy_to_user(task, arg, &batt, sizeof(batt)) != 0)
            return -EFAULT;

        return 0;
    }

    if (request == POWER_DEVICE_POWEROFF) {
        serial_printf(LOG_OK "Power Device recieved Poweroff instruction\n");
        power_prepare();
        acpi_shutdown();
        serial_printf(LOG_ERROR "acpi_shutdown() returned unexpectedly\n");
        return -EIO;
    }else if (request == POWER_DEVICE_REBOOT){
        serial_printf(LOG_OK "Power Device recieved Reboot instruction\n");
        power_prepare();
        acpi_reboot();
        serial_printf(LOG_ERROR "acpi_reboot() returned unexpectedly\n");
        return -EIO;
    }else if (request == POWER_DEVICE_KERNEL_PANIC){
        serial_printf(LOG_OK "Power Device recieved Kernel Panic instruction");
        ke_panic(NULL, "Manually Initiated Crash");
        serial_printf(LOG_ERROR "ke_panic() returned? how did it manage that?\n");
        return -EIO;
    }

    return -EINVAL;
}

static INodeOps_t power_inode_ops = {
    .read = power_read,
    .size = power_size,
    .ioctl = power_ioctl,
};

void power_device_init(void) {
    INode_t *dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode)
        return;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops = &power_inode_ops;
    dev_inode->type = INODE_DEVICE;
    dev_inode->shared = 1;

    device_register(dev_inode, "power");
}