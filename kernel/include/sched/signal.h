#pragma once

#include <sched/scheduler.h>
#include <user/signal.h>

int signal_send(task_t *task, int sig);
int signal_send_pid(uint64_t pid, int sig);
int signal_send_group(task_t *task, int sig);

int signal_set_action(task_t *task, int sig, const sigaction_t *act, sigaction_t *old);
int signal_set_mask(task_t *task, int how, const sigset_t *set, sigset_t *old);

int signal_handle_sigreturn(task_t *task, cpu_context_t *ctx);

int signal_deliver_fault(task_t *task, cpu_context_t *ctx, int sig, int code, uint64_t addr);
int signal_set_altstack(task_t *task, const user_stack_t *ss, user_stack_t *old, uintptr_t sp);
void signal_deliver_pending(task_t *task, cpu_context_t *ctx);
int signal_should_interrupt(task_t *task);