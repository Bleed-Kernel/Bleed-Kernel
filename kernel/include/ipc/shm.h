#pragma once

#include <stdint.h>
#include <sched/scheduler.h>

#define SHM_MAX_OBJECTS 64
#define SHM_MAX_PAGES   65536   // 256MiB, far past any framebuffer

// make an object of zeroed pages owned by task
long shm_create(task_t *task, uint64_t pages);

// map an object into task, returns the address
long shm_map(task_t *task, uint64_t id, uint64_t user_pages_ptr);

// owner only. the id stops working, tasks that already mapped it keep their pages
long shm_destroy(task_t *task, uint64_t id);

// destroy everything task still owns
void shm_task_cleanup(task_t *task);
