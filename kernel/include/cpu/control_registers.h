#pragma once
#include <stdint.h>

static inline uint64_t read_cr4(void) {
    uint64_t val;
    asm volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val) {
    asm volatile ("mov %0, %%cr4" :: "r"(val));
}

static inline uint64_t read_cr3(){
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    return cr3;
}

static inline void write_cr3(uint64_t cr3){
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

static inline uint64_t read_cr0(void){
    uint64_t val;
    asm volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val){
    asm volatile("mov %0, %%cr0" :: "r"(val) : "memory");
}

static inline void cr0_set_bits(uint64_t mask){
    uint64_t cr0 = read_cr0();
    cr0 |= mask;
    write_cr0(cr0);
}

static inline void cr0_clear_bits(uint64_t mask){
    uint64_t cr0 = read_cr0();
    cr0 &= ~mask;
    write_cr0(cr0);
}