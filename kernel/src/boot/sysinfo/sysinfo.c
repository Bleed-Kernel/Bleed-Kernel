#include <stdio.h>
#include <boot/bootloader_interface/generic_bootloader.h>
#include <fs/vfs.h>
#include <boot/sysinfo/sysinfo.h>
#include <mm/kalloc.h>
#include <ansii.h>

system_memory_info_t *get_system_memory_info(){
    if (g_gbi.memmap_entry_count == 0) kprintf(LOG_ERROR "No Memory Map from Limine, How did you get here?");
    system_memory_info_t *memory_table = kmalloc(sizeof(system_memory_info_t));
    if (!memory_table) return NULL;

    uint64_t usable = 0;
    uint64_t reserved = 0;
    uint64_t faulty = 0;

    for (size_t i = 0; i < g_gbi.memmap_entry_count; i++){
        gbi_memmap_entry_t *entry = &g_gbi.memmap[i];

        switch (entry->type){
            case GBI_MEMMAP_USABLE:
                usable += entry->length;
                break;
            case GBI_MEMMAP_ACPI_RECLAIMABLE:
            case GBI_MEMMAP_ACPI_NVS:
            case GBI_MEMMAP_KERNEL_AND_MODULES:
                reserved += entry->length;
                break;
            case GBI_MEMMAP_BAD_MEMORY:
                faulty += entry->length;
                break;
            default:
                break;
        }
    }

    memory_table->MEMORY_USABLE = usable;
    memory_table->MEMORY_RESERVED = reserved;
    memory_table->MEMORY_FAULTY = faulty;

    return memory_table;
}