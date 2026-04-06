#include <fs/partition.h>
#include <mm/kalloc.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

void partition_probe(void *drive_obj, int drive_idx, sector_reader_t reader, part_register_cb_t reg_cb) {
    uint8_t sector[512];
    if (reader(drive_obj, 0, 1, sector) < 0) return;

    part_mbr_t *mbr = (part_mbr_t *)sector;
    if (mbr->signature != PART_MBR_SIGNATURE) return;

    bool is_gpt = false;
    for (int i = 0; i < 4; i++) {
        if (mbr->partitions[i].type == 0xEE) {
            is_gpt = true;
            break;
        }
    }

    if (is_gpt) {
        serial_printf(LOG_OK "Current Drive identified as GPT\n");
        part_gpt_header_t gpt_hdr;
        
        // Read LBA 1 header
        if (reader(drive_obj, 1, 1, &gpt_hdr) < 0) return;
        if (memcmp(gpt_hdr.signature, "EFI PART", 8) != 0) return; // Invalid GPT signature

        size_t entry_size = gpt_hdr.entry_size;
        uint32_t sectors_to_read = (gpt_hdr.entry_count * entry_size + 511) / 512;
        
        uint8_t *table = kmalloc(sectors_to_read * 512);
        if (!table) return;

        // Read the Partition Entry Array
        if (reader(drive_obj, gpt_hdr.partition_array_lba, sectors_to_read, table) < 0) {
            kfree(table, sectors_to_read * 512);
            return;
        }

        for (uint32_t i = 0; i < gpt_hdr.entry_count; i++) {
            part_gpt_entry_t *e = (part_gpt_entry_t *)(table + (i * entry_size));
            
            bool empty = true;
            for (int j = 0; j < 16; j++) {
                if (e->type_guid[j] != 0) { empty = false; break; }
            }
            if (empty) continue;

            uint64_t size = e->last_lba - e->first_lba + 1;
            
            reg_cb(drive_obj, drive_idx, i, e->first_lba, size, 0xEE);
        }
        kfree(table, sectors_to_read * 512);

    } else {
        serial_printf(LOG_INFO "Current Drive identified as MBR\n");
        for (int i = 0; i < 4; i++) {
            if (mbr->partitions[i].type == 0 || mbr->partitions[i].sector_count == 0) continue;
            
            reg_cb(drive_obj, drive_idx, i, 
                   mbr->partitions[i].lba_start, 
                   mbr->partitions[i].sector_count, 
                   mbr->partitions[i].type);
        }
    }
}