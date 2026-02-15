// apic.h - Local APIC + I/O APIC Driver for Alteo OS
// Replaces legacy 8259 PIC with APIC for proper interrupt routing
#ifndef APIC_H
#define APIC_H

#include "stdint.h"

// Local APIC register offsets (memory-mapped, relative to LAPIC base)
#define LAPIC_ID            0x020   // Local APIC ID
#define LAPIC_VERSION       0x030   // Local APIC Version
#define LAPIC_TPR           0x080   // Task Priority Register
#define LAPIC_APR           0x090   // Arbitration Priority
#define LAPIC_PPR           0x0A0   // Processor Priority
#define LAPIC_EOI           0x0B0   // End Of Interrupt
#define LAPIC_RRD           0x0C0   // Remote Read
#define LAPIC_LDR           0x0D0   // Logical Destination
#define LAPIC_DFR           0x0E0   // Destination Format
#define LAPIC_SVR           0x0F0   // Spurious Interrupt Vector
#define LAPIC_ISR           0x100   // In-Service Register (8x 32-bit)
#define LAPIC_TMR           0x180   // Trigger Mode Register
#define LAPIC_IRR           0x200   // Interrupt Request Register
#define LAPIC_ESR           0x280   // Error Status
#define LAPIC_CMCI          0x2F0   // Corrected Machine Check Interrupt
#define LAPIC_ICR_LO        0x300   // Interrupt Command (low 32 bits)
#define LAPIC_ICR_HI        0x310   // Interrupt Command (high 32 bits)
#define LAPIC_TIMER         0x320   // Timer LVT
#define LAPIC_THERMAL       0x330   // Thermal Sensor LVT
#define LAPIC_PERF          0x340   // Performance Counter LVT
#define LAPIC_LINT0         0x350   // Local Interrupt 0 (LINT0)
#define LAPIC_LINT1         0x360   // Local Interrupt 1 (LINT1)
#define LAPIC_ERROR         0x370   // Error LVT
#define LAPIC_TIMER_ICR     0x380   // Timer Initial Count
#define LAPIC_TIMER_CCR     0x390   // Timer Current Count
#define LAPIC_TIMER_DCR     0x3E0   // Timer Divide Configuration

// SVR bits
#define LAPIC_SVR_ENABLE    (1 << 8)    // Software enable APIC
#define LAPIC_SVR_VECTOR    0xFF        // Spurious vector number

// Timer modes
#define LAPIC_TIMER_ONESHOT     0x00000000
#define LAPIC_TIMER_PERIODIC    0x00020000
#define LAPIC_TIMER_TSC         0x00040000
#define LAPIC_TIMER_MASKED      0x00010000

// Timer divide values
#define LAPIC_TIMER_DIV_1       0x0B
#define LAPIC_TIMER_DIV_2       0x00
#define LAPIC_TIMER_DIV_4       0x01
#define LAPIC_TIMER_DIV_8       0x02
#define LAPIC_TIMER_DIV_16      0x03
#define LAPIC_TIMER_DIV_32      0x08
#define LAPIC_TIMER_DIV_64      0x09
#define LAPIC_TIMER_DIV_128     0x0A

// ICR delivery modes
#define LAPIC_ICR_FIXED         0x00000000
#define LAPIC_ICR_SMI           0x00000200
#define LAPIC_ICR_NMI           0x00000400
#define LAPIC_ICR_INIT          0x00000500
#define LAPIC_ICR_STARTUP       0x00000600

// ICR destination shorthand
#define LAPIC_ICR_SELF          0x00040000
#define LAPIC_ICR_ALL           0x00080000
#define LAPIC_ICR_ALL_EXCL      0x000C0000

// ICR flags
#define LAPIC_ICR_PHYSICAL      0x00000000
#define LAPIC_ICR_LOGICAL       0x00000800
#define LAPIC_ICR_PENDING       0x00001000
#define LAPIC_ICR_ASSERT        0x00004000
#define LAPIC_ICR_DEASSERT      0x00000000
#define LAPIC_ICR_EDGE          0x00000000
#define LAPIC_ICR_LEVEL         0x00008000

// I/O APIC registers (accessed via indirect register select)
#define IOAPIC_REG_ID           0x00    // I/O APIC ID
#define IOAPIC_REG_VER          0x01    // I/O APIC Version
#define IOAPIC_REG_ARB          0x02    // Arbitration ID
#define IOAPIC_REG_REDTBL       0x10    // Redirection Table (base, each entry is 2 regs)

// I/O APIC Redirection Table Entry bits
#define IOAPIC_RED_VECTOR_MASK  0x000000FF
#define IOAPIC_RED_DELMOD_FIXED 0x00000000
#define IOAPIC_RED_DELMOD_LOWPR 0x00000100
#define IOAPIC_RED_DELMOD_SMI   0x00000200
#define IOAPIC_RED_DELMOD_NMI   0x00000400
#define IOAPIC_RED_DELMOD_INIT  0x00000500
#define IOAPIC_RED_DELMOD_EXTINT 0x00000700
#define IOAPIC_RED_DESTMOD_PHYS 0x00000000
#define IOAPIC_RED_DESTMOD_LOG  0x00000800
#define IOAPIC_RED_PENDING      0x00001000
#define IOAPIC_RED_ACTIVE_LOW   0x00002000
#define IOAPIC_RED_ACTIVE_HIGH  0x00000000
#define IOAPIC_RED_LEVEL        0x00008000
#define IOAPIC_RED_EDGE         0x00000000
#define IOAPIC_RED_MASKED       0x00010000

// Interrupt vector assignments
#define APIC_TIMER_VECTOR       0x20    // IRQ 0 equivalent
#define APIC_SPURIOUS_VECTOR    0xFF
#define APIC_IRQ_BASE           0x20    // Same as PIC remapping: IRQ 0 = vector 0x20

// ---- API ----

// Initialize Local APIC and I/O APIC(s) using ACPI MADT data.
// Disables the legacy 8259 PIC and routes interrupts through APIC.
// Falls back to PIC if no APIC is available.
// Returns 0 on success, -1 if APIC not available (PIC remains active).
int apic_init(void);

// Check if APIC is active (vs. legacy PIC)
int apic_is_active(void);

// Send End-Of-Interrupt to Local APIC
void lapic_eoi(void);

// Get current CPU's APIC ID
uint32_t lapic_get_id(void);

// Send IPI (Inter-Processor Interrupt)
void lapic_send_ipi(uint8_t dest_apic_id, uint32_t flags);

// Send INIT IPI to a specific AP
void lapic_send_init(uint8_t dest_apic_id);

// Send SIPI (Startup IPI) to a specific AP
void lapic_send_sipi(uint8_t dest_apic_id, uint8_t vector);

// I/O APIC: set a redirection table entry for an IRQ
void ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id, uint32_t flags);

// I/O APIC: mask/unmask an IRQ
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);

// Start APIC timer with periodic interrupt at ~1000Hz (or specified frequency)
void lapic_timer_init(uint32_t frequency_hz);

// Read/write Local APIC register
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t val);

#endif
