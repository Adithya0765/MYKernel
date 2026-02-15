// vmm.h - Virtual Memory Manager for Alteo OS
// Provides 4-level page table management (PML4 -> PDPT -> PD -> PT)
#ifndef VMM_H
#define VMM_H

#include "stdint.h"

// Page flags (x86_64 page table entry bits)
#define VMM_FLAG_PRESENT      (1ULL << 0)
#define VMM_FLAG_WRITABLE     (1ULL << 1)
#define VMM_FLAG_USER         (1ULL << 2)
#define VMM_FLAG_WRITETHROUGH (1ULL << 3)
#define VMM_FLAG_NOCACHE      (1ULL << 4)
#define VMM_FLAG_ACCESSED     (1ULL << 5)
#define VMM_FLAG_DIRTY        (1ULL << 6)
#define VMM_FLAG_HUGE         (1ULL << 7)   // 2MB page (PD) or 1GB page (PDPT)
#define VMM_FLAG_GLOBAL       (1ULL << 8)
#define VMM_FLAG_NX           (1ULL << 63)  // No-execute

// Address masks
#define VMM_ADDR_MASK         0x000FFFFFFFFFF000ULL  // Bits 12-51
#define VMM_LARGE_ADDR_MASK   0x000FFFFFFFE00000ULL  // Bits 21-51 (2MB aligned)

// Page sizes
#define VMM_PAGE_SIZE         4096
#define VMM_LARGE_PAGE_SIZE   (2 * 1024 * 1024)  // 2MB

// Page table index extraction macros
#define VMM_PML4_INDEX(addr)  (((uint64_t)(addr) >> 39) & 0x1FF)
#define VMM_PDPT_INDEX(addr)  (((uint64_t)(addr) >> 30) & 0x1FF)
#define VMM_PD_INDEX(addr)    (((uint64_t)(addr) >> 21) & 0x1FF)
#define VMM_PT_INDEX(addr)    (((uint64_t)(addr) >> 12) & 0x1FF)

// Number of entries per table
#define VMM_ENTRIES_PER_TABLE 512

// Page table entry type
typedef uint64_t pte_t;

// Initialize the VMM: create kernel page tables, identity-map 4GB, install page fault handler
void vmm_init(void);

// Map a single 4KB page: virtual -> physical with given flags
int vmm_map_page(pte_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a single 4KB page
void vmm_unmap_page(pte_t* pml4, uint64_t virt);

// Get the physical address for a virtual address (returns 0 if unmapped)
uint64_t vmm_get_physical(pte_t* pml4, uint64_t virt);

// Create a new address space (PML4) with kernel space already mapped
pte_t* vmm_create_address_space(void);

// Destroy an address space (free all user-space page tables)
void vmm_destroy_address_space(pte_t* pml4);

// Switch to an address space (load CR3)
void vmm_switch_address_space(pte_t* pml4);

// Get the current address space (read CR3)
pte_t* vmm_get_current_address_space(void);

// Get the kernel PML4
pte_t* vmm_get_kernel_pml4(void);

// Map a range of pages (convenience)
int vmm_map_range(pte_t* pml4, uint64_t virt_start, uint64_t phys_start,
                  uint64_t size, uint64_t flags);

// Allocate physical pages and map them at a virtual address
int vmm_alloc_pages(pte_t* pml4, uint64_t virt_start, uint64_t size, uint64_t flags);

// Invalidate a single TLB entry
void vmm_invlpg(uint64_t addr);

#endif
