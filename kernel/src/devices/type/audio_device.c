#include <devices/devices.h>
#include <devices/type/audio_device.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <user/user_poll.h>
#include <ansii.h>
#include <string.h>

static audio_device_t *audio_dev;

// the window a writer is allowed into: never the period the hardware is on
static uint64_t audio_window_start(audio_device_t *dev) {
    return (dev->played + (dev->running ? 1 : 0)) * dev->period_bytes;
}

static uint64_t audio_window_end(audio_device_t *dev) {
    return (dev->played + dev->periods) * dev->period_bytes;
}

static uint64_t audio_writable(audio_device_t *dev) {
    uint64_t end = audio_window_end(dev);
    return dev->write_pos < end ? end - dev->write_pos : 0;
}

// lock held
static void audio_reset(audio_device_t *dev) {
    memset(dev->ring, 0, dev->ring_bytes);
    dev->running   = false;
    dev->played    = 0;
    dev->write_pos = 0;
}

static void audio_stop_locked(audio_device_t *dev) {
    if (dev->running)
        dev->driver->stop(dev->priv);
    audio_reset(dev);
}

static void audio_lock(audio_device_t *dev, unsigned long *flags) {
    *flags = irq_push();
    spinlock_acquire(&dev->lock);
}

static void audio_unlock(audio_device_t *dev, unsigned long flags) {
    spinlock_release(&dev->lock);
    irq_restore(flags);
}

void audio_device_advance(audio_device_t *dev, uint32_t hw_period) {
    if (!dev || hw_period >= dev->periods)
        return;

    unsigned long flags;
    audio_lock(dev, &flags);
    if (!dev->running) {
        audio_unlock(dev, flags);
        return;
    }

    bool     had_data = dev->write_pos > audio_window_start(dev);
    uint64_t before   = dev->played;
    while ((uint32_t)(dev->played % dev->periods) != hw_period) {
        memset(dev->ring + (dev->played % dev->periods) * dev->period_bytes, 0, dev->period_bytes);
        dev->played++;
    }

    if (dev->played == before) {
        audio_unlock(dev, flags);
        return;
    }

    uint64_t start = audio_window_start(dev);
    if (dev->write_pos < start) {
        if (had_data)
            dev->underruns++;
        dev->write_pos = start;
    }

    // nobody left to feed it and whatever they queued is 
    if (!dev->owner_pid && !had_data)
        audio_stop_locked(dev);

    audio_unlock(dev, flags);
    sched_wake_queue(&dev->wait);
}

// lock held
static int audio_start_locked(audio_device_t *dev) {
    if (dev->running)
        return 0;

    int r = dev->driver->start(dev->priv);
    if (r < 0)
        return r;
    dev->running = true;
    return 0;
}

static int audio_map_ring(audio_device_t *dev, task_t *task) {
    if (dev->owner_map)
        return 0;

    size_t    pages = dev->ring_bytes / PAGE_SIZE;
    uintptr_t base  = (uintptr_t)task_mmap_reserve(task, pages);
    if (!base)
        return -ENOMEM;

    // NOFREE, its the cards ring and not the tasks memory
    for (size_t i = 0; i < pages; i++)
        paging_map_page(task->page_map, dev->ring_phys + i * PAGE_SIZE, base + i * PAGE_SIZE,
                        PTE_USER | PTE_WRITABLE | PTE_NX | PTE_NOFREE);

    dev->owner_map = base;
    return 0;
}

static bool audio_claim(audio_device_t *dev, task_t *task) {
    if (dev->owner_pid && dev->owner_pid != task->id)
        return false;
    if (!dev->owner_pid) {
        dev->owner_prio = task->prio_base;
        if (task->prio_base > PRIO_AUDIO)
            task->prio_base = PRIO_AUDIO;
    }
    dev->owner_pid = task->id;
    return true;
}

static void audio_get_pos(audio_device_t *dev, struct audio_pos *pos) {
    uint64_t period_start = dev->played * dev->period_bytes;
    uint64_t hw_pos       = period_start;

    if (dev->running) {
        uint64_t off  = dev->driver->hw_offset(dev->priv);
        uint64_t base = (dev->played % dev->periods) * dev->period_bytes;
        hw_pos += (off + dev->ring_bytes - base) % dev->ring_bytes;
    }

    pos->write_pos = dev->write_pos;
    pos->writable  = audio_writable(dev);
    pos->played    = period_start;
    pos->hw_pos    = hw_pos;
    pos->underruns = dev->underruns;
    pos->running   = dev->running;
    pos->_pad      = 0;
}

static long audio_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    (void)offset;
    audio_device_t *dev  = inode->internal_data;
    task_t         *task = get_current_task();
    if (!dev || !task)
        return -ENODEV;

    // plain writes for anything that doesnt want the ring, it plays as soon as theres something in it
    size_t copied = 0;
    while (copied < len) {
        sigset_t seen = task->sig_pending;

        unsigned long flags;
        audio_lock(dev, &flags);
        if (!audio_claim(dev, task)) {
            audio_unlock(dev, flags);
            return copied ? (long)copied : -EBUSY;
        }

        uint64_t room = audio_writable(dev);
        if (room) {
            size_t chunk = len - copied;
            if (chunk > room)
                chunk = room;

            size_t at    = dev->write_pos % dev->ring_bytes;
            size_t first = chunk < dev->ring_bytes - at ? chunk : dev->ring_bytes - at;
            memcpy(dev->ring + at, (const uint8_t *)buf + copied, first);
            memcpy(dev->ring, (const uint8_t *)buf + copied + first, chunk - first);

            dev->write_pos += chunk;
            copied         += chunk;

            int r = audio_start_locked(dev);
            audio_unlock(dev, flags);
            if (r < 0)
                return copied ? (long)copied : r;
            continue;
        }
        audio_unlock(dev, flags);

        if (signal_should_interrupt(task))
            return copied ? (long)copied : -EINTR;

        // looked at again with interrupts off so the tick cant free a period in between
        flags = irq_push();
        if (!audio_writable(dev) && task->sig_pending == seen)
            sched_wait_queue(&dev->wait, 0);
        irq_restore(flags);
    }

    return (long)copied;
}

static int audio_poll(INode_t *inode) {
    audio_device_t *dev = inode->internal_data;
    if (!dev) return POLLERR;
    return audio_writable(dev) >= dev->period_bytes ? POLLOUT : 0;
}

static wait_queue_t *audio_wait_queue(INode_t *inode) {
    audio_device_t *dev = inode->internal_data;
    return dev ? &dev->wait : NULL;
}

static int audio_ioctl_info(audio_device_t *dev, task_t *task, void *arg) {
    struct audio_info info = {
        .rate         = dev->rate,
        .channels     = dev->channels,
        .format       = AUDIO_FORMAT_S16LE,
        .frame_bytes  = dev->channels * 2,
        .period_bytes = (uint32_t)dev->period_bytes,
        .periods      = dev->periods,
        .ring_bytes   = dev->ring_bytes,
    };
    return copy_to_user(task, arg, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
}

// poll says theres room, this is for keeping a small lead where theres always room
static int audio_wait_period(audio_device_t *dev, task_t *task) {
    if (!dev->running)
        return -EAGAIN;
    if (signal_should_interrupt(task))
        return -EINTR;

    sigset_t      seen   = task->sig_pending;
    uint64_t      played = dev->played;
    unsigned long flags  = irq_push();
    if (dev->running && dev->played == played && task->sig_pending == seen)
        sched_wait_queue(&dev->wait, sched_deadline_ms(AUDIO_WAIT_MS));
    irq_restore(flags);
    return 0;
}

static int audio_ioctl(INode_t *inode, unsigned long request, void *arg) {
    audio_device_t *dev  = inode->internal_data;
    task_t         *task = get_current_task();
    if (!dev)
        return -ENODEV;
    if (!task)
        return -ESRCH;

    // the ones that dont need the card
    if (request == AUDIO_IOC_GET_INFO)
        return arg ? audio_ioctl_info(dev, task, arg) : -EFAULT;

    if (request == AUDIO_IOC_GET_OWNER) {
        uint64_t pid = dev->owner_pid;
        return arg && copy_to_user(task, arg, &pid, sizeof(pid)) == 0 ? 0 : -EFAULT;
    }

    if (request == AUDIO_IOC_GET_POS) {
        if (!arg) return -EFAULT;

        struct audio_pos pos;
        unsigned long flags;
        audio_lock(dev, &flags);
        audio_get_pos(dev, &pos);
        audio_unlock(dev, flags);
        return copy_to_user(task, arg, &pos, sizeof(pos)) == 0 ? 0 : -EFAULT;
    }

    if (request == AUDIO_IOC_WAIT)
        return audio_wait_period(dev, task);

    // anyone can turn it up or down, the tray does it without ever owning the card
    if (request == AUDIO_IOC_SET_VOLUME) {
        struct audio_volume vol;
        if (!arg || copy_from_user(task, &vol, arg, sizeof(vol)) != 0)
            return -EFAULT;
        if (vol.left > 100)  vol.left  = 100;
        if (vol.right > 100) vol.right = 100;
        vol.mute = vol.mute ? 1 : 0;
        vol._pad = 0;
        if (!dev->driver->set_volume)
            return -ENOTTY;

        unsigned long flags;
        audio_lock(dev, &flags);
        int r = dev->driver->set_volume(dev->priv, &vol);
        if (r == 0)
            dev->volume = vol;
        audio_unlock(dev, flags);
        return r;
    }

    if (request == AUDIO_IOC_GET_VOLUME) {
        struct audio_volume vol = dev->volume;
        return arg && copy_to_user(task, arg, &vol, sizeof(vol)) == 0 ? 0 : -EFAULT;
    }

    // the rest belong to whoever has it, copied in before the lock
    uint32_t rate   = 0;
    uint64_t commit = 0;
    if (request == AUDIO_IOC_SET_RATE && (!arg || copy_from_user(task, &rate, arg, sizeof(rate)) != 0))
        return -EFAULT;
    if (request == AUDIO_IOC_COMMIT && (!arg || copy_from_user(task, &commit, arg, sizeof(commit)) != 0))
        return -EFAULT;

    unsigned long flags;
    audio_lock(dev, &flags);

    if (request == AUDIO_IOC_ACQUIRE) {
        if (!arg) {
            audio_unlock(dev, flags);
            return -EFAULT;
        }
        if (!audio_claim(dev, task)) {
            audio_unlock(dev, flags);
            return -EBUSY;
        }

        int r = audio_map_ring(dev, task);
        struct audio_map map = { .addr = dev->owner_map, .ring_bytes = dev->ring_bytes };
        audio_unlock(dev, flags);
        if (r < 0)
            return r;
        return copy_to_user(task, arg, &map, sizeof(map)) == 0 ? 0 : -EFAULT;
    }

    if (dev->owner_pid != task->id) {
        audio_unlock(dev, flags);
        return -EPERM;
    }

    int r = 0;
    switch (request) {
        case AUDIO_IOC_RELEASE: {
            // whats queued still plays out
            uintptr_t map  = dev->owner_map;
            dev->owner_pid = 0;
            dev->owner_map = 0;
            task->prio_base = dev->owner_prio;
            audio_unlock(dev, flags);
            if (map)
                task_munmap(task, (void *)map);
            return 0;
        }
        case AUDIO_IOC_START:
            r = audio_start_locked(dev);
            break;
        case AUDIO_IOC_STOP:
            // the tick doesnt look at a stopped card
            audio_stop_locked(dev);
            audio_unlock(dev, flags);
            sched_wake_queue(&dev->wait);
            return 0;
        case AUDIO_IOC_COMMIT: {
            uint64_t room = audio_writable(dev);
            if (commit > room)
                commit = room;
            dev->write_pos += commit;
            audio_unlock(dev, flags);
            return copy_to_user(task, arg, &commit, sizeof(commit)) == 0 ? 0 : -EFAULT;
        }
        case AUDIO_IOC_SET_RATE:
            if (dev->running) {
                r = -EBUSY;
                break;
            }
            r = dev->driver->set_rate ? dev->driver->set_rate(dev->priv, rate) : -ENOTTY;
            if (r < 0)
                break;
            dev->rate = (uint32_t)r;
            rate      = (uint32_t)r;
            audio_unlock(dev, flags);
            return copy_to_user(task, arg, &rate, sizeof(rate)) == 0 ? 0 : -EFAULT;
        default:
            r = -EINVAL;
            break;
    }

    audio_unlock(dev, flags);
    return r;
}

static struct INodeOps audio_inode_ops = {
    .write      = audio_write,
    .ioctl      = audio_ioctl,
    .poll       = audio_poll,
    .wait_queue = audio_wait_queue,
};

void audio_device_task_exit(task_t *task) {
    if (!audio_dev || !task || audio_dev->owner_pid != task->id)
        return;

    // the mapping goes with the address space, the queued audio still drains
    unsigned long flags;
    audio_lock(audio_dev, &flags);
    audio_dev->owner_pid = 0;
    audio_dev->owner_map = 0;
    audio_unlock(audio_dev, flags);
}

audio_device_t *audio_device_register(const char *name, const audio_driver_t *driver, void *priv,
                                      paddr_t ring_phys, size_t period_bytes, uint32_t periods,
                                      uint32_t rate, uint32_t channels) {
    if (audio_dev || !driver || !ring_phys || !period_bytes || !periods)
        return NULL;

    audio_device_t *dev = kmalloc(sizeof(audio_device_t));
    if (!dev) return NULL;
    memset(dev, 0, sizeof(audio_device_t));
    spinlock_init(&dev->lock);

    dev->driver       = driver;
    dev->priv         = priv;
    dev->ring_phys    = ring_phys;
    dev->ring         = paddr_to_vaddr(ring_phys);
    dev->period_bytes = period_bytes;
    dev->periods      = periods;
    dev->ring_bytes   = period_bytes * periods;
    dev->rate         = rate;
    dev->channels     = channels;
    dev->volume       = (struct audio_volume){ .left = 100, .right = 100 };
    audio_reset(dev);

    dev->device.ops           = &audio_inode_ops;
    dev->device.internal_data = dev;
    dev->device.type          = INODE_DEVICE;

    if (device_register(&dev->device, (char *)name) < 0) {
        kfree(dev);
        return NULL;
    }

    audio_dev = dev;
    serial_printf(LOG_OK "audio: %s is /dev/%s, %u Hz, %u periods of %u bytes\n", driver->name, name,
                  rate, periods, (uint32_t)period_bytes);
    return dev;
}
