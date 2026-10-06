#include <stdint.h>
#include <stdbool.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <cpu/control_registers.h>
#include <cpu/cpuid.h>

#define CPUID_LEAF_BASIC_MAX     0
#define CPUID_LEAF_EXT_FEATURES  7

#define CPUID_7_EBX_SMEP (1u << 7)
#define CPUID_7_EBX_SMAP (1u << 20)

#define CPU_CR4_SMEP_BIT (1ULL << 20)
#define CPU_CR4_SMAP_BIT (1ULL << 21)

int smap_supported = 0;

int SMAP_init(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t features = 0;

    cpuid(CPUID_LEAF_BASIC_MAX, &eax, &ebx, &ecx, &edx);
    if (eax >= CPUID_LEAF_EXT_FEATURES) {
        cpuid_count(CPUID_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
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