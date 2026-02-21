#include <devices/devices.h>
#include <devices/type/mouse_device.h>
#include <input/mouse_dispatch.h>
#include <mm/kalloc.h>
#include <string.h>

static mouse_device_t *mouse_device = NULL;

static long mouse_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)offset;
    mouse_device_t *mouse = inode->internal_data;
    if (!mouse)
        return -1;

    spinlock_acquire(&mouse->lock);

    if (mouse->head == mouse->tail) {
        spinlock_release(&mouse->lock);
        return 0;
    }

    size_t bytes_read = 0;
    while (bytes_read + sizeof(mouse_event_t) <= len && mouse->tail != mouse->head) {
        mouse_event_t *event = &mouse->buffer[mouse->tail];
        memcpy((uint8_t *)buf + bytes_read, event, sizeof(mouse_event_t));

        mouse->tail = (mouse->tail + 1) % MOUSE_BUFFER_SIZE;
        bytes_read += sizeof(mouse_event_t);
    }

    spinlock_release(&mouse->lock);
    return (long)bytes_read;
}

static struct INodeOps mouse_inode_ops = {
    .read = mouse_read,
};

static void mouse_listener(const mouse_event_t *ev) {
    if (!mouse_device)
        return;

    spinlock_acquire(&mouse_device->lock);

    size_t head = mouse_device->head;
    size_t next = (head + 1) % MOUSE_BUFFER_SIZE;

    if (next == mouse_device->tail)
        mouse_device->tail = (mouse_device->tail + 1) % MOUSE_BUFFER_SIZE;

    mouse_device->buffer[head] = *ev;
    mouse_device->head = next;

    spinlock_release(&mouse_device->lock);
}

void mouse_device_init(void) {
    mouse_device = kmalloc(sizeof(mouse_device_t));
    memset(mouse_device, 0, sizeof(mouse_device_t));
    spinlock_init(&mouse_device->lock);

    mouse_device->device.ops = &mouse_inode_ops;
    mouse_device->device.internal_data = mouse_device;
    mouse_device->device.type = INODE_DEVICE;

    device_register(&mouse_device->device, "mouse0");
    mouse_register_listener(mouse_listener);
}
