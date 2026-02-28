#pragma once

#include <sched/scheduler.h>
#include <user/signal.h>

int signal_send(task_t *task, int sig);
int signal_send_pid(uint64_t pid, int sig);

int signal_set_action(task_t *task, int sig, const sigaction_t *act, sigaction_t *old);
int signal_set_mask(task_t *task, int how, const sigset_t *set, sigset_t *old);

int signal_handle_sigreturn(task_t *task, cpu_context_t *ctx);
void signal_deliver_pending(task_t *task, cpu_context_t *ctx);
int signal_should_interrupt(task_t *task);