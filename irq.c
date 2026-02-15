// irq.c - Interrupt Request Manager
#include "irq.h"
#include "idt.h"
#include "isr.h"
#include "apic.h"

// PIC Ports
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

void (*irq_handlers[16])(registers_t*) = {0};

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Send End-Of-Interrupt to PIC or APIC
void irq_send_eoi(uint8_t irq) {
    if (apic_is_active()) {
        lapic_eoi();
    } else {
        if (irq >= 8) {
            outb(PIC2_COMMAND, PIC_EOI);
        }
        outb(PIC1_COMMAND, PIC_EOI);
    }
}

// Initialize PIC
void irq_remap() {
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    // Start Init
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    
    // Remap offsets (IRQ 0-7 -> 32-39, IRQ 8-15 -> 40-47)
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    
    // Setup Cascade
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    
    // Environment mode (8086)
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    
    // Restore masks (but we will unmask later)
    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

// Unmask a specific interrupt
void irq_clear_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

// Enable basic interrupts (Timer, Keyboard, Cascade, Mouse)
void irq_enable_all() {
    // Mask everything first
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    
    // Enable IRQ 0 (Timer)
    irq_clear_mask(0);
    // Enable IRQ 1 (Keyboard)
    irq_clear_mask(1);
    // Enable IRQ 2 (Cascade - Required for Mouse)
    irq_clear_mask(2);
    // Enable IRQ 12 (Mouse)
    irq_clear_mask(12);
}

// Main Handler called from Assembly
void irq_handler(registers_t* regs) {
    int irq = regs->int_no - 32;
    
    // Call the specific handler if it exists
    if (irq_handlers[irq] != 0) {
        irq_handlers[irq](regs);
    }
    
    // CRITICAL: Send EOI or system will freeze
    irq_send_eoi(irq);
}

void irq_install_handler(int irq, void (*handler)(registers_t*)) {
    irq_handlers[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    irq_handlers[irq] = 0;
}

void irq_init() {
    irq_remap();
    
    // Install dummy handlers for all IRQs (64-bit)
    idt_set_gate(32, (uint64_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint64_t)irq1, 0x08, 0x8E);
    idt_set_gate(34, (uint64_t)irq2, 0x08, 0x8E);
    idt_set_gate(35, (uint64_t)irq3, 0x08, 0x8E);
    idt_set_gate(36, (uint64_t)irq4, 0x08, 0x8E);
    idt_set_gate(37, (uint64_t)irq5, 0x08, 0x8E);
    idt_set_gate(38, (uint64_t)irq6, 0x08, 0x8E);
    idt_set_gate(39, (uint64_t)irq7, 0x08, 0x8E);
    idt_set_gate(40, (uint64_t)irq8, 0x08, 0x8E);
    idt_set_gate(41, (uint64_t)irq9, 0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);
}