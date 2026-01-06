#include <sched/scheduler.h>
#include <panic.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

__attribute__((noreturn))
void exit(void) {
    task_t *current_task = get_current_task();
    task_t *w = current_task->wait_queue;

    while (w){
        task_t *next = w->wait_next;

        w->wait_next = NULL;
        w->state = TASK_READY;
        w = next;
    }

    current_task->wait_queue = NULL;

    sched_mark_task_dead();
    sched_yield();

    serial_printf(
        "%sTask %d has exited, marking as dead for reaping\n",
        LOG_INFO,
        current_task->id
    );

    for (;;) { asm volatile ("hlt"); }
}