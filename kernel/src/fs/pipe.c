#include <fs/pipe.h>

#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <string.h>
#include <user/errno.h>
#include <user/user_poll.h>
#include <user/user_copy.h>
#include <devices/type/tty_device.h>

#define PIPE_BUFFER_SIZE 4096

typedef struct pipe_state {
    spinlock_t lock;
    size_t read_pos;
    size_t write_pos;
    size_t count;
    int readers;
    int writers;
    uint8_t nonblock;
    wait_queue_t wait;
    char buffer[PIPE_BUFFER_SIZE];
} pipe_state_t;

static inline size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

static long pipe_read_inode(INode_t *inode, void *out_buffer, size_t size, size_t offset) {
    (void)offset;
    if (!inode || !out_buffer || size == 0)
        return 0;

    pipe_state_t *state = (pipe_state_t *)inode->internal_data;
    if (!state)
        return -EIO;

    char *dst = (char *)out_buffer;
    size_t copied = 0;

    for (;;) {
        unsigned long flags = irq_push();
        spinlock_acquire(&state->lock);

        while (copied < size && state->count > 0) {
            size_t bytes = min_size(size - copied, state->count);
            size_t first = min_size(bytes, PIPE_BUFFER_SIZE - state->read_pos);

            memcpy(dst + copied, state->buffer + state->read_pos, first);
            state->read_pos = (state->read_pos + first) % PIPE_BUFFER_SIZE;
            state->count -= first;
            copied += first;

            size_t remaining = bytes - first;
            if (remaining > 0) {
                memcpy(dst + copied, state->buffer + state->read_pos, remaining);
                state->read_pos += remaining;
                state->count -= remaining;
                copied += remaining;
            }
        }

        int writers = state->writers;
        int nonblock = state->nonblock;
        spinlock_release(&state->lock);
        irq_restore(flags);

        // theres room again, a writer might be polling for it
        if (copied > 0) {
            sched_wake_queue(&state->wait);
            return (long)copied;
        }
        if (writers == 0)
            return 0;
        if (nonblock)
            return -EAGAIN;

        // checked with the lock dropped, a fatal signal never comes back from this
        task_t  *current = get_current_task();
        sigset_t seen    = current->sig_pending;
        if (signal_should_interrupt(current))
            return -EINTR;

        // looked at again with interrupts off
        flags = irq_push();
        if (!state->count && state->writers && current->sig_pending == seen)
            sched_wait_queue(&state->wait, 0);
        irq_restore(flags);
    }
}

static long pipe_write_inode(INode_t *inode, const void *in_buffer, size_t size, size_t offset) {
    (void)offset;
    if (!inode || !in_buffer || size == 0)
        return 0;

    pipe_state_t *state = (pipe_state_t *)inode->internal_data;
    if (!state)
        return -EIO;

    const char *src = (const char *)in_buffer;
    size_t copied = 0;

    for (;;) {
        unsigned long flags = irq_push();
        spinlock_acquire(&state->lock);

        if (state->readers == 0) {
            spinlock_release(&state->lock);
            irq_restore(flags);
            return copied ? (long)copied : -EPIPE;
        }

        while (copied < size && state->count < PIPE_BUFFER_SIZE) {
            size_t space = PIPE_BUFFER_SIZE - state->count;
            size_t bytes = min_size(size - copied, space);
            size_t first = min_size(bytes, PIPE_BUFFER_SIZE - state->write_pos);

            memcpy(state->buffer + state->write_pos, src + copied, first);
            state->write_pos = (state->write_pos + first) % PIPE_BUFFER_SIZE;
            state->count += first;
            copied += first;

            size_t remaining = bytes - first;
            if (remaining > 0) {
                memcpy(state->buffer + state->write_pos, src + copied, remaining);
                state->write_pos += remaining;
                state->count += remaining;
                copied += remaining;
            }
        }

        int full = (state->count == PIPE_BUFFER_SIZE);
        spinlock_release(&state->lock);
        irq_restore(flags);

        if (copied > 0)
            sched_wake_queue(&state->wait);

        if (copied == size)
            return (long)copied;
        if (!full)
            continue;

        task_t  *current = get_current_task();
        sigset_t seen    = current->sig_pending;
        if (signal_should_interrupt(current))
            return copied ? (long)copied : -EINTR;

        flags = irq_push();
        if (state->count == PIPE_BUFFER_SIZE && state->readers && current->sig_pending == seen)
            sched_wait_queue(&state->wait, 0);
        irq_restore(flags);
    }
}

// the same flag ioctls a tty and the keyboard answer, so a program that reads key events
// off stdin sets itself up the same way when stdin is a pipe from a terminal window
static int pipe_read_ioctl(INode_t *inode, unsigned long request, void *arg) {
    pipe_state_t *state = (pipe_state_t *)inode->internal_data;
    task_t *task = get_current_task();
    if (!state) return -EIO;
    if (!arg)   return -EINVAL;

    switch (request) {
        case TTY_IOCTL_FIONBIO: {
            int nonblock;
            if (copy_from_user(task, &nonblock, arg, sizeof(nonblock)) != 0) return -EFAULT;
            state->nonblock = nonblock ? 1 : 0;
            return 0;
        }
        case TTY_IOCTL_SET_FLAGS: {
            uint32_t flags;
            if (copy_from_user(task, &flags, arg, sizeof(flags)) != 0) return -EFAULT;
            state->nonblock = (flags & TTY_NONBLOCK) ? 1 : 0;
            return 0;
        }
        case TTY_IOCTL_GET_FLAGS: {
            uint32_t flags = state->nonblock ? TTY_NONBLOCK : 0;
            return copy_to_user(task, arg, &flags, sizeof(flags)) == 0 ? 0 : -EFAULT;
        }
        default:
            return -ENOTTY;
    }
}

static int pipe_read_poll(INode_t *inode) {
    pipe_state_t *state = (pipe_state_t *)inode->internal_data;
    if (!state) return POLLERR;

    int ready = 0;
    if (state->count > 0)    ready |= POLLIN;
    if (state->writers == 0) ready |= POLLHUP;
    return ready;
}

static int pipe_write_poll(INode_t *inode) {
    pipe_state_t *state = (pipe_state_t *)inode->internal_data;
    if (!state) return POLLERR;

    // nobody left to read it, a write would only get EPIPE
    if (state->readers == 0) return POLLERR;
    return state->count < PIPE_BUFFER_SIZE ? POLLOUT : 0;
}

static wait_queue_t *pipe_wait_queue(INode_t *inode) {
    pipe_state_t *state = (pipe_state_t *)inode->internal_data;
    return state ? &state->wait : NULL;
}

static const INodeOps_t pipe_read_ops = {
    .create = NULL,
    .read = pipe_read_inode,
    .write = NULL,
    .lookup = NULL,
    .ioctl = pipe_read_ioctl,
    .drop = NULL,
    .readdir = NULL,
    .size = NULL,
    .poll = pipe_read_poll,
    .wait_queue = pipe_wait_queue
};

static const INodeOps_t pipe_write_ops = {
    .create = NULL,
    .read = NULL,
    .write = pipe_write_inode,
    .lookup = NULL,
    .ioctl = NULL,
    .drop = NULL,
    .readdir = NULL,
    .size = NULL,
    .poll = pipe_write_poll,
    .wait_queue = pipe_wait_queue
};

int pipe_create_file_pair(file_t **out_read, file_t **out_write) {
    if (!out_read || !out_write)
        return -EINVAL;

    pipe_state_t *state = (pipe_state_t *)kmalloc(sizeof(pipe_state_t));
    if (!state)
        return -ENOMEM;
    memset(state, 0, sizeof(pipe_state_t));
    spinlock_init(&state->lock);
    state->readers = 1;
    state->writers = 1;

    INode_t *read_inode = (INode_t *)kmalloc(sizeof(INode_t));
    INode_t *write_inode = (INode_t *)kmalloc(sizeof(INode_t));
    file_t *read_file = (file_t *)kmalloc(sizeof(file_t));
    file_t *write_file = (file_t *)kmalloc(sizeof(file_t));

    if (!read_inode || !write_inode || !read_file || !write_file) {
        if (read_inode) kfree(read_inode);
        if (write_inode) kfree(write_inode);
        if (read_file) kfree(read_file);
        if (write_file) kfree(write_file);
        kfree(state);
        return -ENOMEM;
    }

    memset(read_inode, 0, sizeof(INode_t));
    memset(write_inode, 0, sizeof(INode_t));
    memset(read_file, 0, sizeof(file_t));
    memset(write_file, 0, sizeof(file_t));

    read_inode->type = INODE_DEVICE;
    read_inode->ops = &pipe_read_ops;
    read_inode->internal_data = state;
    read_inode->shared = 1;

    write_inode->type = INODE_DEVICE;
    write_inode->ops = &pipe_write_ops;
    write_inode->internal_data = state;
    write_inode->shared = 1;

    read_file->type = FD_TYPE_PIPE;
    read_file->inode = read_inode;
    read_file->offset = 0;
    read_file->flags = O_RDONLY;
    read_file->shared = 1;

    write_file->type = FD_TYPE_PIPE;
    write_file->inode = write_inode;
    write_file->offset = 0;
    write_file->flags = O_WRONLY;
    write_file->shared = 1;

    *out_read = read_file;
    *out_write = write_file;
    return 0;
}

void pipe_file_release(file_t *f) {
    if (!f || f->type != FD_TYPE_PIPE || !f->inode)
        return;

    pipe_state_t *state = (pipe_state_t *)f->inode->internal_data;
    if (!state) {
        kfree(f->inode);
        return;
    }

    unsigned long flags = irq_push();
    spinlock_acquire(&state->lock);

    int mode = f->flags & O_MODE;
    if (mode == O_RDONLY)
        state->readers--;
    else if (mode == O_WRONLY)
        state->writers--;

    int free_state = (state->readers <= 0 && state->writers <= 0);
    spinlock_release(&state->lock);
    irq_restore(flags);

    // whoever is polling the other end needs to see the hangup
    if (free_state)
        sched_wait_queue_destroy(&state->wait);
    else
        sched_wake_queue(&state->wait);

    kfree(f->inode);
    if (free_state)
        kfree(state);
}
