#pragma once

void supervisor_memory_protection_init(void);

static inline void stac(void){
    asm volatile("stac" ::: "cc");
}

static inline void clac(void){
    asm volatile("clac" ::: "cc");
}