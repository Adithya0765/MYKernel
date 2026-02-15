// apic.c - Local APIC + I/O APIC Driver for Alteo OS
// Replaces legacy 8259 PIC with APIC interrupt routing
#include "apic.h"
#include "acpi.h"

// ---- Port I/O ----
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ---- MSR Access ----
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ __volatile__("wrmsr" : : "a"((uint32_t)val), "d"((uint32_t)(val >> 32)), "c"(msr));
}

#define IA32_APIC_BASE_MSR      0x1B
#define IA32_APIC_BASE_ENABLE   (1 << 11)

// ---- State ----
static volatile uint32_t* lapic_base = 0;
static volatile uint32_t* ioapic_base = 0;
static int apic_active = 0;
static uint32_t ioapic_max_redir = 0;    // Max redirection entries
static uint32_t lapic_timer_ticks = 0;   // Calibrated ticks per interval

// ---- Local APIC Register Access ----

uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

// ---- I/O APIC Register Access ----

static uint32_t ioapic_read(uint32_t reg) {
    ioapic_base[0] = reg;              // IOREGSEL
    return ioapic_base[4];             // IOWIN (at offset 0x10 = index 4)
}

static void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic_base[0] = reg;              // IOREGSEL
    ioapic_base[4] = val;             // IOWIN
}

// ---- PIC Disable ----

static void pic_disable(void) {
    // Mask all IRQs on both PICs
    outb(0x21, 0xFF);  // PIC1 data
    outb(0xA1, 0xFF);  // PIC2 data
}

// ---- Calibration ----

// Simple delay using PIT channel 2 for APIC timer calibration
static void pit_delay_10ms(void) {
    // PIT frequency = 1193182 Hz
    // 10ms = 11932 ticks
    uint16_t count = 11932;

    // Channel 2, mode 0 (terminal count), binary
    outb(0x61, (inb(0x61) & 0xFD) | 0x01); // Enable speaker gate, disable speaker
    outb(0x43, 0xB0);   // Channel 2, lobyte/hibyte, mode 0
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));

    // Reset the flipflop
    uint8_t tmp = inb(0x61) & 0xFE;
    outb(0x61, tmp);
    outb(0x61, tmp | 1);

    // Wait for PIT output to go high
    while (!(inb(0x61) & 0x20));
}

// ---- I/O APIC IRQ Configuration ----

// Get the GSI for a given ISA IRQ (accounts for Interrupt Source Overrides)
static uint32_t irq_to_gsi(uint8_t irq, uint16_t* flags_out) {
    int iso_count = acpi_get_iso_count();
    acpi_iso_t* isos = acpi_get_isos();

    for (int i = 0; i < iso_count; i++) {
        if (isos[i].irq == irq) {
            if (flags_out) *flags_out = isos[i].flags;
            return isos[i].gsi;
        }
    }

    // No override — identity map
    if (flags_out) *flags_out = 0;
    return (uint32_t)irq;
}

void ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id, uint32_t flags) {
    if (!ioapic_base) return;

    uint16_t iso_flags = 0;
    uint32_t gsi = irq_to_gsi(irq, &iso_flags);

    if (gsi > ioapic_max_redir) return;

    // Build the redirection entry
    uint64_t redir = (uint64_t)vector;

    // Apply polarity from ISO flags
    uint8_t polarity = iso_flags & 0x03;
    if (polarity == 3) {
        redir |= IOAPIC_RED_ACTIVE_LOW;
    }
    // else: default (active high) or conforms to bus spec

    // Apply trigger mode from ISO flags
    uint8_t trigger = (iso_flags >> 2) & 0x03;
    if (trigger == 3) {
        redir |= IOAPIC_RED_LEVEL;
    }
    // else: default (edge) or conforms to bus spec

    // Apply additional caller flags
    redir |= flags;

    // Set destination APIC ID in bits 56-63
    redir |= ((uint64_t)dest_apic_id << 56);

    // Write to redirection table (two 32-bit writes)
    uint32_t reg_lo = IOAPIC_REG_REDTBL + gsi * 2;
    uint32_t reg_hi = IOAPIC_REG_REDTBL + gsi * 2 + 1;

    ioapic_write(reg_hi, (uint32_t)(redir >> 32));
    ioapic_write(reg_lo, (uint32_t)(redir & 0xFFFFFFFF));
}

void ioapic_mask_irq(uint8_t irq) {
    if (!ioapic_base) return;

    uint16_t iso_flags = 0;
    uint32_t gsi = irq_to_gsi(irq, &iso_flags);
    if (gsi > ioapic_max_redir) return;

    uint32_t reg_lo = IOAPIC_REG_REDTBL + gsi * 2;
    uint32_t val = ioapic_read(reg_lo);
    val |= IOAPIC_RED_MASKED;
    ioapic_write(reg_lo, val);
}

void ioapic_unmask_irq(uint8_t irq) {
    if (!ioapic_base) return;

    uint16_t iso_flags = 0;
    uint32_t gsi = irq_to_gsi(irq, &iso_flags);
    if (gsi > ioapic_max_redir) return;

    uint32_t reg_lo = IOAPIC_REG_REDTBL + gsi * 2;
    uint32_t val = ioapic_read(reg_lo);
    val &= ~IOAPIC_RED_MASKED;
    ioapic_write(reg_lo, val);
}

// ---- Initialization ----

int apic_init(void) {
    // Get APIC info from ACPI
    uint32_t lapic_phys = acpi_get_lapic_address();
    int num_ioapics = acpi_get_ioapic_count();
    acpi_ioapic_info_t* ioapic_info = acpi_get_ioapics();

    // If no LAPIC address found, ACPI might not have been initialized or no APIC available
    if (lapic_phys == 0 || num_ioapics == 0) {
        apic_active = 0;
        return -1; // Fall back to PIC
    }

    // Map the Local APIC base address (identity-mapped in our kernel)
    lapic_base = (volatile uint32_t*)(uintptr_t)lapic_phys;

    // Map the first I/O APIC (most systems have only one)
    ioapic_base = (volatile uint32_t*)(uintptr_t)ioapic_info[0].address;

    // Get I/O APIC max redirection entries
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_redir = (ver >> 16) & 0xFF;

    // Ensure LAPIC is enabled via MSR
    uint64_t apic_msr = rdmsr(IA32_APIC_BASE_MSR);
    apic_msr |= IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, apic_msr);

    // Disable the legacy 8259 PIC
    pic_disable();

    // Set the Spurious Interrupt Vector Register to enable the APIC
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);

    // Clear Task Priority to accept all interrupts
    lapic_write(LAPIC_TPR, 0);

    // Mask all I/O APIC entries first
    for (uint32_t i = 0; i <= ioapic_max_redir; i++) {
        uint32_t reg_lo = IOAPIC_REG_REDTBL + i * 2;
        ioapic_write(reg_lo, IOAPIC_RED_MASKED);
    }

    // Get BSP APIC ID
    uint32_t bsp_id = lapic_get_id();

    // Route standard ISA IRQs through I/O APIC
    // IRQ 0 = Timer (vector 0x20)
    ioapic_set_irq(0, APIC_IRQ_BASE + 0, bsp_id, 0);
    // IRQ 1 = Keyboard (vector 0x21)
    ioapic_set_irq(1, APIC_IRQ_BASE + 1, bsp_id, 0);
    // IRQ 2 = Cascade (not needed with APIC, but set up anyway)
    // IRQ 12 = Mouse (vector 0x2C)
    ioapic_set_irq(12, APIC_IRQ_BASE + 12, bsp_id, 0);
    // IRQ 14 = Primary ATA (vector 0x2E)
    ioapic_set_irq(14, APIC_IRQ_BASE + 14, bsp_id, 0);
    // IRQ 15 = Secondary ATA (vector 0x2F)
    ioapic_set_irq(15, APIC_IRQ_BASE + 15, bsp_id, 0);

    apic_active = 1;
    return 0;
}

int apic_is_active(void) {
    return apic_active;
}

void lapic_eoi(void) {
    if (lapic_base) {
        lapic_write(LAPIC_EOI, 0);
    }
}

uint32_t lapic_get_id(void) {
    if (!lapic_base) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_send_ipi(uint8_t dest_apic_id, uint32_t flags) {
    if (!lapic_base) return;

    lapic_write(LAPIC_ICR_HI, ((uint32_t)dest_apic_id << 24));
    lapic_write(LAPIC_ICR_LO, flags);

    // Wait for delivery
    while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_PENDING);
}

void lapic_send_init(uint8_t dest_apic_id) {
    lapic_send_ipi(dest_apic_id, LAPIC_ICR_INIT | LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL);
    // De-assert
    lapic_send_ipi(dest_apic_id, LAPIC_ICR_INIT | LAPIC_ICR_DEASSERT | LAPIC_ICR_LEVEL);
}

void lapic_send_sipi(uint8_t dest_apic_id, uint8_t vector) {
    lapic_send_ipi(dest_apic_id, LAPIC_ICR_STARTUP | (uint32_t)vector);
}

void lapic_timer_init(uint32_t frequency_hz) {
    if (!lapic_base) return;
    (void)frequency_hz;

    // Set divider to 16
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);

    // Calibrate: run timer with max count for 10ms, measure how far it counts
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_MASKED | LAPIC_TIMER_ONESHOT);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    pit_delay_10ms();

    // Stop timer and read remaining count
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_MASKED);
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

    // elapsed = ticks in 10ms
    // For target frequency: ticks_per_interval = elapsed * (1000 / 10) / frequency_hz
    //                     = elapsed * 100 / frequency_hz
    if (frequency_hz == 0) frequency_hz = 1000;
    lapic_timer_ticks = elapsed * 100 / frequency_hz;
    if (lapic_timer_ticks == 0) lapic_timer_ticks = 1;

    // Set up periodic timer with vector APIC_TIMER_VECTOR (0x20 = same as IRQ0)
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_ICR, lapic_timer_ticks);
}
