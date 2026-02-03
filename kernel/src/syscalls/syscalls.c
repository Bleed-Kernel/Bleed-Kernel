#include <stdint.h>
#include <stdio.h>
#include <sched/scheduler.h>
#include <ansii.h>
#include <threads/exit.h>
#include <syscalls/syscall.h>

#define SYSCALL(idx, func) [idx] = (SyscallHandler)func

typedef uint64_t (*SyscallHandler)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

enum {
    SYS_READ,
    SYS_WRITE,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_YEILD,
    SYS_SPAWN,
    SYS_SHUTDOWN,
    SYS_REBOOT,
    SYS_EXIT,
    SYS_WAITPID,
    SYS_TKILL,
    SYS_MEMINFO,
    SYS_TIME,
    SYS_CHDIR,
    SYS_GETCWD,
    SYS_READDIR,
    SYS_STAT,
    SYS_MMAP,
    SYS_MUNMAP,
    SYS_TASKCOUNT,
    SYS_TASKINFO,
    SYS_MAPFB
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
SyscallHandler syscall_handlers[] = {
    SYSCALL(SYS_READ, sys_read),
    SYSCALL(SYS_WRITE, sys_write),
    SYSCALL(SYS_OPEN, sys_open),
    SYSCALL(SYS_CLOSE, sys_close),
    SYSCALL(SYS_YEILD, sys_yield),
    SYSCALL(SYS_SPAWN, sys_spawn),
    SYSCALL(SYS_SHUTDOWN, sys_shutdown),
    SYSCALL(SYS_REBOOT, sys_reboot),
    SYSCALL(SYS_EXIT, sys_exit),
    SYSCALL(SYS_WAITPID, sys_waitpid),
    SYSCALL(SYS_TKILL, sys_tkill),
    SYSCALL(SYS_MEMINFO, sys_meminfo),
    SYSCALL(SYS_TIME, sys_time),
    SYSCALL(SYS_CHDIR, sys_chdir),
    SYSCALL(SYS_GETCWD, sys_getcwd),
    SYSCALL(SYS_READDIR, sys_readdir),
    SYSCALL(SYS_STAT, sys_stat),
    SYSCALL(SYS_MMAP, sys_mmap),
    SYSCALL(SYS_MUNMAP, sys_munmap),
    SYSCALL(SYS_TASKINFO, sys_taskinfo),
    SYSCALL(SYS_TASKCOUNT, sys_taskcount),
    SYSCALL(SYS_MAPFB, sys_mapfb)
};
#pragma GCC diagnostic pop

uint64_t syscall_dispatch(cpu_context_t *cpu_ctx){
    return syscall_handlers[cpu_ctx->rax](
        cpu_ctx->rdi,
        cpu_ctx->rsi,
        cpu_ctx->rdx,
        cpu_ctx->r10,
        cpu_ctx->r8,
        cpu_ctx->r9
    );
}
