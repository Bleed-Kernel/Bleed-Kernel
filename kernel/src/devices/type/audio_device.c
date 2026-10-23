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

static void audio_lock(audio_device_t *dev, unsigned long *flags) {
    *flags = irq_push();
    spinlock_acquire(&dev->lock);
}

static void audio_unlock(audio_device_t *dev, unsigned long flags) {
    spinlock_release(&dev->lock);
    irq_restore(flags);
}

// streams

// lock held
static audio_stream_t *stream_of(audio_device_t *dev, uint64_t pid) {
    for (int i = 0; i < AUDIO_STREAMS; i++) {
        audio_stream_t *s = &dev->streams[i];
        if (s->used && s->pid == pid)
            return s;
    }
    return NULL;
}

// lock held
static audio_stream_t *stream_new(audio_device_t *dev, task_t *task) {
    for (int i = 0; i < AUDIO_STREAMS; i++) {
        audio_stream_t *s = &dev->streams[i];
        if (s->used)
            continue;

        paddr_t ring = pmm_alloc_pages(dev->ring_bytes / PAGE_SIZE);
        if (!ring)
            return NULL;

        memset(s, 0, sizeof(*s));
        s->used      = true;
        s->pid       = task->id;
        s->ring_phys = ring;
        s->ring      = paddr_to_vaddr(ring);

        // whoever is making noise gets in ahead of whatever isnt
        s->prio = task->prio_base;
        if (task->prio_base > PRIO_AUDIO)
            task->prio_base = PRIO_AUDIO;
        return s;
    }
    return NULL;
}

// lock held. what it already wrote is in the cards ring by now and still plays
static void stream_free(audio_device_t *dev, audio_stream_t *s) {
    pmm_free_pages(s->ring_phys, dev->ring_bytes / PAGE_SIZE);
    memset(s, 0, sizeof(*s));
}

// a running stream can write from the period
static uint64_t stream_window_start(audio_device_t *dev, audio_stream_t *s) {
    if (!s->running)
        return 0;
    uint64_t next = (dev->played + 1) * dev->period_bytes;
    return next > s->origin ? next - s->origin : 0;
}

static uint64_t stream_window_end(audio_device_t *dev, audio_stream_t *s) {
    if (!s->running)
        return dev->ring_bytes - (AUDIO_JOIN_AHEAD + 1) * dev->period_bytes;
    return (dev->played + dev->periods) * dev->period_bytes - s->origin;
}

static uint64_t stream_writable(audio_device_t *dev, audio_stream_t *s) {
    uint64_t end = stream_window_end(dev, s);
    return s->write_pos < end ? end - s->write_pos : 0;
}

// mixing

static int16_t clip16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// lock held. bytes from..to of a streams own positions added into the cards ring
static void stream_mix(audio_device_t *dev, audio_stream_t *s, uint64_t from, uint64_t to) {
    from -= from % sizeof(int16_t);
    for (uint64_t at = from; at + sizeof(int16_t) <= to; at += sizeof(int16_t)) {
        int16_t *out = (int16_t *)(dev->ring + (s->origin + at) % dev->ring_bytes);
        int16_t  in  = *(const int16_t *)(s->ring + at % dev->ring_bytes);
        *out = clip16((int32_t)*out + in);
    }

    uint64_t end = (s->origin + to + dev->period_bytes - 1) / dev->period_bytes;
    if (end > dev->heard)
        dev->heard = end;
}

static bool any_running(audio_device_t *dev) {
    for (int i = 0; i < AUDIO_STREAMS; i++)
        if (dev->streams[i].used && dev->streams[i].running)
            return true;
    return false;
}

// lock held
static void card_stop(audio_device_t *dev) {
    if (dev->running)
        dev->driver->stop(dev->priv);
    memset(dev->ring, 0, dev->ring_bytes);
    dev->running = false;
    dev->played  = 0;
    dev->heard   = 0;
}

// lock held
static int card_start(audio_device_t *dev) {
    if (dev->running)
        return 0;

    int r = dev->driver->start(dev->priv);
    if (r < 0) {
        card_stop(dev);
        return r;
    }
    dev->running = true;
    return 0;
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

    uint64_t before = dev->played;
    while ((uint32_t)(dev->played % dev->periods) != hw_period) {
        memset(dev->ring + (dev->played % dev->periods) * dev->period_bytes, 0, dev->period_bytes);
        dev->played++;
    }
    if (dev->played == before) {
        audio_unlock(dev, flags);
        return;
    }

    // a stream that fell behind the hardware lost that bit, it carries on from where the hardware is
    for (int i = 0; i < AUDIO_STREAMS; i++) {
        audio_stream_t *s = &dev->streams[i];
        if (!s->used || !s->running)
            continue;

        uint64_t start = stream_window_start(dev, s);
        if (s->write_pos < start) {
            uint64_t was = (before + 1) * dev->period_bytes;
            if (s->write_pos + s->origin > was)
                s->underruns++;
            s->write_pos = start;
        }
    }

    // nobody playing and the last of what they had has been heard
    if (!any_running(dev) && dev->played >= dev->heard)
        card_stop(dev);

    audio_unlock(dev, flags);
    sched_wake_queue(&dev->wait);
}

// lock held. a fresh card starts on the streams first byte.
static int stream_start(audio_device_t *dev, audio_stream_t *s) {
    if (s->running)
        return 0;

    s->origin  = dev->running ? (dev->played + AUDIO_JOIN_AHEAD) * dev->period_bytes : 0;
    s->running = true;
    stream_mix(dev, s, 0, s->write_pos);

    int r = card_start(dev);
    if (r < 0)
        s->running = false;
    return r;
}

// lock held. nothing more goes in
static void stream_stop(audio_stream_t *s) {
    s->running   = false;
    s->write_pos = 0;
    s->origin    = 0;
}

// lock held. bytes the stream wrote into its ring at write_pos
static void stream_commit(audio_device_t *dev, audio_stream_t *s, uint64_t bytes) {
    if (s->running)
        stream_mix(dev, s, s->write_pos, s->write_pos + bytes);
    s->write_pos += bytes;
}

static int stream_map(audio_device_t *dev, audio_stream_t *s, task_t *task) {
    if (s->map)
        return 0;

    size_t    pages = dev->ring_bytes / PAGE_SIZE;
    uintptr_t base  = (uintptr_t)task_mmap_reserve(task, pages);
    if (!base)
        return -ENOMEM;

    // NOFREE, the stream owns the pages
    for (size_t i = 0; i < pages; i++)
        paging_map_page(task->page_map, s->ring_phys + i * PAGE_SIZE, base + i * PAGE_SIZE,
                        PTE_USER | PTE_WRITABLE | PTE_NX | PTE_NOFREE);

    s->map = base;
    return 0;
}

static void stream_pos(audio_device_t *dev, audio_stream_t *s, struct audio_pos *pos) {
    memset(pos, 0, sizeof(*pos));
    pos->write_pos = s->write_pos;
    pos->writable  = stream_writable(dev, s);
    pos->underruns = s->underruns;
    pos->running   = s->running && dev->running;
    if (!pos->running)
        return;

    uint64_t played = dev->played * dev->period_bytes;
    uint64_t off    = dev->driver->hw_offset(dev->priv);
    uint64_t base   = (dev->played % dev->periods) * dev->period_bytes;
    uint64_t hw     = played + (off + dev->ring_bytes - base) % dev->ring_bytes;

    pos->played = played > s->origin ? played - s->origin : 0;
    pos->hw_pos = hw > s->origin ? hw - s->origin : 0;
}

// inode ops

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
        audio_stream_t *s = stream_of(dev, task->id);
        if (!s)
            s = stream_new(dev, task);
        if (!s) {
            audio_unlock(dev, flags);
            return copied ? (long)copied : -EBUSY;
        }

        int r = stream_start(dev, s);
        if (r < 0) {
            audio_unlock(dev, flags);
            return copied ? (long)copied : r;
        }

        uint64_t room = stream_writable(dev, s);
        if (room) {
            size_t chunk = len - copied;
            if (chunk > room)
                chunk = room;

            size_t at    = s->write_pos % dev->ring_bytes;
            size_t first = chunk < dev->ring_bytes - at ? chunk : dev->ring_bytes - at;
            memcpy(s->ring + at, (const uint8_t *)buf + copied, first);
            memcpy(s->ring, (const uint8_t *)buf + copied + first, chunk - first);

            stream_commit(dev, s, chunk);
            copied += chunk;
            audio_unlock(dev, flags);
            continue;
        }
        audio_unlock(dev, flags);

        if (signal_should_interrupt(task))
            return copied ? (long)copied : -EINTR;

        // looked at again with interrupts off so the tick cant free a period in between
        flags = irq_push();
        if (!stream_writable(dev, s) && task->sig_pending == seen)
            sched_wait_queue(&dev->wait, 0);
        irq_restore(flags);
    }

    return (long)copied;
}

static int audio_poll(INode_t *inode) {
    audio_device_t *dev  = inode->internal_data;
    task_t         *task = get_current_task();
    if (!dev || !task)
        return POLLERR;

    audio_stream_t *s = stream_of(dev, task->id);
    if (!s)
        return POLLOUT;
    return stream_writable(dev, s) >= dev->period_bytes ? POLLOUT : 0;
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

static uint64_t first_pid(audio_device_t *dev) {
    for (int i = 0; i < AUDIO_STREAMS; i++)
        if (dev->streams[i].used)
            return dev->streams[i].pid;
    return 0;
}

static int audio_ioctl(INode_t *inode, unsigned long request, void *arg) {
    audio_device_t *dev  = inode->internal_data;
    task_t         *task = get_current_task();
    if (!dev)
        return -ENODEV;
    if (!task)
        return -ESRCH;

    // the ones that dont need a stream
    if (request == AUDIO_IOC_GET_INFO)
        return arg ? audio_ioctl_info(dev, task, arg) : -EFAULT;

    if (request == AUDIO_IOC_GET_OWNER) {
        uint64_t pid = first_pid(dev);
        return arg && copy_to_user(task, arg, &pid, sizeof(pid)) == 0 ? 0 : -EFAULT;
    }

    if (request == AUDIO_IOC_WAIT)
        return audio_wait_period(dev, task);

    // anyone can turn it up or down, the tray does it without ever playing anything
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

    // the rest are about the callers own stream, copied in before the lock
    uint32_t rate   = 0;
    uint64_t commit = 0;
    if (request == AUDIO_IOC_SET_RATE && (!arg || copy_from_user(task, &rate, arg, sizeof(rate)) != 0))
        return -EFAULT;
    if (request == AUDIO_IOC_COMMIT && (!arg || copy_from_user(task, &commit, arg, sizeof(commit)) != 0))
        return -EFAULT;

    unsigned long flags;
    audio_lock(dev, &flags);
    audio_stream_t *s = stream_of(dev, task->id);

    if (request == AUDIO_IOC_ACQUIRE) {
        if (!arg) {
            audio_unlock(dev, flags);
            return -EFAULT;
        }
        if (!s)
            s = stream_new(dev, task);
        if (!s) {
            audio_unlock(dev, flags);
            return -EBUSY;
        }

        int r = stream_map(dev, s, task);
        struct audio_map map = { .addr = s->map, .ring_bytes = dev->ring_bytes };
        audio_unlock(dev, flags);
        if (r < 0)
            return r;
        return copy_to_user(task, arg, &map, sizeof(map)) == 0 ? 0 : -EFAULT;
    }

    if (!s) {
        audio_unlock(dev, flags);
        return -EPERM;
    }

    int r = 0;
    switch (request) {
        case AUDIO_IOC_RELEASE: {
            uintptr_t map   = s->map;
            task->prio_base = s->prio;
            stream_free(dev, s);
            audio_unlock(dev, flags);
            if (map)
                task_munmap(task, (void *)map);
            return 0;
        }
        case AUDIO_IOC_START:
            r = stream_start(dev, s);
            break;
        case AUDIO_IOC_STOP:
            stream_stop(s);
            audio_unlock(dev, flags);
            sched_wake_queue(&dev->wait);
            return 0;
        case AUDIO_IOC_GET_POS: {
            struct audio_pos pos;
            stream_pos(dev, s, &pos);
            audio_unlock(dev, flags);
            return arg && copy_to_user(task, arg, &pos, sizeof(pos)) == 0 ? 0 : -EFAULT;
        }
        case AUDIO_IOC_COMMIT: {
            uint64_t room = stream_writable(dev, s);
            if (commit > room)
                commit = room;
            stream_commit(dev, s, commit);
            audio_unlock(dev, flags);
            return copy_to_user(task, arg, &commit, sizeof(commit)) == 0 ? 0 : -EFAULT;
        }
        case AUDIO_IOC_SET_RATE:
            // everyone shares the one rate, so it only changes while nobody else is using it
            if (dev->running) {
                r = -EBUSY;
                break;
            }
            for (int i = 0; i < AUDIO_STREAMS; i++) {
                if (dev->streams[i].used && dev->streams[i].pid != task->id) {
                    r = -EBUSY;
                    break;
                }
            }
            if (r < 0)
                break;
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
    if (!audio_dev || !task)
        return;

    // the mapping goes with the address space
    unsigned long flags;
    audio_lock(audio_dev, &flags);
    audio_stream_t *s = stream_of(audio_dev, task->id);
    if (s)
        stream_free(audio_dev, s);
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
    card_stop(dev);

    dev->device.ops           = &audio_inode_ops;
    dev->device.internal_data = dev;
    dev->device.type          = INODE_DEVICE;

    if (device_register(&dev->device, (char *)name) < 0) {
        kfree(dev);
        return NULL;
    }

    audio_dev = dev;
    serial_printf(LOG_OK "audio: %s is /dev/%s, %u Hz, %u periods of %u bytes, up to %u programs\n", driver->name,
                  name, rate, periods, (uint32_t)period_bytes, AUDIO_STREAMS);
    return dev;
}
