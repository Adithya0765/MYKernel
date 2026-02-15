// heap.c - Simple Kernel Heap
#include "heap.h"
#include "pmm.h"

// Header for each heap block
typedef struct header {
    size_t size;
    uint8_t is_free;
    struct header* next;
} header_t;

static header_t* head;

// Start heap at 100MB
#define HEAP_START 0x6400000 

void heap_init() {
    head = (header_t*)HEAP_START;
    head->size = 0; // Initial empty state
    head->is_free = 0;
    head->next = 0;
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;
    
    // Align size to 8 bytes
    size_t aligned_size = (size + 7) & ~7;
    size_t total_size = aligned_size + sizeof(header_t);
    
    header_t* curr = head;
    
    // 1. Find a free block
    while (curr) {
        if (curr->is_free && curr->size >= total_size) {
            // Split block logic could go here
            curr->is_free = 0;
            return (void*)(curr + 1);
        }
        if (curr->next == 0) break; // Reached end
        curr = curr->next;
    }
    
    // 2. No free block, extend heap (Ask PMM for more RAM)
    // For simplicity, we just increment the pointer in our large identity mapped space
    // In a full VMM, we would map a new page here.
    
    header_t* new_block = (curr == head && curr->size == 0) ? head : (header_t*)((uint64_t)curr + sizeof(header_t) + curr->size);
    
    new_block->size = aligned_size;
    new_block->is_free = 0;
    new_block->next = 0;
    
    if (curr != new_block) {
        curr->next = new_block;
    }
    
    return (void*)(new_block + 1);
}

void kfree(void* ptr) {
    if (!ptr) return;
    header_t* header = (header_t*)ptr - 1;
    header->is_free = 1;
    
    // TODO: Merge adjacent free blocks to prevent fragmentation
}