#pragma once

#include <stdint.h>
#include <stdbool.h>

// brents cycle check for cluster chains off a disk, a looped fat stops within a couple of laps
typedef struct {
    uint32_t mark;
    uint32_t power;
    uint32_t len;
} chain_guard_t;

static inline void chain_guard_init(chain_guard_t *g, uint32_t first) {
    g->mark  = first;
    g->power = 1;
    g->len   = 0;
}

// false once next is somewhere the chain has already been
static inline bool chain_guard_step(chain_guard_t *g, uint32_t next) {
    if (next == g->mark)
        return false;
    if (++g->len == g->power) {
        g->mark  = next;
        g->power = g->power < 0x80000000u ? g->power * 2 : g->power;
        g->len   = 0;
    }
    return true;
}
