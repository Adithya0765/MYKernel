// pmm.c - Physical Memory Manager (Fixed)
#include "pmm.h"
#include "graphics.h" // Includes multiboot_info_t definition

// Memory Map Entry (Specific to PMM logic)
typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) mmap_entry_t;

// Globals
static uint8_t* bitmap = (uint8_t*)0x1000000; // Bitmap at 16MB
static uint64_t total_blocks = 0;
static uint64_t used_blocks = 0;
static uint64_t bitmap_size = 0;

void pmm_set_bit(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

void pmm_unset_bit(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

int pmm_test_bit(uint64_t bit) {
    return bitmap[bit / 8] & (1 << (bit % 8));
}

int64_t pmm_find_first_free() {
    for (uint64_t i = 0; i < total_blocks; i++) {
        if (!pmm_test_bit(i)) return i;
    }
    return -1;
}

void pmm_init(uint64_t multiboot_info_addr) {
    // Validate multiboot info address
    if (multiboot_info_addr == 0 || multiboot_info_addr < 0x1000) {
        return; // Invalid address, skip PMM initialization
    }
    
    multiboot_info_t* mb_info = (multiboot_info_t*)multiboot_info_addr;
    uint64_t mem_size = 0;

    // Method 1: Try Memory Map (Bit 6)
    if (mb_info->flags & (1 << 6)) {
        mmap_entry_t* entry = (mmap_entry_t*)(uint64_t)mb_info->mmap_addr;
        uint64_t end_addr = mb_info->mmap_addr + mb_info->mmap_length;
        
        while ((uint64_t)entry < end_addr) {
            if (entry->type == 1) { // Available
                uint64_t top = entry->addr + entry->len;
                if (top > mem_size) mem_size = top;
            }
            entry = (mmap_entry_t*)((uint64_t)entry + entry->size + 4);
        }
    } 
    
    // Method 2: Fallback to mem_upper (Bit 0)
    if (mem_size == 0 && (mb_info->flags & (1 << 0))) {
        mem_size = (mb_info->mem_upper * 1024) + 0x100000;
    }

    // Default safety
    if (mem_size == 0) mem_size = 32 * 1024 * 1024;

    // Initialize Bitmap
    total_blocks = mem_size / PAGE_SIZE;
    bitmap_size = total_blocks / 8;

    // 1. Mark EVERYTHING as used first
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF; 
    }

    // 2. Mark available regions as free
    if (mb_info->flags & (1 << 6)) {
        mmap_entry_t* entry = (mmap_entry_t*)(uint64_t)mb_info->mmap_addr;
        uint64_t end_addr = mb_info->mmap_addr + mb_info->mmap_length;
        while ((uint64_t)entry < end_addr) {
            if (entry->type == 1) {
                uint64_t start_block = entry->addr / PAGE_SIZE;
                uint64_t count = entry->len / PAGE_SIZE;
                for (uint64_t i = 0; i < count; i++) {
                    if (start_block + i < total_blocks)
                        pmm_unset_bit(start_block + i);
                }
            }
            entry = (mmap_entry_t*)((uint64_t)entry + entry->size + 4);
        }
    } else {
        uint64_t start = 0x100000 / PAGE_SIZE;
        for(uint64_t i = start; i < total_blocks; i++) {
            pmm_unset_bit(i);
        }
    }

    // 3. Re-mark Kernel area (0-16MB) as used
    uint64_t kernel_limit = 0x1000000 / PAGE_SIZE;
    for (uint64_t i = 0; i < kernel_limit; i++) {
        pmm_set_bit(i);
    }
    
    used_blocks = 0;
    for(uint64_t i=0; i<total_blocks; i++) {
        if(pmm_test_bit(i)) used_blocks++;
    }
}

void* pmm_alloc_block() {
    int64_t frame = pmm_find_first_free();
    if (frame == -1) return 0;
    pmm_set_bit(frame);
    used_blocks++;
    return (void*)(frame * PAGE_SIZE);
}

void pmm_free_block(void* ptr) {
    uint64_t frame = (uint64_t)ptr / PAGE_SIZE;
    pmm_unset_bit(frame);
    used_blocks--;
}

uint64_t pmm_get_free_memory() {
    return (total_blocks - used_blocks) * PAGE_SIZE;
}

uint64_t pmm_get_total_memory() {
    return total_blocks * PAGE_SIZE;
}