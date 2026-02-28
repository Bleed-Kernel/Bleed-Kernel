#pragma once
#include <stdint.h>

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

static inline unsigned long irq_push(void){
    unsigned long flags;
    asm volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

static inline void irq_restore(unsigned long flags){
    asm volatile(
        "push %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static inline void spinlock_init(spinlock_t *lock) {
    lock->lock = 0;
}

static inline void spinlock_acquire(spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static inline void spinlock_release(spinlock_t *lock) {
    __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
}
