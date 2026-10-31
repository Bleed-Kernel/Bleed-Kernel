#include <devices/devices.h>
#include <devices/type/touch_device.h>
#include <input/touch_dispatch.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <string.h>
#include <user/errno.h>
#include <user/user_poll.h>
#include <sched/scheduler.h>

static touch_device_t *touch_device = NULL;

static long touch_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)offset;
    touch_device_t *touch = inode->internal_data;
    if (!touch)
        return -EIO;

    unsigned long irq = irq_push();
    spinlock_acquire(&touch->lock);

    if (touch->head == touch->tail) {
        spinlock_release(&touch->lock);
        irq_restore(irq);
        return -EAGAIN;
    }

    size_t bytes_read = 0;
    while (bytes_read + sizeof(touch_event_t) <= len && touch->tail != touch->head) {
        memcpy((uint8_t *)buf + bytes_read, &touch->buffer[touch->tail], sizeof(touch_event_t));

        touch->tail = (touch->tail + 1) % TOUCH_BUFFER_SIZE;
        bytes_read += sizeof(touch_event_t);
    }

    spinlock_release(&touch->lock);
    irq_restore(irq);
    return (long)bytes_read;
}

static int touch_poll(INode_t *inode) {
    touch_device_t *touch = inode->internal_data;
    if (!touch) return POLLERR;
    return touch->head != touch->tail ? POLLIN : 0;
}

static wait_queue_t *touch_wait_queue(INode_t *inode) {
    touch_device_t *touch = inode->internal_data;
    return touch ? &touch->wait : NULL;
}

static struct INodeOps touch_inode_ops = {
    .read = touch_read,
    .poll = touch_poll,
    .wait_queue = touch_wait_queue,
};

static void touch_listener(const touch_event_t *ev) {
    if (!touch_device)
        return;

    unsigned long irq = irq_push();
    spinlock_acquire(&touch_device->lock);

    size_t head = touch_device->head;
    size_t next = (head + 1) % TOUCH_BUFFER_SIZE;

    if (next == touch_device->tail)
        touch_device->tail = (touch_device->tail + 1) % TOUCH_BUFFER_SIZE;

    touch_device->buffer[head] = *ev;
    touch_device->head = next;

    spinlock_release(&touch_device->lock);
    irq_restore(irq);

    sched_wake_queue(&touch_device->wait);
}

void touch_device_init(void) {
    touch_device = kmalloc(sizeof(touch_device_t));
    if (!touch_device) return;
    memset(touch_device, 0, sizeof(touch_device_t));
    spinlock_init(&touch_device->lock);

    touch_device->device.ops = &touch_inode_ops;
    touch_device->device.internal_data = touch_device;
    touch_device->device.type = INODE_DEVICE;

    device_register(&touch_device->device, "touch");
    touch_register_listener(touch_listener);
}
