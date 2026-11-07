/*
    Shitty implementation mark rewrite?
*/

#include <sched/signal.h>
#include <sched/scheduler.h>
#include <threads/exit.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <string.h>
#include <stddef.h>
#include <gdt/gdt.h>
#include <mm/kalloc.h>

#include "priv_scheduler.h"

// the only rflags bits a task gets to pick for itself on sigreturn (status flags, TF and DF)
#define RFLAGS_USER_MASK    0xDD5ULL
#define RFLAGS_IF           0x200ULL

typedef struct sigframe {
    cpu_context_t saved_ctx;
    sigset_t old_mask;
    uintptr_t prev_frame;
    user_siginfo_t info;
} sigframe_t;

// SA_SIGINFO handlers get linuxs rt frame instead, the restorer address sits right under
// it so rdx is a real ucontext_t and whatever the handler changes in it
typedef struct linux_sigframe {
    user_ucontext_t uc;
    user_siginfo_t  info;
    uintptr_t       prev_frame;
} linux_sigframe_t;

// sig_active_frame is 16 aligned, the low bit says which kind it is
#define FRAME_LINUX 1ULL

enum { G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15, G_RDI, G_RSI, G_RBP, G_RBX,
       G_RDX, G_RAX, G_RCX, G_RSP, G_RIP, G_EFL, G_CSGSFS, G_ERR, G_TRAPNO, G_OLDMASK, G_CR2 };

static void ctx_to_gregs(const cpu_context_t *c, uint64_t *g) {
    g[G_R8]  = c->r8;  g[G_R9]  = c->r9;  g[G_R10] = c->r10; g[G_R11] = c->r11;
    g[G_R12] = c->r12; g[G_R13] = c->r13; g[G_R14] = c->r14; g[G_R15] = c->r15;
    g[G_RDI] = c->rdi; g[G_RSI] = c->rsi; g[G_RBP] = c->rbp; g[G_RBX] = c->rbx;
    g[G_RDX] = c->rdx; g[G_RAX] = c->rax; g[G_RCX] = c->rcx; g[G_RSP] = c->rsp;
    g[G_RIP] = c->rip; g[G_EFL] = c->rflags;
    g[G_CSGSFS] = c->cs;
    g[G_ERR]    = c->error;
    g[G_TRAPNO] = c->vector;
}

// yum
static void gregs_to_ctx(const uint64_t *g, cpu_context_t *c) {
    c->r8  = g[G_R8];  c->r9  = g[G_R9];  c->r10 = g[G_R10]; c->r11 = g[G_R11];
    c->r12 = g[G_R12]; c->r13 = g[G_R13]; c->r14 = g[G_R14]; c->r15 = g[G_R15];
    c->rdi = g[G_RDI]; c->rsi = g[G_RSI]; c->rbp = g[G_RBP]; c->rbx = g[G_RBX];
    c->rdx = g[G_RDX]; c->rax = g[G_RAX]; c->rcx = g[G_RCX]; c->rsp = g[G_RSP];
    c->rip = g[G_RIP]; c->rflags = g[G_EFL];
}

static inline int valid_signal(int sig) {
    return sig > 0 && sig < NSIG;
}

static inline sigset_t sig_bit(int sig) {
    return (sigset_t)1ULL << (sig - 1);
}

static inline sigset_t valid_sig_mask(void) {
    return ((sigset_t)1ULL << (NSIG - 1)) - 1ULL;
}

static int signal_default_ignored(int sig) {
    return sig == SIGCHLD || sig == SIGWINCH || sig == SIGCONT;
}

static int signal_default_stop(int sig) {
    return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

static int signal_default_terminate(int sig) {
    return !signal_default_ignored(sig) && !signal_default_stop(sig);
}

static int signal_apply_default_action(task_t *task, int sig, sigset_t bit) {
    uintptr_t handler = task->sig_handlers[sig];
    if (handler == SIG_IGN || (handler == SIG_DFL && signal_default_ignored(sig)))
        return 0;

    if (handler == SIG_DFL && signal_default_stop(sig)) {
        task->state = TASK_STOPPED;
        task->sig_pending &= ~bit;
        sched_yield(task);
        return 1;
    }

    if (handler == SIG_DFL && signal_default_terminate(sig)) {
        task->sig_pending &= ~bit;
        task->exit_signal = sig;
        task->exit_code = 128 + sig;
        exit();
    }

    return -1;
}

int signal_send(task_t *task, int sig) {
    if (!task)
        return -ESRCH;
    if (!valid_signal(sig))
        return -EINVAL;

    // nothing left to deliver to, and flipping it back to READY would stop it ever being reaped
    if (task->state == TASK_ZOMBIE || task->state == TASK_DEAD || task->state == TASK_FREE)
        return 0;

    if (sig == SIGCONT) {
        task->sig_pending &= ~sig_bit(SIGSTOP);
        task->sig_pending &= ~sig_bit(SIGTSTP);
        task->sig_pending &= ~sig_bit(SIGTTIN);
        task->sig_pending &= ~sig_bit(SIGTTOU);
        sched_continue_task(task);
    }
    if (signal_default_stop(sig))
        task->sig_pending &= ~sig_bit(SIGCONT);

    task->sig_pending |= sig_bit(sig);
    if (task->state == TASK_BLOCKED)
        sched_wake_task(task);
    else
        sched_continue_task(task);
    return 0;
}

int signal_send_group(task_t *task, int sig) {
    if (!task)
        return -ESRCH;
    if (!valid_signal(sig))
        return -EINVAL;

    task_group_t *g = task->group;
    if (!g || !g->threads)
        return signal_send(task, sig);

    // stopping, continuing and killing a process is done to all of it
    if (sig == SIGKILL || sig == SIGCONT ||
        (signal_default_stop(sig) && task->sig_handlers[sig] == SIG_DFL)) {
        for (task_t *t = g->threads; t; t = t->group_next)
            signal_send(t, sig);
        return 0;
    }

    // otherwise its one thread that isnt blocking it, the leader if it can
    task_t *pick = NULL;
    for (task_t *t = g->threads; t; t = t->group_next) {
        if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD || t->state == TASK_FREE)
            continue;
        if (!(t->sig_blocked & sig_bit(sig))) {
            pick = t;
            break;
        }
        if (!pick)
            pick = t;
    }

    return pick ? signal_send(pick, sig) : 0;
}

int signal_send_pid(uint64_t pid, int sig) {
    task_t *task = sched_get_task(pid);
    if (!task)
        return -ESRCH;
    return signal_send_group(task, sig);
}

static void signal_copy_action(task_t *task, int sig, const sigaction_t *act) {
    task->sig_handlers[sig]  = act->handler;
    task->sig_masks[sig]     = act->mask;
    task->sig_flags[sig]     = act->flags;
    task->sig_restorers[sig] = act->restorer;
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

    // handlers belong to the process, every thread keeps a copy
    if (!task->group || !task->group->threads) {
        signal_copy_action(task, sig, act);
        return 0;
    }

    for (task_t *t = task->group->threads; t; t = t->group_next)
        signal_copy_action(t, sig, act);
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

    new_mask &= valid_sig_mask();
    new_mask &= ~sig_bit(SIGKILL);
    new_mask &= ~sig_bit(SIGSTOP);
    task->sig_blocked = new_mask;
    return 0;
}

#define SIGNAL_RED_ZONE 128

static int on_altstack(task_t *task, uintptr_t sp) {
    return task->sig_altstack_size && sp > task->sig_altstack_sp &&
           sp <= task->sig_altstack_sp + task->sig_altstack_size;
}

static void signal_enter_mask(task_t *task, int sig, sigset_t handler_mask, uint64_t handler_flags) {
    task->sig_blocked |= handler_mask;
    if (!(handler_flags & (SA_NODEFER | SA_RESETHAND)))
        task->sig_blocked |= sig_bit(sig);
    if (handler_flags & SA_RESETHAND) {
        task->sig_handlers[sig] = SIG_DFL;
        task->sig_flags[sig] &= ~(uint64_t)SA_SIGINFO;
    }
}

static int signal_setup_linux_frame(task_t *task, cpu_context_t *ctx, int sig, int code, uint64_t addr,
                                    uintptr_t top) {
    // ucontext 16 aligned, and the restorer slot under it leaves rsp how a call would
    uintptr_t frame_addr = (top - sizeof(linux_sigframe_t)) & ~0xFULL;
    uintptr_t ret_addr_sp = frame_addr - sizeof(uint64_t);
    if (!user_ptr_valid(frame_addr) || !user_ptr_valid(ret_addr_sp))
        return -EFAULT;

    // a kilobyte, too much for the 8k kernel stack. the fault path isnt always under the bkl
    linux_sigframe_t *f = kmalloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    memset(f, 0, sizeof(*f));
    ctx_to_gregs(ctx, f->uc.gregs);
    f->uc.gregs[G_OLDMASK] = task->sig_blocked;
    f->uc.gregs[G_CR2]     = (sig == SIGSEGV || sig == SIGBUS) ? addr : 0;
    f->uc.fpregs      = frame_addr + offsetof(linux_sigframe_t, uc.fpregs_mem);
    f->uc.sigmask[0]  = task->sig_blocked;
    f->uc.stack.sp    = task->sig_altstack_sp;
    f->uc.stack.size  = task->sig_altstack_size;
    f->uc.stack.flags = task->sig_altstack_size ? task->sig_altstack_flags : SS_DISABLE;
    f->info.signo = sig;
    f->info.code  = code;
    f->info.addr  = addr;
    f->prev_frame = task->sig_active_frame;

    uint64_t ret_addr = (uint64_t)task->sig_restorers[sig];
    int bad = copy_to_user(task, (void *)frame_addr, f, sizeof(*f)) != 0 ||
              copy_to_user(task, (void *)ret_addr_sp, &ret_addr, sizeof(ret_addr)) != 0;
    kfree(f);
    if (bad)
        return -EFAULT;

    uintptr_t handler = task->sig_handlers[sig];
    task->sig_active_frame = frame_addr | FRAME_LINUX;
    signal_enter_mask(task, sig, task->sig_masks[sig], task->sig_flags[sig]);

    ctx->rip = handler;
    ctx->rdi = (uint64_t)sig;
    ctx->rsi = frame_addr + offsetof(linux_sigframe_t, info);
    ctx->rdx = frame_addr;
    ctx->rax = 0;
    ctx->rsp = ret_addr_sp;
    return 0;
}

static int signal_return_linux_frame(task_t *task, cpu_context_t *ctx, uintptr_t frame_addr) {
    // only the registers and the mask come back, no need to read the whole frame
    uint64_t gregs[UC_NGREG];
    uint64_t mask, prev;
    if (copy_from_user(task, gregs, (const void *)(frame_addr + offsetof(linux_sigframe_t, uc.gregs)), sizeof(gregs)) ||
        copy_from_user(task, &mask, (const void *)(frame_addr + offsetof(linux_sigframe_t, uc.sigmask)), sizeof(mask)) ||
        copy_from_user(task, &prev, (const void *)(frame_addr + offsetof(linux_sigframe_t, prev_frame)), sizeof(prev)))
        return -EFAULT;

    cpu_context_t back = *ctx;
    gregs_to_ctx(gregs, &back);
    if (!user_ptr_valid(back.rip))
        return -EFAULT;

    back.cs     = USER_CS;
    back.ss     = USER_SS;
    back.rflags = (back.rflags & RFLAGS_USER_MASK) | RFLAGS_IF;

    *ctx = back;
    task->sig_blocked = mask & ~(sig_bit(SIGKILL) | sig_bit(SIGSTOP));
    task->sig_active_frame = prev;
    return 0;
}

static int signal_setup_user_handler(task_t *task, cpu_context_t *ctx, int sig, int code, uint64_t addr) {
    uintptr_t handler = task->sig_handlers[sig];
    uintptr_t restorer = task->sig_restorers[sig];
    sigset_t handler_mask = task->sig_masks[sig];
    uint64_t handler_flags = task->sig_flags[sig];

    if (handler == SIG_DFL || handler == SIG_IGN || restorer < 2)
        return -EINVAL;

    // SA_ONSTACK goes to the alt stack, how a stack overflow still gets its handler run.
    // otherwise whatever stack the task is on, copy_to_user still says no if its not mapped
    uintptr_t top = ctx->rsp - SIGNAL_RED_ZONE;
    if ((handler_flags & SA_ONSTACK) && !(task->sig_altstack_flags & SS_DISABLE) &&
        task->sig_altstack_size && !on_altstack(task, ctx->rsp))
        top = task->sig_altstack_sp + task->sig_altstack_size;
    else if (!user_ptr_valid(ctx->rsp))
        return -EFAULT;

    if (handler_flags & SA_SIGINFO)
        return signal_setup_linux_frame(task, ctx, sig, code, addr, top);

    uintptr_t frame_addr = (top - sizeof(sigframe_t)) & ~0xFULL;
    if (!user_ptr_valid(frame_addr))
        return -EFAULT;

    sigframe_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.saved_ctx = *ctx;
    frame.old_mask = task->sig_blocked;
    frame.prev_frame = task->sig_active_frame;
    frame.info.signo = sig;
    frame.info.code  = code;
    frame.info.addr  = addr;

    if (copy_to_user(task, (void *)frame_addr, &frame, sizeof(frame)) != 0)
        return -EFAULT;

    uintptr_t ret_addr_sp = frame_addr - sizeof(uint64_t);
    if (!user_ptr_valid(ret_addr_sp))
        return -EFAULT;

    uint64_t ret_addr = (uint64_t)restorer;
    if (copy_to_user(task, (void *)ret_addr_sp, &ret_addr, sizeof(ret_addr)) != 0)
        return -EFAULT;

    task->sig_active_frame = frame_addr;
    signal_enter_mask(task, sig, handler_mask, handler_flags);

    // rsi is the frame so a handler can read and change the context it came from
    ctx->rip = handler;
    ctx->rdi = (uint64_t)sig;
    ctx->rsi = frame_addr;
    ctx->rsp = ret_addr_sp;
    return 0;
}

int signal_deliver_fault(task_t *task, cpu_context_t *ctx, int sig, int code, uint64_t addr) {
    if (!task || !ctx || !valid_signal(sig))
        return -EINVAL;

    // a fault inside the handler, or with it blocked, would only fault again
    uintptr_t handler = task->sig_handlers[sig];
    if (handler == SIG_DFL || handler == SIG_IGN)
        return -EINVAL;
    if (task->sig_active_frame || (task->sig_blocked & sig_bit(sig)))
        return -EINVAL;

    return signal_setup_user_handler(task, ctx, sig, code, addr);
}

int signal_set_altstack(task_t *task, const user_stack_t *ss, user_stack_t *old, uintptr_t sp) {
    if (old) {
        old->sp    = task->sig_altstack_sp;
        old->size  = task->sig_altstack_size;
        old->flags = task->sig_altstack_size ? task->sig_altstack_flags : SS_DISABLE;
        if (on_altstack(task, sp))
            old->flags = SS_ONSTACK;
        old->pad = 0;
    }
    if (!ss)
        return 0;

    if (on_altstack(task, sp))
        return -EPERM;
    if (ss->flags & ~(SS_DISABLE | SS_ONSTACK))
        return -EINVAL;
    if (ss->flags & SS_DISABLE) {
        task->sig_altstack_sp    = 0;
        task->sig_altstack_size  = 0;
        task->sig_altstack_flags = SS_DISABLE;
        return 0;
    }
    if (ss->size < MINSIGSTKSZ)
        return -ENOMEM;
    if (!user_ptr_valid(ss->sp) || !user_ptr_valid(ss->sp + ss->size - 1))
        return -EFAULT;

    task->sig_altstack_sp    = ss->sp;
    task->sig_altstack_size  = ss->size;
    task->sig_altstack_flags = 0;
    return 0;
}

int signal_handle_sigreturn(task_t *task, cpu_context_t *ctx) {
    if (!task || !ctx || !task->sig_active_frame)
        return -EINVAL;

    if (task->sig_active_frame & FRAME_LINUX)
        return signal_return_linux_frame(task, ctx, task->sig_active_frame & ~FRAME_LINUX);

    sigframe_t frame;
    if (copy_from_user(task, &frame, (const void *)task->sig_active_frame, sizeof(frame)) != 0)
        return -EFAULT;

    // the frame lives in user memory so it cant be trusted, without this a task
    // could sigreturn itself straight into ring 0
    if (!user_ptr_valid(frame.saved_ctx.rip))
        return -EFAULT;

    frame.saved_ctx.cs     = USER_CS;
    frame.saved_ctx.ss     = USER_SS;
    frame.saved_ctx.rflags = (frame.saved_ctx.rflags & RFLAGS_USER_MASK) | RFLAGS_IF;

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

        // an ignored signal shouldnt hold up whatever is queued behind it
        int default_action = signal_apply_default_action(task, sig, bit);
        if (default_action == 0)
            continue;
        if (default_action != -1)
            return;

        if (signal_setup_user_handler(task, ctx, sig, SI_USER, 0) < 0) {
            task->exit_signal = sig;
            task->exit_code = 128 + sig;
            exit();
        }
        return;
    }
}

int signal_should_interrupt(task_t *task) {
    if (!task)
        return 0;

    sigset_t ready = task->sig_pending & ~task->sig_blocked;
    if (!ready)
        return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        sigset_t bit = sig_bit(sig);
        if (!(ready & bit))
            continue;

        uintptr_t handler = task->sig_handlers[sig];
        if (handler == SIG_IGN)
            continue;
        if (handler == SIG_DFL && signal_default_ignored(sig))
            continue;

        // a handler cant be entered while another is on the stack, so it cant interrupt anything yet
        if (handler != SIG_DFL && task->sig_active_frame)
            continue;

        // leave it pending when theres a handler, signal_deliver_pending still has to run it
        int default_action = signal_apply_default_action(task, sig, bit);
        if (default_action != -1)
            return default_action;

        return 1;
    }

    return 0;
}