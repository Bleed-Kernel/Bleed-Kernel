#include <fs/vfs.h>
#include <stdint.h>
#include <stdio.h>
#include <mm/kalloc.h>
#include <string.h>
#include <devices/type/fb_device.h>

uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
    if (fd >= MAX_FDS || !current_fd_table)
        return -1;

    file_t* f = current_fd_table->fds[fd];
    if (!f || !f->inode || !f->inode->ops->ioctl)
        return -1;

    void* karg = NULL;
    size_t arg_size = 0;

    if (request == FB_IOC_GET_INFO) {
        arg_size = sizeof(struct fb_info);
        karg = kmalloc(arg_size);
    }

    if (!karg) return -1;

    int result = f->inode->ops->ioctl(f->inode, request, karg);

    if (result == 0 && request == FB_IOC_GET_INFO) {
        memcpy((void*)arg, karg, arg_size);
    }

    kprintf("Syscall: ioctl on fd %lld, request %llx\n", fd, request);

    kfree(karg, arg_size);
    return result;
}