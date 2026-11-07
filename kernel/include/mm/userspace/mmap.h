#pragma once
#include <sched/scheduler.h>
#include <stdint.h>

// window of user address space that mmap, the framebuffer mapping and ipc hand out
#define USER_MMAP_BASE  0x0000004000000000ULL
#define USER_MMAP_LIMIT 0x00007fffffe00000ULL

// linux values, the syscalls take them as they are
#define PROT_NONE           0x0
#define PROT_READ           0x1
#define PROT_WRITE          0x2
#define PROT_EXEC           0x4

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_TYPE            0x0f
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_NORESERVE       0x4000
#define MAP_POPULATE        0x8000
#define MAP_FIXED_NOREPLACE 0x100000

// ours, never from user space: the fd was read only so the range can never be written
// cause its never used in user we can do exactly whatever we want
#define MAP_KERNEL_NOWRITE  0x80000000

#define MADV_DONTNEED       4
#define MADV_FREE           8

// a range of mmaps, anonymous only for now. gives back the address or -errno
long task_vma_map(struct task *task, uintptr_t hint, size_t len, int prot, int flags);

// MAP_PRIVATE copies of a file or memfd page by page as theyre touched, MAP_SHARED of a
// memfd maps its own pages. inode gets a ref of its very own
struct INode;
long task_vma_map_file(struct task *task, uintptr_t hint, size_t len, int prot, int flags,
                       struct INode *inode, uint64_t offset);

// unmaps whatever mmaps fall in the range, splitting the ones it cuts through
long task_vma_unmap(struct task *task, uintptr_t addr, size_t len);

// -ENOMEM when part of the range isnt mmapped, so the caller can treat it the old way
long task_vma_protect(struct task *task, uintptr_t addr, size_t len, int prot);
long task_vma_discard(struct task *task, uintptr_t addr, size_t len);

typedef struct user_heap {
    uintptr_t current;
    uintptr_t end;
    struct task* task;
} user_heap_t;

struct task;
int task_addr_mapped(struct task *task, uintptr_t addr);
int task_addr_file(struct task *task, uintptr_t addr, const char **name, uint64_t *off);
