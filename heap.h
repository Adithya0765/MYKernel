// heap.h - Kernel Heap
#ifndef HEAP_H
#define HEAP_H

#include "stdint.h"

void heap_init();
void* kmalloc(size_t size);
void kfree(void* ptr);

#endif