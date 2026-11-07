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

// linux values
#define SA_SIGINFO   0x4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048

// what sigaltstack takes, linuxs stack_t
typedef struct user_stack {
    uintptr_t sp;
    int32_t   flags;
    uint32_t  pad;
    uint64_t  size;
} user_stack_t;

#define SI_USER     0
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

// the start of linuxs siginfo_t, padded out to its 128 bytes
typedef struct user_siginfo {
    int32_t  signo;
    int32_t  err;
    int32_t  code;
    int32_t  pad;
    uint64_t addr;      // si_addr for faults, si_pid + si_uid for the rest
    uint64_t rest[13];
} user_siginfo_t;

// linuxs ucontext_t for x86_64, what SA_SIGINFO handlers get in rdx.
// gregs go r8..r15 rdi rsi rbp rbx rdx rax rcx rsp rip eflags csgsfs err trapno oldmask cr2

// MUSL and GLIBC agree on this one
#define UC_NGREG 23
typedef struct user_ucontext {
    uint64_t     flags;
    uint64_t     link;
    user_stack_t stack;
    uint64_t     gregs[UC_NGREG];
    uint64_t     fpregs;
    uint64_t     reserved1[8];
    uint64_t     sigmask[16]; //only the first word means anything
    uint8_t      fpregs_mem[512];
} user_ucontext_t;

typedef struct sigaction {
    uintptr_t handler;
    sigset_t mask;
    uint64_t flags;
    uintptr_t restorer;
} sigaction_t;