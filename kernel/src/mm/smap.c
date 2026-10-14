#include <stdint.h>
#include <stdbool.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <cpu/control_registers.h>
#include <cpu/cpuid.h>
#include <mm/smap.h>

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

    int missing = 0;
    if (!has_smap) missing += SMAP_MISSING;
    if (!has_smep) missing += SMEP_MISSING;

    switch (missing){
        case SMAP_MISSING:
            serial_printf(LOG_WARN "SMAP not Supported\n");
            break;
        case SMEP_MISSING:
            serial_printf(LOG_WARN "SMEP not Supported\n");
            break;
        case BOTH_MISSING:
            serial_printf(LOG_WARN "SMAP and SMEP are not Supported\n");
            break;
        case FULLY_SUPPORTED:
            serial_printf(LOG_OK "SMAP Enabled\n");
            serial_printf(LOG_OK "SMEP Enabled\n");
            break;
        default:
            serial_printf(LOG_WARN "SMAP/SMEP status is Unknown? Reported (%i)\n", missing);
        }

    return missing;
}