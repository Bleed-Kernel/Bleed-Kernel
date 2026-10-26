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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
SyscallHandler syscall_handlers[] = {
    SYSCALL(SYS_READ, sys_read),
    SYSCALL(SYS_WRITE, sys_write),
    SYSCALL(SYS_OPEN, sys_open),
    SYSCALL(SYS_CLOSE, sys_close),
    SYSCALL(SYS_IOCTL, sys_ioctl),
    SYSCALL(SYS_YIELD, sys_yield),
    SYSCALL(SYS_SPAWN, sys_spawn),
    SYSCALL(SYS_EXIT, sys_exit),
    SYSCALL(SYS_WAITPID, sys_waitpid),
    SYSCALL(SYS_KILL, sys_kill),
    SYSCALL(SYS_MEMINFO, sys_meminfo),
    SYSCALL(SYS_TIME, sys_time),
    SYSCALL(SYS_CHDIR, sys_chdir),
    SYSCALL(SYS_GETCWD, sys_getcwd),
    SYSCALL(SYS_READDIR, sys_readdir),
    SYSCALL(SYS_STAT, sys_stat),
    SYSCALL(SYS_FSTAT, sys_fstat),
    SYSCALL(SYS_LSTAT, sys_stat),   // for now its the same thing because we dont have links 
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
    SYSCALL(SYS_EXEC, sys_exec),
    SYSCALL(SYS_IPC_SEND, sys_ipc_send),
    SYSCALL(SYS_IPC_RECV, sys_ipc_recv),
    SYSCALL(SYS_PIPE, sys_pipe),
    SYSCALL(SYS_DUP2, sys_dup2),
    SYSCALL(SYS_UNLINK, sys_unlink),
    SYSCALL(SYS_RENAME, sys_rename),
    SYSCALL(SYS_MKDIR, sys_mkdir),
    SYSCALL(SYS_SHM_CREATE, sys_shm_create),
    SYSCALL(SYS_SHM_MAP, sys_shm_map),
    SYSCALL(SYS_SHM_DESTROY, sys_shm_destroy),
    SYSCALL(SYS_POLL, sys_poll),
    SYSCALL(SYS_TASKUSAGE, sys_taskusage),
    SYSCALL(SYS_TASKPATH, sys_taskpath),
    SYSCALL(SYS_SHM_MAP_AT, sys_shm_map_at),
    SYSCALL(SYS_EXECVE, sys_execve),
    SYSCALL(SYS_WAITSTATUS, sys_waitstatus),
    SYSCALL(SYS_ARCH_PRCTL, sys_arch_prctl),
    SYSCALL(SYS_MPROTECT, sys_mprotect),
    SYSCALL(SYS_DUP, sys_dup),
    SYSCALL(SYS_GETPPID, sys_getppid),
    SYSCALL(SYS_OPENAT, sys_openat),
    SYSCALL(SYS_FSTATAT, sys_fstatat),
    SYSCALL(SYS_MOUNTINFO, sys_mountinfo),
    SYSCALL(SYS_MOUNTLABEL, sys_mountlabel),
    SYSCALL(SYS_SOCKET, sys_socket),
    SYSCALL(SYS_CONNECT, sys_connect),
    SYSCALL(SYS_SENDTO, sys_sendto),
    SYSCALL(SYS_RECVFROM, sys_recvfrom),
    SYSCALL(SYS_BIND, sys_bind),
    SYSCALL(SYS_GETSOCKNAME, sys_getsockname),
    SYSCALL(SYS_GETPEERNAME, sys_getpeername),
    SYSCALL(SYS_SETSOCKOPT, sys_setsockopt),
    SYSCALL(SYS_GETSOCKOPT, sys_getsockopt)
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
