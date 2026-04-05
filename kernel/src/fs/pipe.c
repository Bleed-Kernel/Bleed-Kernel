#include <fs/pipe.h>

#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <string.h>
#include <user/errno.h>

#define PIPE_BUFFER_SIZE 4096

typedef struct pipe_state {
    spinlock_t lock;
    size_t read_pos;
    size_t write_pos;
    size_t count;
    int readers;
    int writers;
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
        spinlock_release(&state->lock);
        irq_restore(flags);

        if (copied > 0)
            return (long)copied;
        if (writers == 0)
            return 0;

        sched_yield(get_current_task());
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

        if (copied == size)
            return (long)copied;
        if (!full)
            continue;

        sched_yield(get_current_task());
    }
}

static const INodeOps_t pipe_read_ops = {
    .create = NULL,
    .read = pipe_read_inode,
    .write = NULL,
    .lookup = NULL,
    .ioctl = NULL,
    .drop = NULL,
    .readdir = NULL,
    .size = NULL
};

static const INodeOps_t pipe_write_ops = {
    .create = NULL,
    .read = NULL,
    .write = pipe_write_inode,
    .lookup = NULL,
    .ioctl = NULL,
    .drop = NULL,
    .readdir = NULL,
    .size = NULL
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
        if (read_inode) kfree(read_inode, sizeof(INode_t));
        if (write_inode) kfree(write_inode, sizeof(INode_t));
        if (read_file) kfree(read_file, sizeof(file_t));
        if (write_file) kfree(write_file, sizeof(file_t));
        kfree(state, sizeof(pipe_state_t));
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
        kfree(f->inode, sizeof(INode_t));
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

    kfree(f->inode, sizeof(INode_t));
    if (free_state)
        kfree(state, sizeof(pipe_state_t));
}
