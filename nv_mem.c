// nv_mem.c - NVIDIA GPU Memory Management Implementation
// VRAM allocator, GPU page tables, GART, buffer objects
#include "nv_mem.h"
#include "gpu.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"

// ============================================================
// External State
// ============================================================

extern gpu_state_t gpu_state;

// Shorthand MMIO accessors using global gpu_state
#define GPU_RD32(reg)       nv_rd32(gpu_state.mmio, (reg))
#define GPU_WR32(reg, val)  nv_wr32(gpu_state.mmio, (reg), (val))

// ============================================================
// Global State
// ============================================================

nv_mem_state_t nv_mem_state;

// ============================================================
// VRAM Allocator
// ============================================================

// Simple bump allocator with free list for VRAM

static uint64_t align_up_64(uint64_t val, uint64_t align) {
    return (val + align - 1) & ~(align - 1);
}

int nv_vram_alloc(uint64_t size, uint32_t alignment, uint32_t flags,
                  uint64_t* offset) {
    (void)flags;
    if (!nv_mem_state.initialized || size == 0) return -1;

    if (alignment < NV_MEM_ALIGN_4K) alignment = NV_MEM_ALIGN_4K;

    // First, check free list for a suitable block
    for (int i = 0; i < NV_VRAM_MAX_BLOCKS; i++) {
        nv_vram_block_t* blk = &nv_mem_state.vram_blocks[i];
        if (!blk->in_use) continue;
        // We don't reuse freed blocks in simple allocator; skip
    }

    // Bump allocator
    uint64_t aligned_off = align_up_64(nv_mem_state.vram_free_offset, alignment);
    if (aligned_off + size > nv_mem_state.vram_total) {
        return -1; // Out of VRAM
    }

    // Record allocation
    int slot = -1;
    for (int i = 0; i < NV_VRAM_MAX_BLOCKS; i++) {
        if (!nv_mem_state.vram_blocks[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    nv_mem_state.vram_blocks[slot].offset = aligned_off;
    nv_mem_state.vram_blocks[slot].size = size;
    nv_mem_state.vram_blocks[slot].flags = flags;
    nv_mem_state.vram_blocks[slot].in_use = 1;

    nv_mem_state.vram_free_offset = aligned_off + size;
    nv_mem_state.vram_used += size;
    nv_mem_state.alloc_count++;

    if (nv_mem_state.vram_used > nv_mem_state.peak_usage) {
        nv_mem_state.peak_usage = nv_mem_state.vram_used;
    }

    *offset = aligned_off;
    return 0;
}

void nv_vram_free(uint64_t offset) {
    for (int i = 0; i < NV_VRAM_MAX_BLOCKS; i++) {
        nv_vram_block_t* blk = &nv_mem_state.vram_blocks[i];
        if (blk->in_use && blk->offset == offset) {
            nv_mem_state.vram_used -= blk->size;
            blk->in_use = 0;
            nv_mem_state.free_count++;
            return;
        }
    }
}

uint64_t nv_vram_available(void) {
    return nv_mem_state.vram_total - nv_mem_state.vram_used;
}

uint64_t nv_vram_used_bytes(void) {
    return nv_mem_state.vram_used;
}

// ============================================================
// GPU Virtual Memory (NV50+)
// ============================================================

// Page Directory virtual address for kernel access
#define GPU_PD_VBASE    0xFFFF8000D0000000ULL

int nv_vm_init(void) {
    if (gpu_state.arch < 0x50) {
        // Pre-NV50 doesn't have GPU VM
        nv_mem_state.vm_enabled = 0;
        return 0;
    }

    // Allocate page directory (512 entries * 8 bytes = 4KB, fits in one page)
    void* pd_page = pmm_alloc_block();
    if (!pd_page) return -1;

    uint64_t pd_phys = (uint64_t)(uintptr_t)pd_page;
    nv_mem_state.pd_phys = pd_phys;
    nv_mem_state.pd_virt = GPU_PD_VBASE;

    // Map page directory to CPU space
    vmm_map_page(vmm_get_kernel_pml4(), GPU_PD_VBASE, pd_phys,
                 VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE);

    // Clear page directory
    uint64_t* pd = (uint64_t*)GPU_PD_VBASE;
    for (int i = 0; i < NV50_VM_PDE_COUNT; i++) {
        pd[i] = 0;
    }

    // Program the GPU's channel with this page directory
    // For NV50: Write PD address to BAR0 + 0x200 (PFIFO channel context)
    // This is simplified - real hardware needs per-channel VM binding
    if (gpu_state.mmio) {
        // Program VM engine with page directory base
        // NV50_PFIFO_VM_PD = 0x1280
        GPU_WR32(0x001280, (uint32_t)(pd_phys >> 8));
    }

    nv_mem_state.vm_enabled = 1;
    nv_mem_state.vm_next_addr = 0x10000000ULL; // Start allocations at 256MB

    return 0;
}

int nv_vm_map(uint64_t gpu_va, uint64_t phys_addr, uint64_t size, uint32_t flags) {
    if (!nv_mem_state.vm_enabled) return -1;

    uint64_t* pd = (uint64_t*)nv_mem_state.pd_virt;
    uint64_t va = gpu_va;
    uint64_t pa = phys_addr;

    while (size > 0) {
        // PDE index
        int pde_idx = (int)(va / NV50_VM_BLOCK_SIZE);
        if (pde_idx >= NV50_VM_PDE_COUNT) return -1;

        // Ensure PDE exists
        if (!(pd[pde_idx] & NV50_PDE_PRESENT)) {
            // Allocate a page table page
            void* pt_page = pmm_alloc_block();
            if (!pt_page) return -1;

            uint64_t pt_phys = (uint64_t)(uintptr_t)pt_page;

            // Map PT to CPU space for initialization
            uint64_t pt_virt = GPU_PD_VBASE + 0x1000 + (uint64_t)pde_idx * 0x1000;
            vmm_map_page(vmm_get_kernel_pml4(), pt_virt, pt_phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE);

            // Clear page table
            uint64_t* pt = (uint64_t*)pt_virt;
            for (int i = 0; i < (int)(0x1000 / sizeof(uint64_t)); i++) {
                pt[i] = 0;
            }

            // Set PDE
            pd[pde_idx] = NV50_PDE_PRESENT | (pt_phys & 0xFFFFFFF000ULL);
        }

        // Get page table
        uint64_t pt_phys = pd[pde_idx] & 0xFFFFFFF000ULL;
        uint64_t pt_virt = GPU_PD_VBASE + 0x1000 + (uint64_t)pde_idx * 0x1000;

        // PTE index within this PDE's coverage
        uint64_t va_in_block = va - (uint64_t)pde_idx * NV50_VM_BLOCK_SIZE;
        int pte_idx = (int)(va_in_block / NV50_VM_SMALL_PAGE);

        // Set PTE
        uint64_t* pt = (uint64_t*)pt_virt;
        uint64_t pte = NV50_PTE_PRESENT | (pa & 0xFFFFFFF000ULL);
        if (flags & NV_MEM_ACCESS_RO) {
            pte |= NV50_PTE_READ_ONLY;
        }
        pt[pte_idx] = pte;

        va += NV50_VM_SMALL_PAGE;
        pa += NV50_VM_SMALL_PAGE;
        if (size >= NV50_VM_SMALL_PAGE) {
            size -= NV50_VM_SMALL_PAGE;
        } else {
            size = 0;
        }
    }

    // Flush GPU TLB
    if (gpu_state.mmio) {
        // NV50_VM_TLB_FLUSH = 0x100C80
        GPU_WR32(0x100C80, 0x00000001);
    }

    return 0;
}

int nv_vm_unmap(uint64_t gpu_va, uint64_t size) {
    if (!nv_mem_state.vm_enabled) return -1;

    uint64_t* pd = (uint64_t*)nv_mem_state.pd_virt;
    uint64_t va = gpu_va;

    while (size > 0) {
        int pde_idx = (int)(va / NV50_VM_BLOCK_SIZE);
        if (pde_idx >= NV50_VM_PDE_COUNT) return -1;

        if (pd[pde_idx] & NV50_PDE_PRESENT) {
            uint64_t pt_virt = GPU_PD_VBASE + 0x1000 + (uint64_t)pde_idx * 0x1000;
            uint64_t va_in_block = va - (uint64_t)pde_idx * NV50_VM_BLOCK_SIZE;
            int pte_idx = (int)(va_in_block / NV50_VM_SMALL_PAGE);

            uint64_t* pt = (uint64_t*)pt_virt;
            pt[pte_idx] = 0;
        }

        va += NV50_VM_SMALL_PAGE;
        if (size >= NV50_VM_SMALL_PAGE) {
            size -= NV50_VM_SMALL_PAGE;
        } else {
            size = 0;
        }
    }

    // Flush GPU TLB
    if (gpu_state.mmio) {
        GPU_WR32(0x100C80, 0x00000001);
    }

    return 0;
}

int nv_vm_alloc_va(uint64_t size, uint64_t alignment, uint64_t* gpu_va) {
    if (!nv_mem_state.vm_enabled) return -1;
    if (alignment < NV50_VM_SMALL_PAGE) alignment = NV50_VM_SMALL_PAGE;

    uint64_t addr = align_up_64(nv_mem_state.vm_next_addr, alignment);
    nv_mem_state.vm_next_addr = addr + align_up_64(size, NV50_VM_SMALL_PAGE);

    *gpu_va = addr;
    return 0;
}

// ============================================================
// GART (Graphics Address Remapping Table)
// ============================================================

int nv_gart_map(uint64_t cpu_phys, uint64_t size, uint64_t* gpu_addr) {
    if (!nv_mem_state.initialized) return -1;

    // Find free GART slot
    int slot = -1;
    for (int i = 0; i < NV_GART_MAX_PAGES; i++) {
        if (!nv_mem_state.gart_entries[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    uint64_t ga = align_up_64(nv_mem_state.gart_next_addr, NV50_VM_SMALL_PAGE);

    nv_mem_state.gart_entries[slot].gpu_addr = ga;
    nv_mem_state.gart_entries[slot].cpu_phys = cpu_phys;
    nv_mem_state.gart_entries[slot].size = size;
    nv_mem_state.gart_entries[slot].in_use = 1;

    nv_mem_state.gart_next_addr = ga + align_up_64(size, NV50_VM_SMALL_PAGE);

    // If VM is available, create GPU page table mapping
    if (nv_mem_state.vm_enabled) {
        nv_vm_map(ga, cpu_phys, size, NV_MEM_ACCESS_RW);
    } else {
        // Pre-NV50: Program GART table directly
        // PGARTTABLE at 0x10000 in BAR2
        if (gpu_state.ramin) {
            uint64_t pages = (size + NV50_VM_SMALL_PAGE - 1) / NV50_VM_SMALL_PAGE;
            for (uint64_t i = 0; i < pages; i++) {
                // Each GART entry maps one page
                uint32_t entry_offset = (uint32_t)((ga / NV50_VM_SMALL_PAGE + i) * 4);
                volatile uint32_t* gart = (volatile uint32_t*)((uint64_t)gpu_state.ramin + 0x10000);
                gart[entry_offset / 4] = (uint32_t)((cpu_phys + i * NV50_VM_SMALL_PAGE) >> 12) | 0x01;
            }
        }
    }

    *gpu_addr = ga;
    return 0;
}

void nv_gart_unmap(uint64_t gpu_addr) {
    for (int i = 0; i < NV_GART_MAX_PAGES; i++) {
        nv_gart_entry_t* e = &nv_mem_state.gart_entries[i];
        if (e->in_use && e->gpu_addr == gpu_addr) {
            if (nv_mem_state.vm_enabled) {
                nv_vm_unmap(gpu_addr, e->size);
            }
            e->in_use = 0;
            return;
        }
    }
}

// ============================================================
// Buffer Objects
// ============================================================

int nv_bo_new(uint64_t size, uint32_t flags, nv_bo_t* bo) {
    if (!nv_mem_state.initialized || !bo) return -1;

    bo->size = size;
    bo->flags = flags;
    bo->cpu_addr = 0;

    if (flags & NV_MEM_VRAM) {
        // Allocate in VRAM
        uint64_t offset;
        if (nv_vram_alloc(size, NV_MEM_ALIGN_4K, flags, &offset) < 0) {
            return -1;
        }
        bo->gpu_offset = offset;
        bo->domain = NV_MEM_VRAM;
    } else {
        // Allocate in system memory and map through GART
        uint64_t pages = (size + 4095) / 4096;
        // Allocate contiguous physical pages
        // For simplicity, allocate one page at a time
        void* phys = pmm_alloc_block();
        if (!phys) return -1;

        uint64_t gpu_addr;
        if (nv_gart_map((uint64_t)(uintptr_t)phys, pages * 4096, &gpu_addr) < 0) {
            pmm_free_block(phys);
            return -1;
        }
        bo->gpu_offset = gpu_addr;
        bo->domain = NV_MEM_GART;
    }

    return 0;
}

void nv_bo_del(nv_bo_t* bo) {
    if (!bo) return;

    if (bo->cpu_addr) {
        nv_bo_unmap(bo);
    }

    if (bo->domain == NV_MEM_VRAM) {
        nv_vram_free(bo->gpu_offset);
    } else if (bo->domain == NV_MEM_GART) {
        nv_gart_unmap(bo->gpu_offset);
    }

    bo->gpu_offset = 0;
    bo->size = 0;
}

int nv_bo_map(nv_bo_t* bo) {
    if (!bo || bo->cpu_addr) return -1;

    if (bo->domain == NV_MEM_VRAM) {
        // Map through BAR1 aperture
        if (gpu_state.vram && bo->gpu_offset < gpu_state.vram_size) {
            bo->cpu_addr = (uint64_t)gpu_state.vram + bo->gpu_offset;
            return 0;
        }
        return -1;
    } else {
        // GART buffers are in system memory, find the physical address
        for (int i = 0; i < NV_GART_MAX_PAGES; i++) {
            nv_gart_entry_t* e = &nv_mem_state.gart_entries[i];
            if (e->in_use && e->gpu_addr == bo->gpu_offset) {
                // Map the physical memory to a CPU virtual address
                uint64_t virt = 0xFFFF8000E0000000ULL + bo->gpu_offset;
                uint64_t pages = (bo->size + 4095) / 4096;
                for (uint64_t p = 0; p < pages; p++) {
                    vmm_map_page(vmm_get_kernel_pml4(),
                                virt + p * 4096,
                                e->cpu_phys + p * 4096,
                                VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
                }
                bo->cpu_addr = virt;
                return 0;
            }
        }
        return -1;
    }
}

void nv_bo_unmap(nv_bo_t* bo) {
    if (!bo) return;
    // For VRAM BOs mapped through BAR1, just clear the pointer
    // For GART BOs, we'd need to unmap VMM pages (simplified)
    bo->cpu_addr = 0;
}

// ============================================================
// Initialization
// ============================================================

int nv_mem_init(void) {
    // Zero state
    for (int i = 0; i < (int)sizeof(nv_mem_state); i++) {
        ((char*)&nv_mem_state)[i] = 0;
    }

    if (!gpu_state.initialized) {
        nv_mem_state.initialized = 1;
        return 0; // No GPU, just stub
    }

    nv_mem_state.vram_total = gpu_state.vram_total;

    // Reserve first 16MB for display scanout
    nv_mem_state.vram_free_offset = 16 * 1024 * 1024;

    // Initialize GPU virtual memory if NV50+
    nv_vm_init();

    // Initialize GART space
    nv_mem_state.gart_next_addr = NV_GPU_GART_START;

    nv_mem_state.initialized = 1;
    return 0;
}

void nv_mem_shutdown(void) {
    // Free all VRAM allocations (tracking only)
    for (int i = 0; i < NV_VRAM_MAX_BLOCKS; i++) {
        nv_mem_state.vram_blocks[i].in_use = 0;
    }
    // Free GART entries
    for (int i = 0; i < NV_GART_MAX_PAGES; i++) {
        nv_mem_state.gart_entries[i].in_use = 0;
    }
    nv_mem_state.initialized = 0;
}
