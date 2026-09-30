#include <stdint.h>
#include <stdbool.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <cpu/control_registers.h>

#define CPUID_LEAF_BASIC_MAX     0
#define CPUID_LEAF_EXT_FEATURES  7

#define CPUID_7_EBX_SMEP (1u << 7)
#define CPUID_7_EBX_SMAP (1u << 20)

#define CPU_CR4_SMEP_BIT (1ULL << 20)
#define CPU_CR4_SMAP_BIT (1ULL << 21)

int smap_supported = 0;

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

int SMAP_init(void) {
    uint32_t eax, ebx, ecx, edx = 0;
    uint32_t features = 0;

    cpuid(CPUID_LEAF_BASIC_MAX, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= CPUID_LEAF_EXT_FEATURES) {
        cpuid(CPUID_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
        features = ebx;
    }

    bool has_smep = (features & CPUID_7_EBX_SMEP) != 0;
    bool has_smap = (features & CPUID_7_EBX_SMAP) != 0;

    uint64_t cr4 = read_cr4();
    if (has_smep)
        cr4 |= CPU_CR4_SMEP_BIT;
    if (has_smap)
        cr4 |= CPU_CR4_SMAP_BIT;
    write_cr4(cr4);

    smap_supported = has_smap;

    if (has_smep)
        serial_printf(LOG_OK "SMEP Enabled\n");
    else
        serial_printf(LOG_ERROR "SMEP Not Supported\n");

    if (has_smap)
        serial_printf(LOG_OK "SMAP Enabled\n");
    else
        serial_printf(LOG_ERROR "SMAP Not Supported\n");

    int missing = (has_smep ? 0 : 1) | (has_smap ? 0 : 2);
    return -missing;
}