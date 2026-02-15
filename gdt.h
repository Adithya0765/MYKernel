// gdt.h - Global Descriptor Table and TSS for Alteo OS
// Provides kernel/user segments and Task State Segment for Ring 3 support
#ifndef GDT_H
#define GDT_H

#include "stdint.h"

// Segment selectors
#define GDT_NULL          0x00
#define GDT_KERNEL_CODE   0x08   // Ring 0 code (64-bit)
#define GDT_KERNEL_DATA   0x10   // Ring 0 data
#define GDT_USER_DATA     0x18   // Ring 3 data (SS for SYSRET)
#define GDT_USER_CODE     0x20   // Ring 3 code (CS for SYSRET)
#define GDT_TSS           0x28   // Task State Segment (16 bytes, spans 2 GDT slots)

// User-mode selectors with RPL=3
#define GDT_USER_DATA_RPL3  (GDT_USER_DATA | 3)  // 0x1B
#define GDT_USER_CODE_RPL3  (GDT_USER_CODE | 3)  // 0x23

// GDT entry count: null + kernel_code + kernel_data + user_data + user_code + tss(2 slots)
#define GDT_ENTRY_COUNT 7

// Task State Segment (64-bit)
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;          // Stack pointer for transition to Ring 0
    uint64_t rsp1;          // Stack pointer for Ring 1 (unused)
    uint64_t rsp2;          // Stack pointer for Ring 2 (unused)
    uint64_t reserved1;
    uint64_t ist1;          // Interrupt Stack Table entry 1
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    // I/O Map Base Address
} __attribute__((packed)) tss_t;

// Initialize GDT with all required segments and TSS
void gdt_init(void);

// Update TSS RSP0 (called during context switch to set kernel stack for current process)
void tss_set_rsp0(uint64_t rsp0);

// Get current TSS RSP0
uint64_t tss_get_rsp0(void);

// Assembly functions (in gdt_asm.asm)
extern void gdt_flush(uint64_t gdt_ptr);
extern void tss_flush(uint16_t tss_selector);

#endif
