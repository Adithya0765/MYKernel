// acpi.h - ACPI Table Parser for Alteo OS
// Finds RSDP, parses RSDT/XSDT, extracts MADT and FADT
#ifndef ACPI_H
#define ACPI_H

#include "stdint.h"

// ACPI table signatures (as 32-bit values for easy comparison)
#define ACPI_SIG_RSDP   0x2052545020445352ULL   // "RSD PTR " (8 bytes)
#define ACPI_SIG_RSDT   0x54445352              // "RSDT"
#define ACPI_SIG_XSDT   0x54445358              // "XSDT"
#define ACPI_SIG_MADT   0x43495041              // "APIC"
#define ACPI_SIG_FADT   0x50434146              // "FACP"
#define ACPI_SIG_HPET   0x54455048              // "HPET"
#define ACPI_SIG_MCFG   0x4746434D              // "MCFG"

// MADT entry types
#define MADT_TYPE_LAPIC             0
#define MADT_TYPE_IOAPIC            1
#define MADT_TYPE_ISO               2   // Interrupt Source Override
#define MADT_TYPE_NMI_SOURCE        3
#define MADT_TYPE_LAPIC_NMI         4
#define MADT_TYPE_LAPIC_OVERRIDE    5
#define MADT_TYPE_X2APIC            9

// MADT flags
#define MADT_FLAG_PCAT_COMPAT       (1 << 0)    // Dual 8259 PIC present

// LAPIC flags
#define MADT_LAPIC_ENABLED          (1 << 0)
#define MADT_LAPIC_ONLINE_CAPABLE   (1 << 1)

// Limits
#define ACPI_MAX_CPUS               64
#define ACPI_MAX_IOAPICS            8
#define ACPI_MAX_ISO                24  // Interrupt Source Overrides

// ---- ACPI Structures (packed, read from memory) ----

// RSDP (Root System Description Pointer)
typedef struct __attribute__((packed)) {
    char     signature[8];      // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          // 0 = ACPI 1.0 (RSDT only), 2 = ACPI 2.0+ (XSDT)
    uint32_t rsdt_address;      // Physical address of RSDT
    // ACPI 2.0+ fields:
    uint32_t length;
    uint64_t xsdt_address;      // Physical address of XSDT
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

// Standard ACPI table header
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

// MADT (Multiple APIC Description Table) header
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;    // Physical address of Local APIC
    uint32_t flags;                 // PCAT compat flag
} acpi_madt_t;

// MADT entry header
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} madt_entry_header_t;

// MADT: Local APIC entry (type 0)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;     // bit 0 = enabled, bit 1 = online capable
} madt_lapic_t;

// MADT: I/O APIC entry (type 1)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;    // Physical address of I/O APIC
    uint32_t gsi_base;          // Global System Interrupt base
} madt_ioapic_t;

// MADT: Interrupt Source Override entry (type 2)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t  bus_source;        // Always 0 (ISA)
    uint8_t  irq_source;       // ISA IRQ number
    uint32_t gsi;               // GSI this IRQ maps to
    uint16_t flags;             // Polarity (bits 0-1) and trigger mode (bits 2-3)
} madt_iso_t;

// MADT: Local APIC NMI entry (type 4)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t  acpi_processor_id; // 0xFF = all processors
    uint16_t flags;
    uint8_t  lint;              // LINT# (0 or 1)
} madt_lapic_nmi_t;

// FADT (Fixed ACPI Description Table) — abbreviated, only the fields we need
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t  pm1_event_length;
    uint8_t  pm1_control_length;
    uint8_t  pm2_control_length;
    uint8_t  pm_timer_length;
    uint8_t  gpe0_length;
    uint8_t  gpe1_length;
    uint8_t  gpe1_base;
    uint8_t  cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
    uint16_t boot_arch_flags;       // ACPI 2.0+
    uint8_t  reserved2;
    uint32_t flags;
    // Generic Address Structure for reset register (ACPI 2.0+)
    uint8_t  reset_reg_space;
    uint8_t  reset_reg_bit_width;
    uint8_t  reset_reg_bit_offset;
    uint8_t  reset_reg_access_size;
    uint64_t reset_reg_address;
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
} acpi_fadt_t;

// ---- Parsed ACPI data structures ----

// CPU info extracted from MADT
typedef struct {
    uint8_t  apic_id;
    uint8_t  acpi_id;
    uint8_t  enabled;
    uint8_t  is_bsp;        // Bootstrap processor (the one running now)
} acpi_cpu_t;

// I/O APIC info extracted from MADT
typedef struct {
    uint8_t  id;
    uint32_t address;       // Physical address
    uint32_t gsi_base;
} acpi_ioapic_info_t;

// Interrupt Source Override
typedef struct {
    uint8_t  bus;
    uint8_t  irq;           // ISA IRQ
    uint32_t gsi;           // GSI it maps to
    uint16_t flags;         // Polarity + trigger mode
} acpi_iso_t;

// ---- API ----

// Initialize ACPI: find RSDP, parse tables
int acpi_init(void);

// Get Local APIC base address
uint32_t acpi_get_lapic_address(void);

// CPU info
int acpi_get_cpu_count(void);
acpi_cpu_t* acpi_get_cpus(void);

// I/O APIC info
int acpi_get_ioapic_count(void);
acpi_ioapic_info_t* acpi_get_ioapics(void);

// Interrupt Source Override info
int acpi_get_iso_count(void);
acpi_iso_t* acpi_get_isos(void);

// FADT info
acpi_fadt_t* acpi_get_fadt(void);
int acpi_has_8259(void);    // MADT PCAT compat flag

// Shutdown / reboot via ACPI
void acpi_shutdown(void);
void acpi_reboot(void);

#endif
