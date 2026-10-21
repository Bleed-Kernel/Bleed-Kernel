#include <devices/devices.h>
#include <devices/type/serial_tty_device.h>
#include <devices/type/tty_device.h>
#include <drivers/serial/serial.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <input/keyboard_input.h>
#include <exec/elf_load.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/signal.h>
#include <user/user_copy.h>
#include <user/user_poll.h>
#include <user/errno.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <string.h>
#include <stdio.h>

// set 1 scancodes, what a keyboard_event_t carries
#define SC_ESCAPE       0x01
#define SC_BACKSPACE    0x0E
#define SC_ENTER        0x1C
#define SC_HOME         0x47
#define SC_UP           0x48
#define SC_PAGE_UP      0x49
#define SC_LEFT         0x4B
#define SC_RIGHT        0x4D
#define SC_END          0x4F
#define SC_DOWN         0x50
#define SC_PAGE_DOWN    0x51
#define SC_INSERT       0x52
#define SC_DELETE       0x53

enum { IN_PLAIN, IN_ESC, IN_CSI, IN_SS3 };
enum { OUT_PLAIN, OUT_ESC, OUT_CSI };

typedef struct {
    INode_t           device;
    file_t           *file;         // what spawned tasks get as 0 1 and 2
    uint32_t          flags;

    spinlock_t        lock;
    keyboard_event_t  events[SERIAL_TTY_EVENTS];
    size_t            head, tail;
    wait_queue_t      wait;

    // keys coming in, only ever touched from the irq
    int               in_state;
    uint32_t          in_params[2];
    int               in_param;
    int               in_cr;

    // where the far end has its cursor, worked out from everything sent to it
    int               out_state;
    uint32_t          out_params[2];
    int               out_param;
    uint32_t          x, y;
    int               wrap_pending;
} serial_tty_t;

static serial_tty_t g_ttyS;
static bool         g_ttyS_ready = false;

static void serial_tty_push(uint16_t keycode, uint8_t keymod) {
    keyboard_event_t down = { .keycode = keycode, .action = KEY_DOWN,    .keymod = keymod };
    keyboard_event_t up   = { .keycode = keycode, .action = KEY_RELEASE, .keymod = keymod };

    unsigned long irq = irq_push();
    spinlock_acquire(&g_ttyS.lock);

    // full means the oldest goes, same as the keyboard
    for (int i = 0; i < 2; i++) {
        size_t next = (g_ttyS.head + 1) % SERIAL_TTY_EVENTS;
        if (next == g_ttyS.tail)
            g_ttyS.tail = (g_ttyS.tail + 1) % SERIAL_TTY_EVENTS;
        g_ttyS.events[g_ttyS.head] = i ? up : down;
        g_ttyS.head = next;
    }

    spinlock_release(&g_ttyS.lock);
    irq_restore(irq);

    sched_wake_queue(&g_ttyS.wait);
}

static bool serial_tty_is_ours(task_t *task) {
    if (!task || !task->fd_table) return false;
    file_t *f = task->fd_table->fds[1];
    return f && f->inode == &g_ttyS.device;
}

static void serial_tty_pick_newest(task_t *task, void *userdata) {
    task_t **picked = userdata;
    if (task->task_privilege != PRIVILEGE_USER) return;
    if (task->state == TASK_DEAD || task->state == TASK_FREE || task->state == TASK_ZOMBIE) return;
    if (!serial_tty_is_ours(task)) return;
    if (!*picked || task->id > (*picked)->id)
        *picked = task;
}

// whatever was started last on this terminal is the one in front
static void serial_tty_interrupt(void) {
    task_t *picked = NULL;
    itterate_each_task(serial_tty_pick_newest, &picked);
    if (picked)
        signal_send(picked, SIGINT);
}

static uint16_t serial_tty_scancode(char c, uint8_t *keymod) {
    for (uint16_t sc = 0; sc < 128; sc++) {
        if (keymap[sc] == c) {
            *keymod = 0;
            return sc;
        }
    }
    for (uint16_t sc = 0; sc < 128; sc++) {
        if (keymap_shift[sc] == c) {
            *keymod = KEYMOD_SHIFT;
            return sc;
        }
    }
    return 0;
}

static void serial_tty_plain(uint8_t c) {
    uint8_t keymod = 0;

    // \r\n from the other end is the one enter
    int after_cr = g_ttyS.in_cr;
    g_ttyS.in_cr = (c == '\r');
    if (c == '\n' && after_cr) return;

    if (c == '\r' || c == '\n') {
        serial_tty_push(SC_ENTER, 0);
        return;
    }
    if (c == 0x7F || c == '\b') {
        serial_tty_push(SC_BACKSPACE, 0);
        return;
    }

    // ctrl+letter comes over as 1 to 26
    if (c >= 1 && c <= 26 && c != '\t') {
        uint16_t sc = serial_tty_scancode((char)('a' + c - 1), &keymod);
        if (sc) serial_tty_push(sc, KEYMOD_CTRL);
        if (c == 3)
            serial_tty_interrupt();
        return;
    }

    uint16_t sc = serial_tty_scancode((char)c, &keymod);
    if (sc) serial_tty_push(sc, keymod);
}

// the modifier in ESC[1;5D is 1 + shift 1 alt 2 ctrl 4
static uint8_t serial_tty_csi_mod(void) {
    uint32_t m = g_ttyS.in_param > 0 && g_ttyS.in_params[1] ? g_ttyS.in_params[1] - 1 : 0;
    return ((m & 1) ? KEYMOD_SHIFT : 0) | ((m & 2) ? KEYMOD_ALT : 0) | ((m & 4) ? KEYMOD_CTRL : 0);
}

static uint16_t serial_tty_final(uint8_t c) {
    switch (c) {
        case 'A': return SC_UP;
        case 'B': return SC_DOWN;
        case 'C': return SC_RIGHT;
        case 'D': return SC_LEFT;
        case 'H': return SC_HOME;
        case 'F': return SC_END;
    }
    if (c != '~') return 0;

    switch (g_ttyS.in_params[0]) {
        case 1: case 7: return SC_HOME;
        case 4: case 8: return SC_END;
        case 2: return SC_INSERT;
        case 3: return SC_DELETE;
        case 5: return SC_PAGE_UP;
        case 6: return SC_PAGE_DOWN;
    }
    return 0;
}

static void serial_tty_in(uint8_t c) {
    switch (g_ttyS.in_state) {
        case IN_PLAIN:
            if (c == 0x1B) g_ttyS.in_state = IN_ESC;
            else           serial_tty_plain(c);
            return;

        case IN_ESC:
            if (c == '[' || c == 'O') {
                g_ttyS.in_state     = c == '[' ? IN_CSI : IN_SS3;
                g_ttyS.in_param     = 0;
                g_ttyS.in_params[0] = 0;
                g_ttyS.in_params[1] = 0;
                return;
            }
            g_ttyS.in_state = IN_PLAIN;
            serial_tty_push(SC_ESCAPE, 0);
            serial_tty_in(c);
            return;

        case IN_CSI:
            if (c >= '0' && c <= '9') {
                g_ttyS.in_params[g_ttyS.in_param] = g_ttyS.in_params[g_ttyS.in_param] * 10 + (c - '0');
                return;
            }
            if (c == ';') {
                if (g_ttyS.in_param < 1) g_ttyS.in_param++;
                return;
            }
            // the final byte is handled the same as an SS3s
            __attribute__((fallthrough));
        case IN_SS3: {
            g_ttyS.in_state = IN_PLAIN;
            uint16_t sc = serial_tty_final(c);
            if (sc) serial_tty_push(sc, serial_tty_csi_mod());
            return;
        }
    }
}

static void serial_tty_rx(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        serial_tty_in((uint8_t)buf[i]);

    if (g_ttyS.in_state == IN_ESC) {
        g_ttyS.in_state = IN_PLAIN;
        serial_tty_push(SC_ESCAPE, 0);
    }
}

static long serial_tty_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode; (void)offset;
    if (len < sizeof(keyboard_event_t)) return -EINVAL;

    for (;;) {
        task_t *current = get_current_task();
        if (signal_should_interrupt(current))
            return -EINTR;

        unsigned long irq = irq_push();
        spinlock_acquire(&g_ttyS.lock);

        size_t n = 0;
        while (n + sizeof(keyboard_event_t) <= len && g_ttyS.tail != g_ttyS.head) {
            memcpy((uint8_t *)buf + n, &g_ttyS.events[g_ttyS.tail], sizeof(keyboard_event_t));
            g_ttyS.tail = (g_ttyS.tail + 1) % SERIAL_TTY_EVENTS;
            n += sizeof(keyboard_event_t);
        }

        spinlock_release(&g_ttyS.lock);
        irq_restore(irq);

        if (n)
            return (long)n;
        if (g_ttyS.flags & TTY_NONBLOCK)
            return -EAGAIN;

        // looked at again with interrupts off so a key cant slip in between
        irq = irq_push();
        if (g_ttyS.tail == g_ttyS.head)
            sched_wait_queue(&g_ttyS.wait, 0);
        irq_restore(irq);
    }
}

static void serial_tty_newline(void) {
    g_ttyS.x = 0;
    g_ttyS.y++;
}

// copying xterm
static void serial_tty_printed(void) {
    if (g_ttyS.wrap_pending) {
        g_ttyS.wrap_pending = 0;
        serial_tty_newline();
    }
    if (g_ttyS.x + 1 >= SERIAL_TTY_COLS) g_ttyS.wrap_pending = 1;
    else                                 g_ttyS.x++;
}

static void serial_tty_csi(uint8_t c) {
    uint32_t n = g_ttyS.out_params[0] ? g_ttyS.out_params[0] : 1;
    g_ttyS.wrap_pending = 0;

    switch (c) {
        case 'A': g_ttyS.y = g_ttyS.y > n ? g_ttyS.y - n : 0; break;
        case 'B': g_ttyS.y += n; break;
        case 'C': g_ttyS.x = g_ttyS.x + n < SERIAL_TTY_COLS ? g_ttyS.x + n : SERIAL_TTY_COLS - 1; break;
        case 'D': g_ttyS.x = g_ttyS.x > n ? g_ttyS.x - n : 0; break;
        case 'G': g_ttyS.x = n - 1 < SERIAL_TTY_COLS ? n - 1 : SERIAL_TTY_COLS - 1; break;
        case 'H':
        case 'f':
            g_ttyS.y = n - 1;
            g_ttyS.x = g_ttyS.out_params[1] ? g_ttyS.out_params[1] - 1 : 0;
            break;
    }
}

static void serial_tty_out(uint8_t c) {
    switch (g_ttyS.out_state) {
        case OUT_ESC:
            g_ttyS.out_state = OUT_PLAIN;
            if (c == '[') {
                g_ttyS.out_state     = OUT_CSI;
                g_ttyS.out_param     = 0;
                g_ttyS.out_params[0] = 0;
                g_ttyS.out_params[1] = 0;
            }
            return;

        case OUT_CSI:
            if (c >= '0' && c <= '9') {
                g_ttyS.out_params[g_ttyS.out_param] = g_ttyS.out_params[g_ttyS.out_param] * 10 + (c - '0');
                return;
            }
            if (c == ';') {
                if (g_ttyS.out_param < 1) g_ttyS.out_param++;
                return;
            }
            if (c < 0x40 || c > 0x7E) return;

            g_ttyS.out_state = OUT_PLAIN;
            serial_tty_csi(c);
            return;
    }

    if (c == 0x1B)                g_ttyS.out_state = OUT_ESC;
    else if (c == '\n')           { g_ttyS.wrap_pending = 0; serial_tty_newline(); }
    else if (c == '\r')           { g_ttyS.wrap_pending = 0; g_ttyS.x = 0; }
    else if (c == '\b')           { g_ttyS.wrap_pending = 0; if (g_ttyS.x) g_ttyS.x--; }
    else if (c == '\t')           { g_ttyS.wrap_pending = 0; g_ttyS.x = (g_ttyS.x | 7) + 1 < SERIAL_TTY_COLS ? (g_ttyS.x | 7) + 1 : SERIAL_TTY_COLS - 1; }
    else if (c >= 0x20 && (c & 0xC0) != 0x80) serial_tty_printed();
}

static long serial_tty_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    (void)inode; (void)offset;
    const char *s = buf;

    for (size_t i = 0; i < len; i++)
        serial_tty_out((uint8_t)s[i]);
    serial_send_n(s, len);
    return (long)len;
}

// relative moves, the far end might be scrolled anywhere so a row number means nothing to it
static void serial_tty_move(uint32_t x, uint32_t y) {
    char seq[48];
    int  n = 0;

    if (y < g_ttyS.y)      n += snprintf(seq + n, sizeof(seq) - n, "\x1b[%uA", g_ttyS.y - y);
    else if (y > g_ttyS.y) n += snprintf(seq + n, sizeof(seq) - n, "\x1b[%uB", y - g_ttyS.y);
    if (x < g_ttyS.x)      n += snprintf(seq + n, sizeof(seq) - n, "\x1b[%uD", g_ttyS.x - x);
    else if (x > g_ttyS.x) n += snprintf(seq + n, sizeof(seq) - n, "\x1b[%uC", x - g_ttyS.x);

    g_ttyS.x = x;
    g_ttyS.y = y;
    g_ttyS.wrap_pending = 0;
    if (n) serial_send_n(seq, (size_t)n);
}

static int serial_tty_arg_in(void *arg, void *dst, size_t len) {
    if (!arg) return -EINVAL;
    return copy_from_user(get_current_task(), dst, arg, len) == 0 ? 0 : -EFAULT;
}

static int serial_tty_arg_out(void *arg, const void *src, size_t len) {
    if (!arg) return -EINVAL;
    return copy_to_user(get_current_task(), arg, src, len) == 0 ? 0 : -EFAULT;
}

// no GET_INDEX, theres no /dev/ttyN behind this and nothing to scroll
static int serial_tty_ioctl(INode_t *inode, unsigned long req, void *arg) {
    (void)inode;
    int r;

    switch (req) {
        case TTY_IOCTL_SET_FLAGS:
            return serial_tty_arg_in(arg, &g_ttyS.flags, sizeof(g_ttyS.flags));

        case TTY_IOCTL_GET_FLAGS:
            return serial_tty_arg_out(arg, &g_ttyS.flags, sizeof(g_ttyS.flags));

        case TTY_IOCTL_GET_CURSOR: {
            tty_cursor_t cursor = { .x = g_ttyS.x, .y = g_ttyS.y };
            return serial_tty_arg_out(arg, &cursor, sizeof(cursor));
        }

        case TTY_IOCTL_SET_CURSOR: {
            tty_cursor_t cursor;
            if ((r = serial_tty_arg_in(arg, &cursor, sizeof(cursor))) < 0) return r;
            if (cursor.x < SERIAL_TTY_COLS)
                serial_tty_move(cursor.x, cursor.y);
            return 0;
        }

        case TTY_IOCTL_GET_WINSIZE: {
            tty_winsize_t ws = { .cols = SERIAL_TTY_COLS, .rows = SERIAL_TTY_ROWS };
            return serial_tty_arg_out(arg, &ws, sizeof(ws));
        }

        case TTY_IOCTL_TIOCGWINSZ: {
            uint16_t ws[4] = { SERIAL_TTY_ROWS, SERIAL_TTY_COLS, 0, 0 };
            return serial_tty_arg_out(arg, ws, sizeof(ws));
        }

        case TTY_IOCTL_TIOCSWINSZ:
            return 0;

        case TTY_IOCTL_TCGETS: {
            tty_termios_t term = {0};
            if (g_ttyS.flags & TTY_ECHO)       term.c_lflag |= TTY_TERM_ECHO;
            if (g_ttyS.flags & TTY_CANNONICAL) term.c_lflag |= TTY_TERM_ICANON;
            term.c_lflag |= TTY_TERM_ISIG;
            term.c_cc[TTY_VINTR]  = 3;
            term.c_cc[TTY_VERASE] = 127;
            term.c_cc[TTY_VMIN]   = (g_ttyS.flags & TTY_NONBLOCK) ? 0 : 1;
            return serial_tty_arg_out(arg, &term, sizeof(term));
        }

        case TTY_IOCTL_TCSETS:
        case TTY_IOCTL_TCSETSW:
        case TTY_IOCTL_TCSETSF: {
            tty_termios_t term;
            if ((r = serial_tty_arg_in(arg, &term, sizeof(term))) < 0) return r;

            g_ttyS.flags &= ~(TTY_ECHO | TTY_CANNONICAL | TTY_NONBLOCK);
            if (term.c_lflag & TTY_TERM_ECHO)   g_ttyS.flags |= TTY_ECHO;
            if (term.c_lflag & TTY_TERM_ICANON) g_ttyS.flags |= TTY_CANNONICAL;
            if (term.c_cc[TTY_VMIN] == 0 && term.c_cc[TTY_VTIME] == 0)
                g_ttyS.flags |= TTY_NONBLOCK;
            return 0;
        }

        case TTY_IOCTL_FIONBIO: {
            int nonblock;
            if ((r = serial_tty_arg_in(arg, &nonblock, sizeof(nonblock))) < 0) return r;

            if (nonblock) g_ttyS.flags |=  TTY_NONBLOCK;
            else          g_ttyS.flags &= ~TTY_NONBLOCK;
            return 0;
        }

        default:
            return -ENOTTY;
    }
}

static int serial_tty_poll(INode_t *inode) {
    (void)inode;
    return POLLOUT | (g_ttyS.head != g_ttyS.tail ? POLLIN : 0);
}

static wait_queue_t *serial_tty_wait_queue(INode_t *inode) {
    (void)inode;
    return &g_ttyS.wait;
}

static const INodeOps_t serial_tty_ops = {
    .read       = serial_tty_read,
    .write      = serial_tty_write,
    .ioctl      = serial_tty_ioctl,
    .poll       = serial_tty_poll,
    .wait_queue = serial_tty_wait_queue,
};

int serial_tty_init(void) {
    if (!serial_present()) return -ENODEV;

    memset(&g_ttyS, 0, sizeof(g_ttyS));
    spinlock_init(&g_ttyS.lock);
    g_ttyS.device.ops           = &serial_tty_ops;
    g_ttyS.device.internal_data = &g_ttyS;
    g_ttyS.device.type          = INODE_DEVICE;
    g_ttyS.flags                = TTY_ECHO | TTY_CANNONICAL;

    // the device keeps a hold on its own file, it never goes away
    g_ttyS.file = kmalloc(sizeof(file_t));
    if (!g_ttyS.file) return -ENOMEM;
    memset(g_ttyS.file, 0, sizeof(file_t));
    g_ttyS.file->type   = FD_TYPE_DEV;
    g_ttyS.file->inode  = &g_ttyS.device;
    g_ttyS.file->flags  = O_RDWR;
    g_ttyS.file->shared = 1;
    g_ttyS.device.shared++;

    long r = device_register(&g_ttyS.device, "ttyS0");
    if (r < 0) return (int)r;

    serial_rx_listen(serial_tty_rx);
    g_ttyS_ready = true;
    return 0;
}

task_t *serial_tty_spawn(INode_t *elf) {
    fd_table_t *boot = vfs_get_kernel_table();
    if (!g_ttyS_ready || !boot || !elf) return NULL;

    file_t *saved[3];
    for (int fd = 0; fd < 3; fd++) {
        saved[fd]      = boot->fds[fd];
        boot->fds[fd]  = g_ttyS.file;
    }

    task_t *task = elf_sched(elf, 0, NULL);

    for (int fd = 0; fd < 3; fd++)
        boot->fds[fd] = saved[fd];
    return task;
}
