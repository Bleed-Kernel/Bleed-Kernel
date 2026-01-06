#include <devices/type/tty_device.h>
#include <sched/scheduler.h>
#include <mm/spinlock.h>

long tty_read(device_t *dev, void *buf, size_t len) {
    tty_t *tty = dev->priv;

    const int mode_canonical = tty->flags & TTY_CANNONICAL;
    //const int mode_echo = tty->flags & TTY_ECHO;  might need it one day

    if (tty->in_head == tty->in_tail)
        return 0;

    if (mode_canonical) {
        size_t i = tty->in_tail;
        int found = 0;

        while (i != tty->in_head) {
            if (tty->inbuffer[i] == '\n') {
                found = 1;
                break;
            }
            i = (i + 1) % TTY_BUFFER_SZ;
        }

        if (!found)
            return 0;
    }

    size_t count = 0;
    while (count < len && tty->in_tail != tty->in_head) {
        char c = tty->inbuffer[tty->in_tail++ % TTY_BUFFER_SZ];
        ((char *)buf)[count++] = c;
        if ((mode_canonical) && c == '\n')
            break;
    }

    return count;
}

int tty_write(device_t *dev, const void *buf, size_t len) {
    tty_t *tty = dev->priv;
    const char *c = buf;

    void (*putc)(tty_t*, char) = tty->ops->putchar;
    for (size_t i = 0; i < len; i++){
        putc(tty, c[i]);
    }

    return len;
}

int tty_ioctl(device_t *dev, unsigned long req, void *arg){
    tty_t *tty = dev->priv;

    switch (req) {
    case 0:
        *(uint32_t *)arg = tty->flags;
        return 0;

    case 1:
        tty->flags = *(uint32_t *)arg;
        return 0;
    }

    return -1;
}
