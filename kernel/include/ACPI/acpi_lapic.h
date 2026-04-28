#pragma once

#include <stdint.h>

#define LAPIC_SIZE            0x1000
#define LAPIC_VIRT            0xFFFFFFFFFFE00000ULL

#define IA32_APIC_BASE_MSR    0x1B
#define IA32_APIC_BASE_ENABLE (1ULL << 11)

#define LAPIC_REG_ID          0x020
#define LAPIC_REG_EOI         0x0B0
#define LAPIC_REG_SVR         0x0F0
#define LAPIC_REG_TPR         0x080
#define LAPIC_REG_DFR         0x0E0
#define LAPIC_REG_LDR         0x0D0
#define LAPIC_REG_LVT_TIMER   0x320
#define LAPIC_REG_LVT_THERMAL 0x330
#define LAPIC_REG_LVT_PMC     0x340
#define LAPIC_REG_LVT_LINT0   0x350
#define LAPIC_REG_LVT_LINT1   0x360
#define LAPIC_REG_LVT_ERROR   0x370

#define LAPIC_REG_TIMER_INIT_CNT 0x380
#define LAPIC_REG_TIMER_CURR_CNT 0x390
#define LAPIC_REG_TIMER_DIV      0x3E0

#define TIMER_PERIODIC           0x20000
#define TIMER_DIVIDE_16          0x03

#define APIC_LVT_MASKED       (1U << 16)
#define APIC_SVR_ENABLE       (1U << 8)

int lapic_init(void);
uint32_t lapic_get_id(void);