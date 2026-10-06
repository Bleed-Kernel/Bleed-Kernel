#pragma once
#include <stdint.h>

struct task;

// work out how big the save area is and build the clean state, call after simd_enable
void fpu_init(void);

// give a task its own save area holding a clean state, 0 on success
int  fpu_task_init(struct task *task);
void fpu_task_free(struct task *task);

// back to a clean state, for exec
void fpu_task_reset(struct task *task);

// child starts with whatever the parent has right now, for fork
void fpu_task_copy(struct task *dst, struct task *src);

// called on every context switch, arms the trap unless next already owns the registers
void fpu_switch(struct task *next);

// #NM handler, returns 1 when the fault was ours and has been dealt with
int  fpu_handle_nm(void);
