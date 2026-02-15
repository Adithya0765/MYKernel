// idt.h - Interrupt Descriptor Table (64-bit)
#ifndef IDT_H
#define IDT_H

#include "stdint.h"

// IDT entry structure for 64-bit
struct idt_entry {
    uint16_t base_low;      // Lower 16 bits of handler address
    uint16_t selector;      // Kernel segment selector
    uint8_t  ist;           // Interrupt Stack Table (0 = don't switch stack)
    uint8_t  flags;         // Flags
    uint16_t base_mid;      // Middle 16 bits of handler address
    uint32_t base_high;     // Upper 32 bits of handler address
    uint32_t reserved;      // Reserved, must be 0
} __attribute__((packed));

// IDT pointer structure for 64-bit
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Initialize IDT
void idt_init();

// Set an IDT gate (64-bit address)
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

#endif