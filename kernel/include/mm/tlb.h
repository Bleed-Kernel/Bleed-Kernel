#pragma once
#include <mm/paging.h>

// make every other cpu running in cr3 drop its tlb, call after changing or removing present ptes
void tlb_shootdown(paddr_t cr3);

// reload cr3 if another cpu asked us to, interrupts must be off
void tlb_flush_check(void);
