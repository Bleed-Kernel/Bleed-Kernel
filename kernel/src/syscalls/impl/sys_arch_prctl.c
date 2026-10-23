#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <cpu/msrs.h>

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

long sys_arch_prctl(uint64_t code, uint64_t addr) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    switch (code) {
    case ARCH_SET_FS:
        // a non canonical base would fault the wrmsr in ring 0, a kernel one hands user the kernel
        if (addr && !user_ptr_valid(addr))
            return -EPERM;
        task->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;

    case ARCH_GET_FS:
        if (copy_to_user(task, (void *)addr, &task->fs_base, sizeof(task->fs_base)) != 0)
            return -EFAULT;
        return 0;
    }

    return -EINVAL;
}
