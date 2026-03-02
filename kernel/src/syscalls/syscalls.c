#include <stdint.h>
#include <stdio.h>
#include <sched/scheduler.h>
#include <ansii.h>
#include <threads/exit.h>
#include <syscalls/syscall.h>
#include <sched/signal.h>
#include <user/errno.h>
#include <drivers/serial/serial.h>

#define SYSCALL(idx, func) [idx] = (SyscallHandler)func

typedef uint64_t (*SyscallHandler)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

enum {
    SYS_READ,
    SYS_WRITE,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_IOCTL,
    SYS_YEILD,
    SYS_SPAWN,
    SYS_SHUTDOWN,
    SYS_REBOOT,
    SYS_EXIT,
    SYS_WAITPID,
    SYS_KILL,
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
    SYS_MAPFB,
    SYS_SEEK,
    SYS_SIGACTION,
    SYS_SIGPROCMASK,
    SYS_SIGRETURN,
    SYS_GETPID,
    SYS_FORK,
    SYS_EXEC
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
SyscallHandler syscall_handlers[] = {
    SYSCALL(SYS_READ, sys_read),
    SYSCALL(SYS_WRITE, sys_write),
    SYSCALL(SYS_OPEN, sys_open),
    SYSCALL(SYS_CLOSE, sys_close),
    SYSCALL(SYS_IOCTL, sys_ioctl),
    SYSCALL(SYS_YEILD, sys_yield),
    SYSCALL(SYS_SPAWN, sys_spawn),
    SYSCALL(SYS_SHUTDOWN, sys_shutdown),
    SYSCALL(SYS_REBOOT, sys_reboot),
    SYSCALL(SYS_EXIT, sys_exit),
    SYSCALL(SYS_WAITPID, sys_waitpid),
    SYSCALL(SYS_KILL, sys_kill),
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
    SYSCALL(SYS_MAPFB, sys_mapfb),
    SYSCALL(SYS_SEEK, sys_seek),
    SYSCALL(SYS_SIGACTION, sys_sigaction),
    SYSCALL(SYS_SIGPROCMASK, sys_sigprocmask),
    SYSCALL(SYS_SIGRETURN, sys_sigreturn),
    SYSCALL(SYS_GETPID, sys_getpid),
    SYSCALL(SYS_FORK, sys_fork),
    SYSCALL(SYS_EXEC, sys_exec)
};
#pragma GCC diagnostic pop

uint64_t syscall_dispatch(cpu_context_t *cpu_ctx){
    uint64_t sysno = cpu_ctx->rax;
    uint64_t ret = (uint64_t)-ENOSYS;

    task_t *current = get_current_task();
    if (current)
        current->context = cpu_ctx;

    if (sysno < (sizeof(syscall_handlers) / sizeof(syscall_handlers[0])) &&
        syscall_handlers[sysno]) {
        ret = syscall_handlers[sysno](
            cpu_ctx->rdi,
            cpu_ctx->rsi,
            cpu_ctx->rdx,
            cpu_ctx->r10,
            cpu_ctx->r8,
            cpu_ctx->r9
        );
    }

    cpu_ctx->rax = ret;
    
    if (current && (cpu_ctx->cs & 0x3) == 0x3) {
        if (sysno == SYS_SIGRETURN) {
            long sigreturn_rc = signal_handle_sigreturn(current, cpu_ctx);
            if (sigreturn_rc < 0) {
                cpu_ctx->rax = (uint64_t)sigreturn_rc;
            } else {
                signal_deliver_pending(current, cpu_ctx);
            }
        } else {
            signal_deliver_pending(current, cpu_ctx);
        }
    }

    return cpu_ctx->rax;
}
