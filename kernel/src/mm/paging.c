#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <drivers/framebuffer/framebuffer.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <cpu/io.h>
#include <ansii.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/serial/serial.h>
#include <kernel/exception/panic.h>
#include <sched/scheduler.h>
#include <cpu/features/features.h>
#include <cpu/msrs.h>
#include <mm/cow.h>

paddr_t kernel_page_map = 0;

extern volatile struct limine_memmap_request memmap_request;

void pat_enable_wc(void) {
    uint64_t pat = rdmsr(0x277);

    pat &= ~(0xFFULL << 32);
    pat |=  (0x01ULL << 32);

    wrmsr(0x277, pat);

    asm volatile("mov %%cr3, %%rax\n"
                "mov %%rax, %%cr3\n"
                ::: "rax", "memory");
}

/// @brief allocate an empty page frame and return the paddr
/// @param vaddr out virtual address
/// @return physical address
uint64_t paging_alloc_empty_frame(void **vaddr) {
    if (vaddr) *vaddr = NULL;

    // paddr 0 still has a valid hhdm address, bail before we zero it and hand it out as a table
    paddr_t paddr = pmm_alloc_pages(1);
    if (!paddr) {
        kprintf(LOG_ERROR "Page Allocation Failed\n");
        return 0;
    }

    void *v = paddr_to_vaddr(paddr);
    memset(v, 0, PAGE_SIZE_4K);
    if (vaddr) *vaddr = v;
    return paddr;
}

/// @brief write a page table at a given index, if it already exsists, we return its paddr
/// @param table Pointer to target table
/// @param index Index of entry to modify
/// @param flags PTE Flags
/// @return paddr table[index]
static uint64_t paging_write_table_entry(uint64_t* table, size_t index, uint64_t flags) {
    uint64_t entry = table[index];
    if (entry & PTE_PRESENT) {
        // huge page not a table, walking into it reads whatever is at that paddr
        if (entry & PTE_PAT_4K) {
            serial_printf(LOG_ERROR "paging: refusing to walk through a huge page entry\n");
            return 0;
        }
        return entry & PADDR_ENTRY_MASK;
    }

    void *v = NULL;
    uint64_t p = paging_alloc_empty_frame(&v);
    if (!v) return 0;

    table[index] = (p & PADDR_ENTRY_MASK) | flags | PTE_PRESENT;
    return p & PADDR_ENTRY_MASK;
}

/// @brief walk page tables at PD level for a vaddr
/// @param vaddr vaddr to resolve
/// @param out_pd page directory pointer to resolved vaddr
/// @param out_pd_index index of the output page directory for vaddr
void paging_walk_page_tables(paddr_t cr3, uint64_t vaddr,
                             uint64_t **out_pd, size_t *out_pd_index,
                             uint64_t flags) {
    if ((cr3 & PADDR_ENTRY_MASK) == 0) {
        *out_pd = NULL;
        *out_pd_index = 0;
        return;
    }

    uint64_t *pml4 = paddr_to_vaddr(cr3 & PADDR_ENTRY_MASK);
    if (!pml4) {
        *out_pd = NULL;
        *out_pd_index = 0;
        return;
    }

    size_t pml4i = (vaddr >> 39) & 0x1FF;
    size_t pdpti = (vaddr >> 30) & 0x1FF;
    size_t pdi   = (vaddr >> 21) & 0x1FF;
    size_t pti   = (vaddr >> 12) & 0x1FF;

    uint64_t p_pdpt = paging_write_table_entry(
        pml4, pml4i, PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER));
    if (!p_pdpt) {
        *out_pd = NULL;
        *out_pd_index = 0;
        return;
    }
    uint64_t *pdpt = paddr_to_vaddr(p_pdpt);

    uint64_t p_pd = paging_write_table_entry(
        pdpt, pdpti, PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER));
    if (!p_pd) {
        *out_pd = NULL;
        *out_pd_index = 0;
        return;
    }
    uint64_t *pd = paddr_to_vaddr(p_pd);

    if (!(flags & PTE_PAT_4K)) {
        uint64_t p_pt = paging_write_table_entry(
            pd, pdi, PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER));
        if (!p_pt) {
            *out_pd = NULL;
            *out_pd_index = 0;
            return;
        }
        uint64_t *pt = paddr_to_vaddr(p_pt);
        *out_pd = pt;
        *out_pd_index = pti;
    } else {
        *out_pd = pd;
        *out_pd_index = pdi;
    }
}

/// @brief map a physical page at a vaddr using a pd entry and can invalidate the TLB
/// @param paddr physical address to map the page frame at
/// @param vaddr virtual address to map the page at
/// @param flags PTE Flags
void paging_map_page_invl(paddr_t cr3, uint64_t paddr, uint64_t vaddr, uint64_t flags, int invalidate_tlb) {
    uint64_t *pd;
    size_t idx;

    paging_walk_page_tables(cr3, vaddr, &pd, &idx,
        flags & (PTE_USER | PTE_PAT_4K));

    if (!pd) return;

    uint64_t old_entry = pd[idx];
    pd[idx] = (paddr & PADDR_ENTRY_MASK)
            | flags
            | PTE_PRESENT;

    if (invalidate_tlb && (old_entry & PTE_PRESENT))
        asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/// @brief map a physical page at a vaddr using a pd entry
/// @param paddr physical address to map the page frame at
/// @param vaddr virtual address to map the page at
/// @param flags PTE Flags
void paging_map_page(paddr_t cr3, uint64_t paddr, uint64_t vaddr, uint64_t flags) {
    paging_map_page_invl(cr3, paddr, vaddr, flags, 1);
}

/// @brief map a 4K page as write-combining (PAT entry 4, see pat_enable_wc)
/// @param paddr physical address to map the page frame at
/// @param vaddr virtual address to map the page at
/// @param flags PTE Flags
void paging_map_page_wc(paddr_t cr3, uint64_t paddr, uint64_t vaddr, uint64_t flags) {
    uint64_t *pt;
    size_t idx;

    // PAT shares bit 7 with PTE_PAT_4K so this cant go through paging_map_page
    paging_walk_page_tables(cr3, vaddr, &pt, &idx, flags & PTE_USER);
    if (!pt) return;

    pt[idx] = (paddr & PADDR_ENTRY_MASK)
            | (flags & ~(PTE_PAT_4K | PTE_PCD | PTE_PWT))
            | PTE_PAT_4K
            | PTE_PRESENT;
}

/// @brief create the kernels page map and save it, key part of multitasking
void paging_init_kernel_map(void) {
    kernel_page_map = read_cr3() & PADDR_ENTRY_MASK;
    if (!paddr_to_vaddr(kernel_page_map))
        ke_panic(NULL, "Kernel PML4 unmapped");
}

/// @brief reinitalise paging so we can access a full memory range, not just the
/// default from limine
void init_paging(void) {
    nx_init();
    pat_enable_wc();
    
    // kernels view of the wc framebuffer
    uintptr_t fb_virt = (uintptr_t)framebuffer_get_addr(0);
    if (fb_virt) {
        paddr_t fb_phys = vaddr_to_paddr((void *)fb_virt);
        size_t  fb_size = framebuffer_get_height(0) * framebuffer_get_pitch(0);

        paddr_t fb_phys_base = fb_phys & ~(paddr_t)(PAGE_SIZE_4K - 1);
        size_t  fb_offset    = fb_phys - fb_phys_base;
        paddr_t cr3 = read_cr3();

        for (size_t off = 0; off < fb_offset + fb_size; off += PAGE_SIZE_4K)
            paging_map_page_wc(cr3, fb_phys_base + off, FB_KERNEL_VIRT + off,
                               PTE_WRITABLE | PTE_NX);

        g_gbi.framebuffer.phys_address = fb_phys;
        g_gbi.framebuffer.address      = FB_KERNEL_VIRT + fb_offset;
    }

    paging_init_kernel_map();
    wp_enable();
}

/// @brief create a new address space that shares the kernels higher half
/// @return paddr of the new pml4, 0 on failure
paddr_t paging_create_address_space(void){
    void* vaddr = NULL;
    paddr_t pml4_paddr = paging_alloc_empty_frame(&vaddr);

    if (!pml4_paddr) {
        serial_printf(LOG_ERROR "Failed to allocate PML4\n");
        return 0;
    }

    uint64_t *kernel_pml4 = (uint64_t *)paddr_to_vaddr(kernel_page_map);
    uint64_t *new_pml4 = (uint64_t *)vaddr;

    for (size_t i = 256; i < 512; i++){
        new_pml4[i] = kernel_pml4[i];
    }
    
    return pml4_paddr;
}

void paging_unmap_page(paddr_t cr3, uint64_t vaddr) {
    uint64_t *pte = paging_get_page(cr3, vaddr, 0);
    if (!pte)
        return;

    if (!(*pte & PTE_PRESENT))
        return;

    *pte = 0;
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/// @brief switch the current CR3 address space context
/// @param cr3 cr3 paddr
void paging_switch_address_space(paddr_t cr3){
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/// @brief free address space CR3 provided
/// @param cr3 target
void paging_destroy_address_space(paddr_t cr3){
    if (!cr3 || (cr3 & PADDR_ENTRY_MASK) == kernel_page_map) return;
    paging_release_user_space(cr3);
    pmm_free_pages(cr3, 1);
}

// step one level down the tables, filling in a missing table when asked to
static uint64_t *paging_next_table(uint64_t *table, size_t index, int create) {
    uint64_t entry = table[index];
    if (!(entry & PTE_PRESENT)) {
        if (!create) return NULL;
        paddr_t paddr = paging_alloc_empty_frame(NULL);
        if (!paddr) return NULL;
        entry = paddr | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        table[index] = entry;
    }

    // huge page not a table, theres no pte underneath it to hand back
    if (entry & PTE_PAT_4K) return NULL;

    return (uint64_t *)paddr_to_vaddr(entry & PADDR_ENTRY_MASK);
}

uint64_t* paging_get_page(paddr_t cr3, uint64_t vaddr, int create) {
    if ((cr3 & PADDR_ENTRY_MASK) == 0) return NULL;

    uint64_t *table = (uint64_t*)paddr_to_vaddr(cr3 & PADDR_ENTRY_MASK);

    // pml4 -> pdpt -> pd -> pt
    for (int shift = 39; shift > 12; shift -= 9) {
        table = paging_next_table(table, (vaddr >> shift) & 0x1FF, create);
        if (!table) return NULL;
    }

    size_t pt_index = (vaddr >> 12) & 0x1FF;
    if (!(table[pt_index] & PTE_PRESENT)) {
        if (!create) return NULL;
        paddr_t paddr = paging_alloc_empty_frame(NULL);
        if (!paddr) return NULL;
        table[pt_index] = paddr | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    return &table[pt_index];
}

// is vaddr backed by anything at all, a 4K page or a huge one further up
static int paging_is_mapped(paddr_t cr3, uint64_t vaddr) {
    uint64_t *table = (uint64_t *)paddr_to_vaddr(cr3 & PADDR_ENTRY_MASK);

    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t entry = table[(vaddr >> shift) & 0x1FF];
        if (!(entry & PTE_PRESENT)) return 0;
        if (shift == 12 || (entry & PTE_PAT_4K)) return 1;
        table = (uint64_t *)paddr_to_vaddr(entry & PADDR_ENTRY_MASK);
    }
    return 0;
}

/// @brief make sure a physical mmio range can be reached through the hhdm
/// @param phys start of the range, doesnt need to be page aligned
/// @param size length of the range in bytes
/// @return 0 on success, -1 if a page couldnt be mapped
int paging_map_mmio(paddr_t phys, size_t size) {
    paddr_t cr3   = read_cr3();
    paddr_t start = PAGE_ALIGN_DOWN(phys);
    paddr_t end   = PAGE_ALIGN_UP(phys + size);

    for (paddr_t p = start; p < end; p += PAGE_SIZE_4K) {
        uint64_t virt = (uint64_t)paddr_to_vaddr(p);

        // limine tends to cover this already with a huge page, theres no pte to edit then
        // and walking into it as if it were a table writes over whatever it points at
        if (paging_is_mapped(cr3, virt))
            continue;

        paging_map_page(cr3, p, virt, PTE_WRITABLE | PTE_NX);
        if (!paging_is_mapped(cr3, virt))
            return -1;
    }
    return 0;
}

int paging_clone_user_space(paddr_t parent_cr3, paddr_t child_cr3) {
    uint64_t *parent_pml4 = (uint64_t *)paddr_to_vaddr(parent_cr3 & PADDR_ENTRY_MASK);
    if (!parent_pml4 || !child_cr3)
        return -1;

    for (size_t pml4i = 0; pml4i < 256; pml4i++) {
        if (!(parent_pml4[pml4i] & PTE_PRESENT))
            continue;

        uint64_t *pdpt = (uint64_t *)paddr_to_vaddr(parent_pml4[pml4i] & PADDR_ENTRY_MASK);
        if (!pdpt)
            return -1;

        for (size_t pdpti = 0; pdpti < 512; pdpti++) {
            if (!(pdpt[pdpti] & PTE_PRESENT))
                continue;
            if (pdpt[pdpti] & PTE_PAT_4K)
                return -1;

            uint64_t *pd = (uint64_t *)paddr_to_vaddr(pdpt[pdpti] & PADDR_ENTRY_MASK);
            if (!pd)
                return -1;

            for (size_t pdi = 0; pdi < 512; pdi++) {
                if (!(pd[pdi] & PTE_PRESENT))
                    continue;
                if (pd[pdi] & PTE_PAT_4K)
                    return -1;

                uint64_t *pt = (uint64_t *)paddr_to_vaddr(pd[pdi] & PADDR_ENTRY_MASK);
                if (!pt)
                    return -1;

                for (size_t pti = 0; pti < 512; pti++) {
                    uint64_t pte = pt[pti];
                    if (!(pte & PTE_PRESENT))
                        continue;
                    if (!(pte & PTE_USER))
                        continue;

                    uint64_t vaddr = ((uint64_t)pml4i << 39)
                                   | ((uint64_t)pdpti << 30)
                                   | ((uint64_t)pdi << 21)
                                   | ((uint64_t)pti << 12);
                    paddr_t phys = pte & PADDR_ENTRY_MASK;
                    uint64_t flags = (pte & ~PADDR_ENTRY_MASK) & ~PTE_PRESENT;

                    // mmio like the framebuffer isnt ram, parent and child both keep pointing at the device.
                    // written raw because the PAT bit would read as a huge page in paging_map_page
                    if (pte & PTE_NOFREE) {
                        uint64_t *child_pt;
                        size_t child_idx;
                        paging_walk_page_tables(child_cr3, vaddr, &child_pt, &child_idx, PTE_USER);
                        if (!child_pt)
                            return -1;
                        child_pt[child_idx] = pte;
                        continue;
                    }

                    if ((pte & PTE_WRITABLE) || (pte & PTE_COW)) {
                        if (!(pte & PTE_COW)) {
                            pt[pti] = (pte & ~PTE_WRITABLE) | PTE_COW;
                            cow_ref_page(phys);
                            asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
                        }

                        cow_ref_page(phys);
                        flags &= ~PTE_WRITABLE;
                        flags |= PTE_COW;
                        paging_map_page_invl(child_cr3, phys, vaddr, flags, 0);
                    } else {
                        paddr_t child_phys = pmm_alloc_pages(1);
                        if (!child_phys)
                            return -1;

                        memcpy(paddr_to_vaddr(child_phys), paddr_to_vaddr(phys), PAGE_SIZE);
                        paging_map_page_invl(child_cr3, child_phys, vaddr, flags, 0);
                    }
                }
            }
        }
    }

    return 0;
}

int paging_handle_cow_fault(struct task *task, uint64_t fault_addr, uint64_t pf_error) {
    if (!task)
        return 0;

    // only a user write to a present page can be cow
    if ((pf_error & 0x7) != 0x7)
        return 0;

    uint64_t page = PAGE_ALIGN_DOWN(fault_addr);
    uint64_t *pte = paging_get_page(task->page_map, page, 0);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW))
        return 0;

    paddr_t old_phys = *pte & PADDR_ENTRY_MASK;
    uint64_t old_flags = *pte & ~PADDR_ENTRY_MASK;
    uint32_t refs = cow_get_refcount(old_phys);

    // last one holding the frame just takes it back, everyone else gets their own copy
    paddr_t new_phys = old_phys;
    if (refs > 1) {
        new_phys = pmm_alloc_pages(1);
        if (!new_phys)
            return 0;

        memcpy(paddr_to_vaddr(new_phys), paddr_to_vaddr(old_phys), PAGE_SIZE);
    }

    uint64_t new_flags = (old_flags | PTE_WRITABLE) & ~PTE_COW;
    *pte = (new_phys & PADDR_ENTRY_MASK) | new_flags;
    (void)cow_unref_page(old_phys);

    asm volatile("invlpg (%0)" :: "r"(page) : "memory");
    return 1;
}

void paging_release_user_space(paddr_t cr3) {
    uint64_t *pml4 = (uint64_t *)paddr_to_vaddr(cr3 & PADDR_ENTRY_MASK);
    if (!pml4)
        return;

    for (size_t pml4i = 0; pml4i < 256; pml4i++) {
        uint64_t pml4e = pml4[pml4i];
        if (!(pml4e & PTE_PRESENT))
            continue;

        paddr_t pdpt_phys = pml4e & PADDR_ENTRY_MASK;
        uint64_t *pdpt = (uint64_t *)paddr_to_vaddr(pdpt_phys);
        if (!pdpt)
            continue;

        for (size_t pdpti = 0; pdpti < 512; pdpti++) {
            uint64_t pdpte = pdpt[pdpti];
            if (!(pdpte & PTE_PRESENT))
                continue;
            if (pdpte & PTE_PAT_4K)
                continue;

            paddr_t pd_phys = pdpte & PADDR_ENTRY_MASK;
            uint64_t *pd = (uint64_t *)paddr_to_vaddr(pd_phys);
            if (!pd)
                continue;

            for (size_t pdi = 0; pdi < 512; pdi++) {
                uint64_t pde = pd[pdi];
                if (!(pde & PTE_PRESENT))
                    continue;
                if (pde & PTE_PAT_4K)
                    continue;

                paddr_t pt_phys = pde & PADDR_ENTRY_MASK;
                uint64_t *pt = (uint64_t *)paddr_to_vaddr(pt_phys);
                if (!pt)
                    continue;

                for (size_t pti = 0; pti < 512; pti++) {
                    uint64_t pte = pt[pti];
                    if (!(pte & PTE_PRESENT))
                        continue;

                    paddr_t phys = pte & PADDR_ENTRY_MASK;
                    if (pte & PTE_NOFREE) {
                        // never came from the pmm
                    } else if (pte & PTE_COW) {
                        if (cow_unref_page(phys) == 0)
                            pmm_free_pages(phys, 1);
                    } else {
                        pmm_free_pages(phys, 1);
                    }
                    pt[pti] = 0;
                }

                pmm_free_pages(pt_phys, 1);
                pd[pdi] = 0;
            }

            pmm_free_pages(pd_phys, 1);
            pdpt[pdpti] = 0;
        }

        pmm_free_pages(pdpt_phys, 1);
        pml4[pml4i] = 0;
    }
}
