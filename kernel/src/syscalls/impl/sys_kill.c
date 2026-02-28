#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/signal.h>
#include <user/errno.h>

typedef struct kill_iter_ctx {
    task_t *sender;
    long signal;
    long target_pgid;
    int sent;
    int found;
    int denied;
    int mode;
} kill_iter_ctx_t;

typedef struct kill_children_ctx {
    task_t *root;
    long signal;
    int sent;
} kill_children_ctx_t;

enum {
    KILL_MODE_PGRP = 1,
    KILL_MODE_ALL = 2
};

static int signal_is_terminating(long signal) {
    if (signal <= 0 || signal >= NSIG)
        return 0;
    if (signal == SIGCHLD || signal == SIGWINCH || signal == SIGCONT)
        return 0;
    if (signal == SIGSTOP || signal == SIGTSTP || signal == SIGTTIN || signal == SIGTTOU)
        return 0;
    return 1;
}

static int task_is_live_user(task_t *task) {
    if (!task)
        return 0;
    if (task->task_privilege == PRIVILEGE_KERNEL)
        return 0;
    if (task->state == TASK_FREE || task->state == TASK_DEAD || task->state == TASK_ZOMBIE)
        return 0;
    return 1;
}

static void kill_children_iter(task_t *task, void *userdata) {
    kill_children_ctx_t *ctx = (kill_children_ctx_t *)userdata;
    if (!ctx || !ctx->root || !task || task == ctx->root)
        return;
    if (!task_is_live_user(task))
        return;
    if (task->ppid != ctx->root->id)
        return;

    if (signal_send(task, (int)ctx->signal) == 0)
        ctx->sent = 1;
}

static int signal_send_with_dependents(task_t *target, long signal) {
    int rc = signal_send(target, (int)signal);
    if (rc < 0)
        return rc;

    if (!signal_is_terminating(signal))
        return 0;

    if (target->state == TASK_BLOCKED && target->wait_target_pid > 0) {
        task_t *dep = sched_get_task(target->wait_target_pid);
        if (task_is_live_user(dep))
            (void)signal_send(dep, (int)signal);
    }

    kill_children_ctx_t cctx = {
        .root = target,
        .signal = signal,
        .sent = 0
    };
    itterate_each_task(kill_children_iter, &cctx);

    return 0;
}

static void kill_iter(task_t *task, void *userdata) {
    kill_iter_ctx_t *ctx = (kill_iter_ctx_t *)userdata;
    if (!task || !ctx || !ctx->sender)
        return;

    if (task->state == TASK_FREE || task->state == TASK_DEAD || task->state == TASK_ZOMBIE)
        return;

    if (task->task_privilege == PRIVILEGE_KERNEL)
        return;

    if (ctx->mode == KILL_MODE_ALL && task == ctx->sender)
        return;

    if (ctx->mode == KILL_MODE_PGRP && (long)task->pgid != ctx->target_pgid)
        return;

    ctx->found = 1;
    if (ctx->signal == 0)
        return;

    if (signal_send(task, (int)ctx->signal) == 0) {
        ctx->sent = 1;
    } else {
        ctx->denied = 1;
    }
}

long sys_kill(long pid, signal_number_t signal) {
    task_t *sender = get_current_task();
    if (!sender)
        return -ESRCH;

    if (signal < 0 || signal >= NSIG)
        return -EINVAL;

    if (pid > 0) {
        task_t *target = sched_get_task((uint64_t)pid);
        if (!target)
            return -ESRCH;
        if (target->state == TASK_FREE || target->state == TASK_DEAD || target->state == TASK_ZOMBIE)
            return -ESRCH;
        if (target->task_privilege == PRIVILEGE_KERNEL)
            return -EPERM;
        if (signal == 0)
            return 0;
        return signal_send_with_dependents(target, signal);
    }

    kill_iter_ctx_t ctx = {
        .sender = sender,
        .signal = signal,
        .target_pgid = 0,
        .sent = 0,
        .found = 0,
        .denied = 0,
        .mode = 0
    };

    if (pid == 0) {
        ctx.mode = KILL_MODE_PGRP;
        ctx.target_pgid = (long)sender->pgid;
    } else if (pid == -1) {
        ctx.mode = KILL_MODE_ALL;
    } else {
        ctx.mode = KILL_MODE_PGRP;
        ctx.target_pgid = -pid;
        if (ctx.target_pgid <= 0)
            return -EINVAL;
    }

    itterate_each_task(kill_iter, &ctx);

    if (!ctx.found)
        return -ESRCH;
    if (!ctx.sent && signal != 0 && ctx.denied)
        return -EPERM;

    return 0;
}
