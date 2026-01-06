#include <stdint.h>
#include <stdio.h>
#include <sched/scheduler.h>
#include <panic.h>
#include <ansii.h>
#include <threads/exit.h>
#include <gdt/gdt.h>
#include <drivers/serial/serial.h>

#define SCHED_TEST_TASKS 4
#define TASK_ITERATIONS 25

static volatile int task_counters[SCHED_TEST_TASKS];
static volatile int next_counter_index = 0;

static void sched_test_task() {
    int my_index = __sync_fetch_and_add(&next_counter_index, 1);
    if (my_index >= SCHED_TEST_TASKS + 2) { // we need to offset by 2 because we have 2 reseved ids for kernel and reaper
        ke_panic("Too many test tasks created");
    }

    for (int i = 0; i < TASK_ITERATIONS; i++) {
        task_counters[my_index]++;
        sched_yield();
    }

    exit();
}

void scheduler_test_self_test(void) {
    kprintf(LOG_INFO "Scheduler Self Test begin\n");

    for (size_t i = 0; i < SCHED_TEST_TASKS; i++)
        task_counters[i] = 0;

    for (size_t i = 0; i < SCHED_TEST_TASKS; i++) {
        uint64_t id = 0;

        // avoid reserved IDs
        while (id == 0 || id == 1)
            id = sched_create_task(kernel_page_map, (uint64_t)sched_test_task, KERNEL_CS, KERNEL_SS)->id;

        kprintf(LOG_INFO "Created test task %lu with ID %llu\n", i, id);
    }

    while (1) {
        int total = 0;
        for (size_t i = 0; i < SCHED_TEST_TASKS; i++)
            total += task_counters[i];

        if (total >= TASK_ITERATIONS * SCHED_TEST_TASKS)
            break;

        sched_yield();
    }

    // verify it went well
    for (size_t i = 0; i < SCHED_TEST_TASKS; i++) {
        if (task_counters[i] != TASK_ITERATIONS){
            kprintf("Task %zu did not complete (counter=%d)\n", i, task_counters[i]);
            ke_panic("Scheduler Failure in Boot Test");
        }
        kprintf(LOG_INFO "Task %lu counter=%d\n", i, task_counters[i]);
    }

    kprintf(LOG_OK "Scheduler Test Passed\n");
}
