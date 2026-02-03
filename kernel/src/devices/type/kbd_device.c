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

typedef struct {
    INode_t device;
    spinlock_t lock;
    char buffer[KBD_BUFFER_SIZE];
    size_t head;
    size_t tail;
} kbd_device_t;

static kbd_device_t *kbd0 = NULL;

static long kbd_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)offset;
    kbd_device_t *kbd = inode->internal_data;
    spinlock_acquire(&kbd->lock);

    if (kbd->head == kbd->tail) {
        spinlock_release(&kbd->lock);
        return 0;
    }

    size_t count = 0;
    while (count < len && kbd->tail != kbd->head) {
        ((char *)buf)[count++] = kbd->buffer[kbd->tail % KBD_BUFFER_SIZE];
        kbd->tail = (kbd->tail + 1) % KBD_BUFFER_SIZE;
    }

    spinlock_release(&kbd->lock);
    return count;
}

static struct INodeOps kbd_inode_ops = {
    .read = kbd_read,
};

static void kbd_listener(const keyboard_event_t *ev) {
    if (ev->action != KEY_DOWN)
        return;

    char c = tty_key_to_ascii(ev);
    if (!c) return;

    spinlock_acquire(&kbd0->lock);
    size_t next_head = (kbd0->head + 1) % KBD_BUFFER_SIZE;
    if (next_head != kbd0->tail) {
        kbd0->buffer[kbd0->head] = c;
        kbd0->head = next_head;
    }else{
        serial_printf("%sKBD: Buffer overflow, dropping key\n", LOG_WARN);
    }

    spinlock_release(&kbd0->lock);
}

void kbd_device_init(INode_t* kbd_inode) {
    kbd0 = kmalloc(sizeof(kbd_device_t));
    memset(kbd0, 0, sizeof(*kbd0));
    spinlock_init(&kbd0->lock);
    if (kbd_inode) {
        file_t* kbd_fd = kmalloc(sizeof(file_t));
        kbd_fd->inode = kbd_inode;
        kbd_fd->flags = O_RDONLY;
        kbd_fd->offset = 0;
        kbd_fd->shared = 1;

        device_register(&kbd0->device, "kbd0");
        
        kbd_inode->ops = &kbd_inode_ops;
        kbd_inode->internal_data = kbd0;

        current_fd_table->fds[3] = kbd_fd;
        keyboard_register_listener(kbd_listener);
    }
}
