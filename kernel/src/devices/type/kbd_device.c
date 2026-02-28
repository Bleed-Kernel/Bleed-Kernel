#include <devices/devices.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <input/keyboard_dispatch.h>
#include <mm/spinlock.h>
#include <stdio.h>
#include <string.h>
#include <devices/type/kbd_device.h>
#include <mm/kalloc.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <user/errno.h>
#include <devices/type/tty_device.h>
#include <mm/smap.h>
#include <sched/signal.h>
#include <sched/scheduler.h>

static kbd_device_t *keyboard_device = NULL;

static long kbd_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)offset;
    kbd_device_t *kbd = inode->internal_data;
    if (!kbd) return -1;
    // ensure only the right task gets the int
    task_t *current = get_current_task();
    if (signal_should_interrupt(current))
        return -EINTR;

    unsigned long irq = irq_push();
    spinlock_acquire(&kbd->lock);

    if (current && current->task_privilege == PRIVILEGE_USER) {
        task_t *owner = NULL;
        if (kbd->owner_pid != 0)
            owner = sched_get_task(kbd->owner_pid);

        if (!owner ||
            owner->state == TASK_DEAD ||
            owner->state == TASK_FREE ||
            owner->state == TASK_ZOMBIE ||
            owner->state == TASK_BLOCKED) {
            kbd->owner_pid = current->id;
            kbd->owner_pgid = current->pgid ? current->pgid : current->id;
        } else if (kbd->owner_pid != current->id) {
            spinlock_release(&kbd->lock);
            irq_restore(irq);
            return -EAGAIN;
        }
    }

    if (kbd->head == kbd->tail) {
        spinlock_release(&kbd->lock);
        irq_restore(irq);
        if (kbd->flags & TTY_NONBLOCK)
            return -EAGAIN;
        return 0;
    }

    size_t bytes_read = 0;
    while (bytes_read + sizeof(keyboard_event_t) <= len && kbd->tail != kbd->head) {
        if (signal_should_interrupt(current)) {
            spinlock_release(&kbd->lock);
            irq_restore(irq);
            return bytes_read ? (long)bytes_read : -EINTR;
        }

        keyboard_event_t *event = &kbd->buffer[kbd->tail];
        memcpy((uint8_t*)buf + bytes_read, event, sizeof(keyboard_event_t));
        
        kbd->tail = (kbd->tail + 1) % KBD_BUFFER_SIZE;
        bytes_read += sizeof(keyboard_event_t);
    }

    spinlock_release(&kbd->lock);
    irq_restore(irq);
    return bytes_read;
}

static int kbd_ioctl(INode_t *inode, unsigned long request, void *arg) {
    kbd_device_t *kbd = inode->internal_data;
    if (!kbd || !arg)
        return -1;

    uint32_t *user_flags = (uint32_t *)arg;
    SMAP_ALLOW{
        switch (request) {
            case TTY_IOCTL_SET_FLAGS:
                kbd->flags = *user_flags;
                return 0;
            case TTY_IOCTL_GET_FLAGS:
                *user_flags = kbd->flags;
                return 0;
            default:
                return -1;
        }
    }
    return -1;
}

static struct INodeOps kbd_inode_ops = {
    .read = kbd_read,
    .ioctl = kbd_ioctl,
};

static void kbd_listener(const keyboard_event_t *ev) {
    if (!keyboard_device) return;

    unsigned long irq = irq_push();
    spinlock_acquire(&keyboard_device->lock);

    size_t head = keyboard_device->head;
    size_t next = (head + 1) % KBD_BUFFER_SIZE;

    if (next == keyboard_device->tail) {
        keyboard_device->tail = (keyboard_device->tail + 1) % KBD_BUFFER_SIZE;
    }

    keyboard_device->buffer[head] = *ev;
    keyboard_device->head = next;

    spinlock_release(&keyboard_device->lock);
    irq_restore(irq);
}

void kbd_device_init(void) {
    keyboard_device = kmalloc(sizeof(kbd_device_t));
    memset(keyboard_device, 0, sizeof(kbd_device_t));
    spinlock_init(&keyboard_device->lock);

    keyboard_device->device.ops = &kbd_inode_ops;
    keyboard_device->device.internal_data = keyboard_device;
    keyboard_device->device.type = INODE_FILE;

    file_t *kbfd = kmalloc(sizeof(file_t));
    kbfd->inode = &keyboard_device->device;
    kbfd->inode->ops = &kbd_inode_ops;
    kbfd->flags = O_RDWR;
    kbfd->offset = 0;
    kbfd->shared = 1;

    keyboard_device->device.type = INODE_DEVICE;

    current_fd_table->fds[0] = kbfd;
    device_register(&keyboard_device->device, "kbd0");
    keyboard_register_listener(kbd_listener);
}
