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
#include <user/user_copy.h>
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
    if (!kbd) return -ENODEV;
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
    // no signal check in here, a fatal one never returns and would take the lock with it
    while (bytes_read + sizeof(keyboard_event_t) <= len && kbd->tail != kbd->head) {
        keyboard_event_t *event = &kbd->buffer[kbd->tail];
        memcpy((uint8_t*)buf + bytes_read, event, sizeof(keyboard_event_t));
        
        kbd->tail = (kbd->tail + 1) % KBD_BUFFER_SIZE;
        bytes_read += sizeof(keyboard_event_t);
    }

    spinlock_release(&kbd->lock);
    irq_restore(irq);
    return bytes_read;
}

// arg is a raw user pointer, everything in and out of an ioctl goes through these two
static int kbd_arg_in(void *arg, void *dst, size_t len) {
    if (!arg) return -EINVAL;
    return copy_from_user(get_current_task(), dst, arg, len) == 0 ? 0 : -EFAULT;
}

static int kbd_arg_out(void *arg, const void *src, size_t len) {
    if (!arg) return -EINVAL;
    return copy_to_user(get_current_task(), arg, src, len) == 0 ? 0 : -EFAULT;
}

static int kbd_ioctl(INode_t *inode, unsigned long request, void *arg) {
    kbd_device_t *kbd = inode->internal_data;
    if (!kbd)
        return -ENODEV;

    int r;

    switch (request) {
        case TTY_IOCTL_SET_FLAGS: {
            uint32_t flags;
            if ((r = kbd_arg_in(arg, &flags, sizeof(flags))) < 0) return r;
            kbd->flags = flags;
            return 0;
        }
        case TTY_IOCTL_GET_FLAGS:
            return kbd_arg_out(arg, &kbd->flags, sizeof(kbd->flags));
        case TTY_IOCTL_TCGETS: {
            tty_termios_t term = {0};
            if (kbd->flags & TTY_ECHO) term.c_lflag |= TTY_TERM_ECHO;
            if (kbd->flags & TTY_CANNONICAL) term.c_lflag |= TTY_TERM_ICANON;
            term.c_lflag |= TTY_TERM_ISIG;
            term.c_cc[TTY_VINTR] = 3;
            term.c_cc[TTY_VERASE] = 127;
            term.c_cc[TTY_VMIN] = (kbd->flags & TTY_NONBLOCK) ? 0 : 1;
            term.c_cc[TTY_VTIME] = 0;
            return kbd_arg_out(arg, &term, sizeof(term));
        }
        case TTY_IOCTL_TCSETS:
        case TTY_IOCTL_TCSETSW:
        case TTY_IOCTL_TCSETSF: {
            tty_termios_t term;
            if ((r = kbd_arg_in(arg, &term, sizeof(term))) < 0) return r;

            if (term.c_lflag & TTY_TERM_ECHO) kbd->flags |= TTY_ECHO;
            else kbd->flags &= ~TTY_ECHO;
            if (term.c_lflag & TTY_TERM_ICANON) kbd->flags |= TTY_CANNONICAL;
            else kbd->flags &= ~TTY_CANNONICAL;
            if (term.c_cc[TTY_VMIN] == 0 && term.c_cc[TTY_VTIME] == 0)
                kbd->flags |= TTY_NONBLOCK;
            else
                kbd->flags &= ~TTY_NONBLOCK;
            return 0;
        }
        case TTY_IOCTL_FIONBIO: {
            int nonblock;
            if ((r = kbd_arg_in(arg, &nonblock, sizeof(nonblock))) < 0) return r;

            if (nonblock)
                kbd->flags |= TTY_NONBLOCK;
            else
                kbd->flags &= ~TTY_NONBLOCK;
            return 0;
        }
        default:
            return -ENOTTY;
    }
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
    if (!keyboard_device) return;
    memset(keyboard_device, 0, sizeof(kbd_device_t));
    spinlock_init(&keyboard_device->lock);

    keyboard_device->device.ops = &kbd_inode_ops;
    keyboard_device->device.internal_data = keyboard_device;
    keyboard_device->device.type = INODE_DEVICE;

    // stdin of the boot fd table, every task clones its table from that one
    fd_table_t *boot_fds = vfs_get_kernel_table();
    file_t *kbfd = boot_fds ? kmalloc(sizeof(file_t)) : NULL;
    if (kbfd) {
        memset(kbfd, 0, sizeof(*kbfd));
        kbfd->type = FD_TYPE_DEV;
        kbfd->inode = &keyboard_device->device;
        kbfd->inode->shared++;
        kbfd->flags = O_RDWR;
        kbfd->shared = 1;
        boot_fds->fds[0] = kbfd;
    }
    device_register(&keyboard_device->device, "keyboard");
    keyboard_register_listener(kbd_listener);
}
