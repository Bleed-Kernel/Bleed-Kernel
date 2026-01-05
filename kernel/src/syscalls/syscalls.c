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
    SYS_EXIT,
    SYS_CLEAR,
    SYS_YEILD,
    SYS_SPAWN,
    SYS_WAITPID,
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
SyscallHandler syscall_handlers[] = {
    SYSCALL(SYS_READ, sys_read),
    SYSCALL(SYS_WRITE, sys_write),
    SYSCALL(SYS_EXIT, sys_exit),
    SYSCALL(SYS_CLEAR, sys_clear),
    SYSCALL(SYS_YEILD, sys_yield),
    SYSCALL(SYS_SPAWN, sys_spawn),
    SYSCALL(SYS_WAITPID, sys_waitpid)
};
#pragma GCC diagnostic pop

uint64_t syscall_dispatch(cpu_context_t *cpu_ctx){
    return syscall_handlers[cpu_ctx->rax](cpu_ctx->rdi, cpu_ctx->rsi, cpu_ctx->rdx, cpu_ctx->r10, cpu_ctx->r8, cpu_ctx->r9);
}