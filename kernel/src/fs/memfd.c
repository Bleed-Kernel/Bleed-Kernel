#include <fs/memfd.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <mm/cow.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <string.h>

// pages come in as theyre written or mapped, a hole reads as zeros.
typedef struct memfd {
    paddr_t  *pages;
    size_t    cap;      // slots in pages
    uint64_t  size;
    uint32_t  seals;
    int       writers;
    char      name[64];
} memfd_t;

static long memfd_read(INode_t *inode, void *out, size_t size, size_t offset);
static long memfd_write(INode_t *inode, const void *in, size_t size, size_t offset);
static int  memfd_truncate(INode_t *inode, size_t size);
static size_t memfd_size(INode_t *inode);
static void memfd_drop(INode_t *inode);

static const INodeOps_t memfd_ops = {
    .read     = memfd_read,
    .write    = memfd_write,
    .truncate = memfd_truncate,
    .size     = memfd_size,
    .drop     = memfd_drop,
};

bool memfd_is(INode_t *inode) {
    return inode && inode->ops == &memfd_ops;
}

static void frame_put(paddr_t p) {
    if (p && cow_unref_page(p) == 0)
        pmm_free_pages(p, 1);
}

static int grow_slots(memfd_t *m, size_t want) {
    if (want <= m->cap)
        return 0;
    size_t cap = m->cap ? m->cap : 16;
    while (cap < want)
        cap *= 2;

    paddr_t *more = kmalloc(cap * sizeof(paddr_t));
    if (!more)
        return -ENOMEM;
    memset(more, 0, cap * sizeof(paddr_t));
    if (m->cap)
        memcpy(more, m->pages, m->cap * sizeof(paddr_t));
    kfree(m->pages);
    m->pages = more;
    m->cap   = cap;
    return 0;
}

paddr_t memfd_page(INode_t *inode, uint64_t index, bool create) {
    memfd_t *m = inode->internal_data;
    if (index >= (m->size + PAGE_SIZE - 1) / PAGE_SIZE)
        return 0;
    if (index < m->cap && m->pages[index])
        return m->pages[index];
    if (!create || grow_slots(m, index + 1) < 0)
        return 0;

    paddr_t p = pmm_alloc_pages(1);
    if (!p)
        return 0;
    memset(paddr_to_vaddr(p), 0, PAGE_SIZE);
    cow_ref_page(p);
    m->pages[index] = p;
    return p;
}

// drops whole pages past size, and zeros the tail of the one size ends in
static void cut_to(memfd_t *m, uint64_t size) {
    size_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = keep; i < m->cap; i++) {
        frame_put(m->pages[i]);
        m->pages[i] = 0;
    }
    if ((size & (PAGE_SIZE - 1)) && keep - 1 < m->cap && m->pages[keep - 1]) {
        size_t at = size & (PAGE_SIZE - 1);
        memset((uint8_t *)paddr_to_vaddr(m->pages[keep - 1]) + at, 0, PAGE_SIZE - at);
    }
}

static int resize(memfd_t *m, uint64_t size) {
    if (size > m->size && (m->seals & F_SEAL_GROW))
        return -EPERM;
    if (size < m->size && (m->seals & F_SEAL_SHRINK))
        return -EPERM;
    if (size < m->size)
        cut_to(m, size);
    m->size = size;
    return 0;
}

static long memfd_read(INode_t *inode, void *out, size_t size, size_t offset) {
    memfd_t *m = inode->internal_data;
    if (offset >= m->size)
        return 0;
    if (size > m->size - offset)
        size = m->size - offset;

    size_t done = 0;
    while (done < size) {
        uint64_t at   = offset + done;
        size_t   off  = at & (PAGE_SIZE - 1);
        size_t   n    = PAGE_SIZE - off < size - done ? PAGE_SIZE - off : size - done;
        uint64_t page = at / PAGE_SIZE;

        if (page < m->cap && m->pages[page])
            memcpy((uint8_t *)out + done, (uint8_t *)paddr_to_vaddr(m->pages[page]) + off, n);
        else
            memset((uint8_t *)out + done, 0, n);
        done += n;
    }
    return (long)done;
}

static long memfd_write(INode_t *inode, const void *in, size_t size, size_t offset) {
    memfd_t *m = inode->internal_data;
    if (m->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))
        return -EPERM;

    uint64_t end;
    if (__builtin_add_overflow(offset, size, &end))
        return -EFBIG;
    if (end > m->size) {
        int err = resize(m, end);
        if (err)
            return err;
    }

    size_t done = 0;
    while (done < size) {
        uint64_t at  = offset + done;
        size_t   off = at & (PAGE_SIZE - 1);
        size_t   n   = PAGE_SIZE - off < size - done ? PAGE_SIZE - off : size - done;
        paddr_t  p   = memfd_page(inode, at / PAGE_SIZE, true);
        if (!p)
            return done ? (long)done : -ENOMEM;
        memcpy((uint8_t *)paddr_to_vaddr(p) + off, (const uint8_t *)in + done, n);
        done += n;
    }
    return (long)done;
}

static int memfd_truncate(INode_t *inode, size_t size) {
    return resize(inode->internal_data, size);
}

static size_t memfd_size(INode_t *inode) {
    memfd_t *m = inode->internal_data;
    return m->size;
}

static void memfd_drop(INode_t *inode) {
    memfd_t *m = inode->internal_data;
    if (!m)
        return;
    for (size_t i = 0; i < m->cap; i++)
        frame_put(m->pages[i]);
    kfree(m->pages);
    kfree(m);
    inode->internal_data = NULL;
}

int memfd_add_seals(INode_t *inode, uint32_t seals) {
    memfd_t *m = inode->internal_data;
    if (seals & ~(uint32_t)(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))
        return -EINVAL;
    if (m->seals & F_SEAL_SEAL)
        return -EPERM;
    if ((seals & F_SEAL_WRITE) && m->writers)
        return -EBUSY;
    m->seals |= seals;
    return 0;
}

int memfd_get_seals(INode_t *inode) {
    memfd_t *m = inode->internal_data;
    return (int)m->seals;
}

int memfd_track_writer(INode_t *inode, int delta) {
    memfd_t *m = inode->internal_data;
    if (delta > 0 && (m->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)))
        return -EPERM;
    m->writers += delta;
    return 0;
}

int memfd_create(const char *name, unsigned int flags) {
    if (flags & ~(unsigned int)(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return -EINVAL;

    memfd_t *m     = kmalloc(sizeof(memfd_t));
    INode_t *inode = kmalloc(sizeof(INode_t));
    file_t  *f     = kmalloc(sizeof(file_t));
    if (!m || !inode || !f) {
        kfree(m);
        kfree(inode);
        kfree(f);
        return -ENOMEM;
    }
    memset(m, 0, sizeof(*m));
    memset(inode, 0, sizeof(*inode));
    memset(f, 0, sizeof(*f));

    strncpy(m->name, name, sizeof(m->name) - 1);
    // without ALLOW_SEALING it starts sealed shut against new seals
    if (!(flags & MFD_ALLOW_SEALING))
        m->seals = F_SEAL_SEAL;

    inode->type          = INODE_FILE;
    inode->ops           = &memfd_ops;
    inode->internal_data = m;
    inode->shared        = 1;
    memcpy(inode->name, "memfd:", 6);
    strncpy(inode->name + 6, m->name, sizeof(inode->name) - 7);

    f->type   = FD_TYPE_FS;
    f->inode  = inode;
    f->flags  = O_RDWR;
    f->shared = 1;

    int fd = vfs_install(f);
    if (fd < 0) {
        vfs_file_put(f);
        return fd;
    }
    if (flags & MFD_CLOEXEC)
        vfs_set_fd_flags(fd, FD_CLOEXEC);
    return fd;
}
