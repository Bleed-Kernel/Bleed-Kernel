#include <stdio.h>
#include <vendor/limine_bootloader/limine.h>
#include <fs/vfs.h>
#include <boot/sysinfo/sysinfo.h>
#include <mm/kalloc.h>
#include <ansii.h>

extern volatile struct limine_memmap_request memmap_request;

system_memory_info_t *get_system_memory_info(){
    if (!memmap_request.response) kprintf(LOG_ERROR "No Memory Map from Limine, How did you get here?");
    system_memory_info_t *memory_table = kmalloc(sizeof(system_memory_info_t));
    if (!memory_table) return NULL;

    uint64_t usable = 0;
    uint64_t reserved = 0;
    uint64_t faulty = 0;
    
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++){
        struct limine_memmap_entry* entry = memmap_request.response->entries[i];

        switch (entry->type){
            case LIMINE_MEMMAP_USABLE:
                usable += entry->length;
                break;
            case LIMINE_MEMMAP_ACPI_TABLES:
            case LIMINE_MEMMAP_ACPI_NVS:
            case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
                reserved += entry->length;
                break;
            case LIMINE_MEMMAP_BAD_MEMORY:
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