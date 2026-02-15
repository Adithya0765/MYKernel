// nv_mem.h - NVIDIA GPU Memory Management for Alteo OS
// GPU virtual address space, VRAM allocation, page table management
#ifndef NV_MEM_H
#define NV_MEM_H

#include "stdint.h"

// ============================================================
// VRAM Allocation
// ============================================================

#define NV_MEM_VRAM         0x01    // Allocate in VRAM
#define NV_MEM_GART         0x02    // Allocate in system memory (GPU-accessible)
#define NV_MEM_MAPPABLE     0x04    // Must be in CPU-mappable region
#define NV_MEM_CONTIG       0x08    // Physically contiguous

// Alignment requirements
#define NV_MEM_ALIGN_4K     0x1000
#define NV_MEM_ALIGN_64K    0x10000
#define NV_MEM_ALIGN_1M     0x100000

// Memory types for NV50+ page tables
#define NV_MEM_TYPE_VM      0x7FC00   // VM type for VRAM
#define NV_MEM_TYPE_GART    0x7FC01   // VM type for GART
#define NV_MEM_ACCESS_RW    0x01
#define NV_MEM_ACCESS_RO    0x02

// ============================================================
// VRAM Allocator
// ============================================================

#define NV_VRAM_MAX_BLOCKS  256

typedef struct {
    uint64_t offset;        // Offset within VRAM
    uint64_t size;          // Size in bytes
    uint32_t flags;         // NV_MEM_* flags
    int      in_use;
} nv_vram_block_t;

// ============================================================
// GPU Virtual Address Space (NV50+)
// ============================================================

// NV50 GPU page table structure:
// PD (Page Directory) -> PT (Page Table) -> Page
// PDE covers 128MB (27 bits), PTE covers 4KB or 64KB pages

#define NV50_VM_BLOCK_SIZE      (128 * 1024 * 1024)    // 128MB per PDE
#define NV50_VM_SMALL_PAGE      4096                     // 4KB small pages
#define NV50_VM_LARGE_PAGE      (64 * 1024)             // 64KB large pages
#define NV50_VM_PDE_COUNT       512                      // Max PDEs
#define NV50_VM_PTE_COUNT       32768                    // PTEs per PDE (small pages)
#define NV50_VM_PTE_LARGE_COUNT 2048                     // PTEs per PDE (large pages)

// PDE format (NV50)
#define NV50_PDE_PRESENT        (1ULL << 0)
#define NV50_PDE_LARGE_PAGE     (1ULL << 2)
// Bits 12-39: Physical address of page table >> 12

// PTE format (NV50)
#define NV50_PTE_PRESENT        (1ULL << 0)
#define NV50_PTE_SUPERVISOR     (1ULL << 1)
#define NV50_PTE_READ_ONLY      (1ULL << 2)
#define NV50_PTE_COMPRESSED     (1ULL << 33)
#define NV50_PTE_STORAGE_TYPE_SHIFT 40
// Bits 12-39: Physical address >> 12

// GPU address ranges
#define NV_GPU_VRAM_START       0x0000000000000000ULL
#define NV_GPU_VRAM_END         0x0000000100000000ULL   // 4GB VRAM space
#define NV_GPU_GART_START       0x0000000100000000ULL
#define NV_GPU_GART_END         0x0000000200000000ULL   // 4GB GART space

// ============================================================
// GART (Graphics Address Remapping Table)
// ============================================================

#define NV_GART_MAX_PAGES   1024    // Max system pages mapped to GPU

typedef struct {
    uint64_t gpu_addr;      // GPU virtual address
    uint64_t cpu_phys;      // CPU physical address
    uint64_t size;          // Mapping size
    int      in_use;
} nv_gart_entry_t;

// ============================================================
// GPU Memory State
// ============================================================

typedef struct {
    int initialized;

    // VRAM allocator
    nv_vram_block_t vram_blocks[NV_VRAM_MAX_BLOCKS];
    uint64_t        vram_total;
    uint64_t        vram_used;
    uint64_t        vram_free_offset;   // Simple bump allocator watermark

    // GPU virtual address space (NV50+)
    int             vm_enabled;
    uint64_t        pd_phys;            // Physical address of page directory
    uint64_t        pd_virt;            // Virtual address of page directory
    uint64_t        vm_next_addr;       // Next free GPU VA

    // GART
    nv_gart_entry_t gart_entries[NV_GART_MAX_PAGES];
    uint64_t        gart_next_addr;

    // Stats
    uint32_t        alloc_count;
    uint32_t        free_count;
    uint64_t        peak_usage;
} nv_mem_state_t;

// ============================================================
// API
// ============================================================

// ---- Initialization ----
int  nv_mem_init(void);
void nv_mem_shutdown(void);

// ---- VRAM Allocation ----
int      nv_vram_alloc(uint64_t size, uint32_t alignment, uint32_t flags,
                       uint64_t* offset);
void     nv_vram_free(uint64_t offset);
uint64_t nv_vram_available(void);
uint64_t nv_vram_used_bytes(void);

// ---- GPU Virtual Memory (NV50+) ----
int  nv_vm_init(void);
int  nv_vm_map(uint64_t gpu_va, uint64_t phys_addr, uint64_t size, uint32_t flags);
int  nv_vm_unmap(uint64_t gpu_va, uint64_t size);
int  nv_vm_alloc_va(uint64_t size, uint64_t alignment, uint64_t* gpu_va);

// ---- GART ----
int  nv_gart_map(uint64_t cpu_phys, uint64_t size, uint64_t* gpu_addr);
void nv_gart_unmap(uint64_t gpu_addr);

// ---- Buffer Objects ----
typedef struct {
    uint64_t gpu_offset;    // Offset in VRAM or GPU VA
    uint64_t cpu_addr;      // CPU virtual address (if mapped)
    uint64_t size;
    uint32_t flags;
    int      domain;        // NV_MEM_VRAM or NV_MEM_GART
} nv_bo_t;

int  nv_bo_new(uint64_t size, uint32_t flags, nv_bo_t* bo);
void nv_bo_del(nv_bo_t* bo);
int  nv_bo_map(nv_bo_t* bo);
void nv_bo_unmap(nv_bo_t* bo);

#endif
