#pragma once
#include <cpu/control_registers.h>
#include <cpu/msrs.h>

// User Mode Instruction Protection
#define CR4_UMIP (1ULL << 11)

// No Execute Bit
#define MSR_EFER 0xC0000080
#define EFER_NXE (1ULL << 11)

// Write Protect
#define CR0_WP (1ULL << 16)

static inline void UMIP_init(void){
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_UMIP;
    write_cr4(cr4);
}

static inline void nx_init(void){
    /*
        As of right now i cant confirm this works because
        i cant get it to crash but better being here than not
    */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
}

static inline void wp_enable(void){
    uint64_t cr0 = read_cr0();
    cr0 |= CR0_WP;
    write_cr0(cr0);
}