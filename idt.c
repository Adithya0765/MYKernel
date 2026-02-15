// idt.c - IDT implementation (64-bit)
#include "idt.h"

// IDT with 256 entries
struct idt_entry idt_entries[256];
struct idt_ptr idtp;

// External assembly function to load IDT
extern void idt_flush(uint64_t);

// Set an IDT gate (64-bit)
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_low = base & 0xFFFF;
    idt_entries[num].base_mid = (base >> 16) & 0xFFFF;
    idt_entries[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt_entries[num].selector = sel;
    idt_entries[num].ist = 0;
    idt_entries[num].flags = flags;
    idt_entries[num].reserved = 0;
}

// Initialize the IDT
void idt_init() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint64_t)&idt_entries;

    // Clear IDT
    for(int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // Load the IDT
    idt_flush((uint64_t)&idtp);
}