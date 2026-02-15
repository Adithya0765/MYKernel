// vmm.c - Virtual Memory Manager for Alteo OS
// Implements 4-level page tables with identity mapping and per-process address spaces
#include "vmm.h"
#include "pmm.h"
#include "isr.h"

// Kernel PML4 - shared across all address spaces
static pte_t* kernel_pml4 = 0;

// ---------- Helpers ----------

static void vmm_memset(void* ptr, int val, uint64_t size) {
    uint8_t* p = (uint8_t*)ptr;
    for (uint64_t i = 0; i < size; i++) p[i] = (uint8_t)val;
}

// Allocate a zeroed 4KB page for use as a page table
static pte_t* vmm_alloc_table(void) {
    void* page = pmm_alloc_block();
    if (!page) return 0;
    vmm_memset(page, 0, VMM_PAGE_SIZE);
    return (pte_t*)page;
}

// ---------- CR/TLB operations ----------

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

void vmm_invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

// ---------- Page fault handler ----------

static void page_fault_handler(registers_t* regs) {
    uint64_t fault_addr = read_cr2();
    uint64_t err = regs->err_code;

    // Decode error code
    int present  = err & 0x1;  // Page was present (protection violation)
    int write    = err & 0x2;  // Write access caused the fault
    int user     = err & 0x4;  // Fault occurred in user mode
    int reserved = err & 0x8;  // Reserved bit overwrite
    int ifetch   = err & 0x10; // Instruction fetch caused the fault

    (void)present;
    (void)write;
    (void)user;
    (void)reserved;
    (void)ifetch;
    (void)fault_addr;

    // TODO: Handle demand paging, copy-on-write, stack growth, mmap, etc.
    // For now, page faults in kernel mode are fatal
    // In the future:
    //   - Check if fault_addr is in a valid VMA for the process
    //   - If demand paging: allocate a physical page and map it
    //   - If COW: copy the page and remap with write permission
    //   - If invalid: send SIGSEGV to the process (or panic if in kernel)

    // Fatal: halt the system
    __asm__ volatile("cli; hlt");
}

// ---------- Initialization ----------

void vmm_init(void) {
    // Allocate the kernel PML4
    kernel_pml4 = vmm_alloc_table();
    if (!kernel_pml4) return;

    // Identity-map the first 4GB using 2MB large pages
    // This covers: kernel code/data (~1MB-16MB), PMM bitmap (16MB),
    // heap (100MB+), framebuffer (typically ~0xFD000000), and MMIO regions
    //
    // Structure: PML4[0] -> PDPT -> PD[0..3] (each PD covers 1GB via 512 x 2MB pages)

    pte_t* pdpt = vmm_alloc_table();
    if (!pdpt) return;
    kernel_pml4[0] = (uint64_t)pdpt | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

    for (int gb = 0; gb < 4; gb++) {
        pte_t* pd = vmm_alloc_table();
        if (!pd) return;
        pdpt[gb] = (uint64_t)pd | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

        for (int i = 0; i < 512; i++) {
            uint64_t phys = (uint64_t)gb * 0x40000000ULL + (uint64_t)i * VMM_LARGE_PAGE_SIZE;
            pd[i] = phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_HUGE;
        }
    }

    // Register the page fault handler (ISR 14)
    isr_register_handler(14, page_fault_handler);

    // Switch CR3 to our new page tables
    // Since we identity-mapped the same 0-4GB range that boot.asm did,
    // this transition is seamless — all current pointers remain valid.
    write_cr3((uint64_t)kernel_pml4);
}

// ---------- Page mapping ----------

int vmm_map_page(pte_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) return -1;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt);
    uint64_t pd_idx   = VMM_PD_INDEX(virt);
    uint64_t pt_idx   = VMM_PT_INDEX(virt);

    // Ensure PDPT exists
    pte_t* pdpt;
    if (pml4[pml4_idx] & VMM_FLAG_PRESENT) {
        pdpt = (pte_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    } else {
        pdpt = vmm_alloc_table();
        if (!pdpt) return -1;
        pml4[pml4_idx] = (uint64_t)pdpt | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                        | (flags & VMM_FLAG_USER);
    }

    // Ensure PD exists
    pte_t* pd;
    if (pdpt[pdpt_idx] & VMM_FLAG_PRESENT) {
        // Check for 1GB huge page — can't map 4KB inside one
        if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) return -1;
        pd = (pte_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    } else {
        pd = vmm_alloc_table();
        if (!pd) return -1;
        pdpt[pdpt_idx] = (uint64_t)pd | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                        | (flags & VMM_FLAG_USER);
    }

    // Handle 2MB huge page — must split into 4KB pages first
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        pte_t* pt = vmm_alloc_table();
        if (!pt) return -1;

        // Fill PT with 512 x 4KB pages matching the original 2MB mapping
        uint64_t base_phys = pd[pd_idx] & VMM_LARGE_ADDR_MASK;
        uint64_t old_flags = pd[pd_idx] & 0xFFF; // Preserve original flags (minus HUGE)
        old_flags &= ~VMM_FLAG_HUGE;

        for (int i = 0; i < VMM_ENTRIES_PER_TABLE; i++) {
            pt[i] = (base_phys + (uint64_t)i * VMM_PAGE_SIZE)
                   | (old_flags | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
        }
        pd[pd_idx] = (uint64_t)pt | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                    | (flags & VMM_FLAG_USER);
    }

    // Ensure PT exists
    pte_t* pt;
    if (pd[pd_idx] & VMM_FLAG_PRESENT) {
        pt = (pte_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    } else {
        pt = vmm_alloc_table();
        if (!pt) return -1;
        pd[pd_idx] = (uint64_t)pt | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                    | (flags & VMM_FLAG_USER);
    }

    // Set the page table entry
    pt[pt_idx] = (phys & VMM_ADDR_MASK) | flags;

    // Flush TLB for this virtual address
    vmm_invlpg(virt);

    return 0;
}

void vmm_unmap_page(pte_t* pml4, uint64_t virt) {
    if (!pml4) return;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt);
    uint64_t pd_idx   = VMM_PD_INDEX(virt);
    uint64_t pt_idx   = VMM_PT_INDEX(virt);

    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return;
    pte_t* pdpt = (pte_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return;
    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) return; // Can't unmap from 1GB huge page
    pte_t* pd = (pte_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);

    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return;
    if (pd[pd_idx] & VMM_FLAG_HUGE) return; // Can't unmap from 2MB huge page
    pte_t* pt = (pte_t*)(pd[pd_idx] & VMM_ADDR_MASK);

    pt[pt_idx] = 0;
    vmm_invlpg(virt);
}

uint64_t vmm_get_physical(pte_t* pml4, uint64_t virt) {
    if (!pml4) return 0;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt);
    uint64_t pd_idx   = VMM_PD_INDEX(virt);
    uint64_t pt_idx   = VMM_PT_INDEX(virt);

    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;
    pte_t* pdpt = (pte_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) {
        // 1GB page
        return (pdpt[pdpt_idx] & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFF);
    }
    pte_t* pd = (pte_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);

    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        // 2MB page
        return (pd[pd_idx] & VMM_LARGE_ADDR_MASK) | (virt & 0x1FFFFF);
    }
    pte_t* pt = (pte_t*)(pd[pd_idx] & VMM_ADDR_MASK);

    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;
    return (pt[pt_idx] & VMM_ADDR_MASK) | (virt & 0xFFF);
}

// ---------- Address space management ----------

pte_t* vmm_create_address_space(void) {
    pte_t* pml4 = vmm_alloc_table();
    if (!pml4) return 0;

    // Copy kernel mappings from the kernel PML4
    // The kernel identity mapping is in PML4 entry 0 (covers 0-512GB)
    // All processes share the same kernel mapping
    if (kernel_pml4) {
        pml4[0] = kernel_pml4[0];
    }

    return pml4;
}

void vmm_destroy_address_space(pte_t* pml4) {
    if (!pml4 || pml4 == kernel_pml4) return;

    // Walk user-space entries and free allocated page tables
    // Skip entry 0 (kernel space — shared, don't free)
    for (int i = 1; i < VMM_ENTRIES_PER_TABLE; i++) {
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;
        pte_t* pdpt = (pte_t*)(pml4[i] & VMM_ADDR_MASK);

        for (int j = 0; j < VMM_ENTRIES_PER_TABLE; j++) {
            if (!(pdpt[j] & VMM_FLAG_PRESENT)) continue;
            if (pdpt[j] & VMM_FLAG_HUGE) continue;
            pte_t* pd = (pte_t*)(pdpt[j] & VMM_ADDR_MASK);

            for (int k = 0; k < VMM_ENTRIES_PER_TABLE; k++) {
                if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
                if (pd[k] & VMM_FLAG_HUGE) continue;
                pte_t* pt = (pte_t*)(pd[k] & VMM_ADDR_MASK);

                // Free physical pages mapped by this PT
                for (int l = 0; l < VMM_ENTRIES_PER_TABLE; l++) {
                    if (pt[l] & VMM_FLAG_PRESENT) {
                        pmm_free_block((void*)(pt[l] & VMM_ADDR_MASK));
                    }
                }
                pmm_free_block(pt);
            }
            pmm_free_block(pd);
        }
        pmm_free_block(pdpt);
    }

    pmm_free_block(pml4);
}

void vmm_switch_address_space(pte_t* pml4) {
    if (pml4) {
        write_cr3((uint64_t)pml4);
    }
}

pte_t* vmm_get_current_address_space(void) {
    return (pte_t*)(read_cr3() & VMM_ADDR_MASK);
}

pte_t* vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}

// ---------- Convenience functions ----------

int vmm_map_range(pte_t* pml4, uint64_t virt_start, uint64_t phys_start,
                  uint64_t size, uint64_t flags) {
    uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virt = virt_start + i * VMM_PAGE_SIZE;
        uint64_t phys = phys_start + i * VMM_PAGE_SIZE;
        if (vmm_map_page(pml4, virt, phys, flags) < 0) return -1;
    }
    return 0;
}

int vmm_alloc_pages(pte_t* pml4, uint64_t virt_start, uint64_t size, uint64_t flags) {
    uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        void* phys = pmm_alloc_block();
        if (!phys) return -1;
        vmm_memset(phys, 0, VMM_PAGE_SIZE); // Zero the page
        uint64_t virt = virt_start + i * VMM_PAGE_SIZE;
        if (vmm_map_page(pml4, virt, (uint64_t)phys, flags) < 0) {
            pmm_free_block(phys);
            return -1;
        }
    }
    return 0;
}
