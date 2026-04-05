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
#include <console/console.h>
#include <mm/smap.h>
#include <sched/signal.h>
#include <sched/scheduler.h>

static kbd_device_t *keyboard_device = NULL;
extern struct INodeOps tty_inode_ops;

static int kbd_fd_tty_index(file_t *f, uint32_t *index_out) {
    if (!f || !index_out)
        return -1;
    if (!f->inode || f->inode->ops != &tty_inode_ops || !f->inode->internal_data)
        return -1;

    tty_t *tty = (tty_t *)f->inode->internal_data;
    *index_out = tty->index;
    return 0;
}

static int kbd_task_tty_index(task_t *task, uint32_t *index_out) {
    if (!task || !task->fd_table || !index_out)
        return -1;

    // Prefer stdout tty, then stderr tty.
    if (kbd_fd_tty_index(task->fd_table->fds[1], index_out) == 0)
        return 0;
    if (kbd_fd_tty_index(task->fd_table->fds[2], index_out) == 0)
        return 0;

    return -1;
}

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
        INode_t *active_console = console_get_active_console();
        tty_t *active_tty = active_console ? (tty_t *)active_console->internal_data : NULL;
        if (!active_tty) {
            spinlock_release(&kbd->lock);
            irq_restore(irq);
            return -EAGAIN;
        }

        uint32_t current_tty_index = 0;
        int has_task_tty = (kbd_task_tty_index(current, &current_tty_index) == 0);
        if (has_task_tty && current_tty_index != active_tty->index) {
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
    if (!kbd)
        return -1;

    SMAP_ALLOW{
        switch (request) {
            case TTY_IOCTL_SET_FLAGS:
                if (!arg) return -1;
                kbd->flags = *(uint32_t *)arg;
                return 0;
            case TTY_IOCTL_GET_FLAGS:
                if (!arg) return -1;
                *(uint32_t *)arg = kbd->flags;
                return 0;
            case TTY_IOCTL_TCGETS: {
                if (!arg) return -1;
                tty_termios_t term = {0};
                if (kbd->flags & TTY_ECHO) term.c_lflag |= TTY_TERM_ECHO;
                if (kbd->flags & TTY_CANNONICAL) term.c_lflag |= TTY_TERM_ICANON;
                term.c_lflag |= TTY_TERM_ISIG;
                term.c_cc[TTY_VINTR] = 3;
                term.c_cc[TTY_VERASE] = 127;
                term.c_cc[TTY_VMIN] = (kbd->flags & TTY_NONBLOCK) ? 0 : 1;
                term.c_cc[TTY_VTIME] = 0;
                *(tty_termios_t *)arg = term;
                return 0;
            }
            case TTY_IOCTL_TCSETS:
            case TTY_IOCTL_TCSETSW:
            case TTY_IOCTL_TCSETSF: {
                if (!arg) return -1;
                tty_termios_t *term = (tty_termios_t *)arg;
                if (term->c_lflag & TTY_TERM_ECHO) kbd->flags |= TTY_ECHO;
                else kbd->flags &= ~TTY_ECHO;
                if (term->c_lflag & TTY_TERM_ICANON) kbd->flags |= TTY_CANNONICAL;
                else kbd->flags &= ~TTY_CANNONICAL;
                if (term->c_cc[TTY_VMIN] == 0 && term->c_cc[TTY_VTIME] == 0)
                    kbd->flags |= TTY_NONBLOCK;
                else
                    kbd->flags &= ~TTY_NONBLOCK;
                return 0;
            }
            case TTY_IOCTL_FIONBIO:
                if (!arg) return -1;
                if (*(int *)arg)
                    kbd->flags |= TTY_NONBLOCK;
                else
                    kbd->flags &= ~TTY_NONBLOCK;
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

    fd_table_t *boot_fds = vfs_get_kernel_table();
    if (boot_fds) {
        boot_fds->fds[0] = kbfd;
    } else {
        kfree(kbfd, sizeof(*kbfd));
    }
    device_register(&keyboard_device->device, "keyboard");
    keyboard_register_listener(kbd_listener);
}
