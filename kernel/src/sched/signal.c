#include <sched/signal.h>
#include <sched/scheduler.h>
#include <threads/exit.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <string.h>

typedef struct sigframe {
    cpu_context_t saved_ctx;
    sigset_t old_mask;
    uintptr_t prev_frame;
} sigframe_t;

static inline int valid_signal(int sig) {
    return sig > 0 && sig < NSIG;
}

static inline sigset_t sig_bit(int sig) {
    return (sigset_t)1ULL << (sig - 1);
}

static int signal_default_ignored(int sig) {
    return sig == SIGCHLD || sig == SIGWINCH || sig == SIGCONT;
}

static int signal_default_terminate(int sig) {
    (void)sig;
    return 1;
}

int signal_send(task_t *task, int sig) {
    if (!task)
        return -ESRCH;
    if (!valid_signal(sig))
        return -EINVAL;

    task->sig_pending |= sig_bit(sig);
    if (task->state == TASK_BLOCKED)
        task->state = TASK_READY;
    return 0;
}

int signal_send_pid(uint64_t pid, int sig) {
    task_t *task = sched_get_task(pid);
    if (!task)
        return -ESRCH;
    return signal_send(task, sig);
}

int signal_set_action(task_t *task, int sig, const sigaction_t *act, sigaction_t *old) {
    if (!task)
        return -ESRCH;
    if (!valid_signal(sig))
        return -EINVAL;

    if (old) {
        old->handler = task->sig_handlers[sig];
        old->mask = task->sig_masks[sig];
        old->flags = task->sig_flags[sig];
        old->restorer = task->sig_restorers[sig];
    }

    if (!act)
        return 0;

    if (sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;

    task->sig_handlers[sig] = act->handler;
    task->sig_masks[sig] = act->mask;
    task->sig_flags[sig] = act->flags;
    task->sig_restorers[sig] = act->restorer;
    return 0;
}

int signal_set_mask(task_t *task, int how, const sigset_t *set, sigset_t *old) {
    if (!task)
        return -ESRCH;

    if (old)
        *old = task->sig_blocked;

    if (!set)
        return 0;

    sigset_t new_mask = task->sig_blocked;
    switch (how) {
        case SIG_BLOCK:
            new_mask |= *set;
            break;
        case SIG_UNBLOCK:
            new_mask &= ~(*set);
            break;
        case SIG_SETMASK:
            new_mask = *set;
            break;
        default:
            return -EINVAL;
    }

    new_mask &= ~sig_bit(SIGKILL);
    new_mask &= ~sig_bit(SIGSTOP);
    task->sig_blocked = new_mask;
    return 0;
}

static int signal_setup_user_handler(task_t *task, cpu_context_t *ctx, int sig) {
    uintptr_t handler = task->sig_handlers[sig];
    uintptr_t restorer = task->sig_restorers[sig];
    sigset_t handler_mask = task->sig_masks[sig];
    uint64_t handler_flags = task->sig_flags[sig];
    uintptr_t stack_floor = USER_STACK_TOP - USER_STACK_SIZE;

    if (handler == SIG_DFL || handler == SIG_IGN || restorer < 2)
        return -EINVAL;

    uintptr_t frame_addr = (ctx->rsp - sizeof(sigframe_t)) & ~0xFULL;
    if (frame_addr < stack_floor)
        return -EFAULT;

    sigframe_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.saved_ctx = *ctx;
    frame.old_mask = task->sig_blocked;
    frame.prev_frame = task->sig_active_frame;

    if (copy_to_user(task, (void *)frame_addr, &frame, sizeof(frame)) != 0)
        return -EFAULT;

    uintptr_t ret_addr_sp = frame_addr - sizeof(uint64_t);
    if (ret_addr_sp < stack_floor)
        return -EFAULT;

    uint64_t ret_addr = (uint64_t)restorer;
    if (copy_to_user(task, (void *)ret_addr_sp, &ret_addr, sizeof(ret_addr)) != 0)
        return -EFAULT;

    task->sig_active_frame = frame_addr;

    task->sig_blocked |= handler_mask;
    if (!(handler_flags & SA_NODEFER))
        task->sig_blocked |= sig_bit(sig);

    ctx->rip = handler;
    ctx->rdi = (uint64_t)sig;
    ctx->rsp = ret_addr_sp;
    return 0;
}

int signal_handle_sigreturn(task_t *task, cpu_context_t *ctx) {
    if (!task || !ctx || !task->sig_active_frame)
        return -EINVAL;

    sigframe_t frame;
    if (copy_from_user(task, &frame, (const void *)task->sig_active_frame, sizeof(frame)) != 0)
        return -EFAULT;

    *ctx = frame.saved_ctx;
    task->sig_blocked = frame.old_mask;
    task->sig_active_frame = frame.prev_frame;
    return 0;
}

void signal_deliver_pending(task_t *task, cpu_context_t *ctx) {
    if (!task || !ctx || task->task_privilege != PRIVILEGE_USER)
        return;

    if (task->sig_active_frame)
        return;

    sigset_t ready = task->sig_pending & ~task->sig_blocked;
    if (!ready)
        return;

    for (int sig = 1; sig < NSIG; sig++) {
        sigset_t bit = sig_bit(sig);
        if (!(ready & bit))
            continue;

        task->sig_pending &= ~bit;

        uintptr_t handler = task->sig_handlers[sig];
        if (handler == SIG_IGN || (handler == SIG_DFL && signal_default_ignored(sig)))
            return;

        if (handler == SIG_DFL && signal_default_terminate(sig)) {
            task->exit_signal = sig;
            task->exit_code = 128 + sig;
            exit();
        }

        if (signal_setup_user_handler(task, ctx, sig) < 0) {
            task->exit_signal = sig;
            task->exit_code = 128 + sig;
            exit();
        }
        return;
    }
}
