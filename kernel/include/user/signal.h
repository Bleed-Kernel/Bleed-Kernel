#pragma once

#include <stdint.h>

#define NSIG 32

typedef uint64_t sigset_t;

typedef enum {
    SIGHUP  = 1,
    SIGINT  = 2,
    SIGQUIT = 3,
    SIGILL  = 4,
    SIGTRAP = 5,
    SIGABRT = 6,
    SIGBUS  = 7,
    SIGFPE  = 8,
    SIGKILL = 9,
    SIGUSR1 = 10,
    SIGSEGV = 11,
    SIGUSR2 = 12,
    SIGPIPE = 13,
    SIGALRM = 14,
    SIGTERM = 15,
    SIGCHLD = 17,
    SIGCONT = 18,
    SIGSTOP = 19,
    SIGTSTP = 20,
    SIGTTIN = 21,
    SIGTTOU = 22,
    SIGWINCH = 28
} signal_number_t;

#define SIG_DFL ((uintptr_t)0)
#define SIG_IGN ((uintptr_t)1)

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NODEFER  0x1
#define SA_RESTART  0x2

typedef struct sigaction {
    uintptr_t handler;
    sigset_t mask;
    uint64_t flags;
    uintptr_t restorer;
} sigaction_t;