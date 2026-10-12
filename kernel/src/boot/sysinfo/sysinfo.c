#include <stdio.h>
#include <string.h>
#include <boot/bootloader_interface/generic_bootloader.h>
#include <fs/vfs.h>
#include <boot/sysinfo/sysinfo.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <ansii.h>

system_memory_info_t *get_system_memory_info(){
    if (g_gbi.memmap_entry_count == 0) kprintf(LOG_ERROR "No Memory Map from Limine, How did you get here?");
    system_memory_info_t *memory_table = kmalloc(sizeof(system_memory_info_t));
    if (!memory_table) return NULL;

    memset(memory_table, 0, sizeof(*memory_table));

    uint64_t usable = 0;
    uint64_t reserved = 0;
    uint64_t faulty = 0;

    // where the ram stops. reserved ranges past here are holes for devices, theres no memory in them
    uint64_t ram_top = 0;
    for (size_t i = 0; i < g_gbi.memmap_entry_count; i++){
        gbi_memmap_entry_t *entry = &g_gbi.memmap[i];
        if (entry->type == GBI_MEMMAP_USABLE && entry->base + entry->length > ram_top)
            ram_top = entry->base + entry->length;
    }

    for (size_t i = 0; i < g_gbi.memmap_entry_count; i++){
        gbi_memmap_entry_t *entry = &g_gbi.memmap[i];

        switch (entry->type){
            case GBI_MEMMAP_USABLE:
                usable += entry->length;
                break;
            case GBI_MEMMAP_ACPI_RECLAIMABLE:
            case GBI_MEMMAP_ACPI_NVS:
                reserved += entry->length;
                memory_table->MEMORY_ACPI += entry->length;
                break;
            case GBI_MEMMAP_KERNEL_AND_MODULES:
                reserved += entry->length;
                memory_table->MEMORY_KERNEL += entry->length;
                break;
            case GBI_MEMMAP_BAD_MEMORY:
                faulty += entry->length;
                break;
            case GBI_MEMMAP_BOOTLOADER_RECLAIMABLE:
                memory_table->MEMORY_BOOTLOADER += entry->length;
                break;
            case GBI_MEMMAP_FRAMEBUFFER:
                memory_table->MEMORY_FRAMEBUFFER += entry->length;
                break;
            case GBI_MEMMAP_RESERVED:
                if (entry->base < ram_top)
                    memory_table->MEMORY_OTHER += entry->length;
                break;
            default:
                break;
        }
    }

    memory_table->MEMORY_USABLE = usable;
    memory_table->MEMORY_RESERVED = reserved;
    memory_table->MEMORY_FAULTY = faulty;
    memory_table->MEMORY_FREE = (uint64_t)pmm_available_pages() * PAGE_SIZE;

    return memory_table;
}