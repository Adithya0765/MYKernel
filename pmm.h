// pmm.h - Physical Memory Manager
#ifndef PMM_H
#define PMM_H

#include "stdint.h"
#include "graphics.h"

#define PAGE_SIZE 4096

void pmm_init(uint64_t multiboot_info_addr);
void* pmm_alloc_block();
void pmm_free_block(void* ptr);
uint64_t pmm_get_free_memory();
uint64_t pmm_get_total_memory();

#endif