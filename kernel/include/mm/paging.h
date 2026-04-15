#pragma once

#include <stdint.h>
#include <mm/pmm.h>
#include <cpu/io.h>

#define PTE_PRESENT         (1ULL << 0)
#define PTE_WRITABLE        (1ULL << 1)
#define PTE_USER            (1ULL << 2)
#define PTE_PWT             (1ULL << 3)
#define PTE_PCD             (1ULL << 4)
#define PTE_ACCESSED        (1ULL << 5)
#define PTE_DIRTY           (1ULL << 6)
#define PTE_PAGESIZE        (1ULL << 7)
#define PTE_GLOBAL          (1ULL << 8)
#define PTE_COW             (1ULL << 9)
#define PTE_PAT             (1ULL << 12)
#define PTE_NX              (1ULL << 63)

#define PAGE_KERNEL_RW      (PTE_WRITABLE)
#define PAGE_KERNEL_RO      (0)
#define PAGE_USER_RW        (PTE_WRITABLE | PTE_USER)
#define PAGE_USER_RO        (PTE_USER)

#define PAGE_FB_WC          (PTE_WRITABLE | PTE_PCD | PTE_PAT | PTE_PAGESIZE)

#define PAGE_SIZE_4K       4096
#define PAGE_SIZE_2M       (512 * PAGE_SIZE_4K)
#define PADDR_ENTRY_MASK   0x000FFFFFFFFFF000ULL

#define PAGE_ALIGN_UP(n)   (((n) + (PAGE_SIZE-1))/PAGE_SIZE*PAGE_SIZE)
#define PAGE_ALIGN_DOWN(n) ((n)/PAGE_SIZE*PAGE_SIZE)

extern paddr_t cr3_paddr;
extern paddr_t kernel_page_map;
struct task;

void reinit_paging();

/// @brief map a physical page at a vaddr using a pd entry
/// @param paddr physical address to map the page frame at
/// @param vaddr virtual address to map the page at
/// @param flags PTE Flags
void paging_map_page(paddr_t cr3, uint64_t paddr, uint64_t vaddr, uint64_t flags);
void paging_map_pages(paddr_t cr3, uint64_t paddr, uint64_t vaddr, size_t size, uint64_t flags);
void paging_map_page_invl(paddr_t cr3, uint64_t paddr, uint64_t vaddr, uint64_t flags, int invalidate_tlb);

/// @brief reinitalise paging so we can access a full memory range, not just the
/// default from limine
paddr_t paging_create_address_space(void);

/// @brief switch the current CR3 address space context
/// @param cr3 cr3 paddr
void paging_switch_address_space(paddr_t cr3);

/// @brief free address space CR3 provided
/// @param cr3 target
void paging_destroy_address_space(paddr_t cr3);
void paging_release_user_space(paddr_t cr3);

uint64_t paging_alloc_empty_frame(void **vaddr);

uint64_t* paging_get_page(paddr_t cr3, uint64_t vaddr, int create);

void paging_unmap_page(paddr_t cr3, uint64_t vaddr);

int paging_clone_user_space(paddr_t parent_cr3, paddr_t child_cr3);
int paging_handle_cow_fault(struct task *task, uint64_t fault_addr, uint64_t pf_error);

void pat_init(void);
