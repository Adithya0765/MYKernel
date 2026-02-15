// acpi.c - ACPI Table Parser for Alteo OS
// Finds RSDP in BIOS memory regions, parses RSDT/XSDT, extracts MADT and FADT
#include "acpi.h"

// ---- Port I/O ----
static inline void acpi_outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void acpi_outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t acpi_inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ---- Parsed Data ----
static acpi_cpu_t         cpus[ACPI_MAX_CPUS];
static int                cpu_count = 0;
static acpi_ioapic_info_t ioapics[ACPI_MAX_IOAPICS];
static int                ioapic_count = 0;
static acpi_iso_t         isos[ACPI_MAX_ISO];
static int                iso_count = 0;
static uint32_t           lapic_address = 0;
static int                has_8259 = 0;
static acpi_fadt_t*       fadt_ptr = (acpi_fadt_t*)0;
static int                acpi_initialized = 0;

// ---- Helpers ----

static void acpi_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static int acpi_memcmp(const void* a, const void* b, uint64_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (uint64_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

// Validate checksum (sum of all bytes should be 0)
static int acpi_checksum_valid(const void* data, uint32_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return (sum == 0);
}

// ---- RSDP Discovery ----

// Search a region of memory for RSDP signature
static acpi_rsdp_t* acpi_find_rsdp_in_range(uint64_t start, uint64_t length) {
    // RSDP is always 16-byte aligned
    for (uint64_t addr = start; addr < start + length; addr += 16) {
        const char* ptr = (const char*)(uintptr_t)addr;
        if (ptr[0] == 'R' && ptr[1] == 'S' && ptr[2] == 'D' && ptr[3] == ' ' &&
            ptr[4] == 'P' && ptr[5] == 'T' && ptr[6] == 'R' && ptr[7] == ' ') {
            // Validate ACPI 1.0 checksum (first 20 bytes)
            if (acpi_checksum_valid(ptr, 20)) {
                return (acpi_rsdp_t*)(uintptr_t)addr;
            }
        }
    }
    return (acpi_rsdp_t*)0;
}

// Find RSDP by searching standard BIOS memory regions
static acpi_rsdp_t* acpi_find_rsdp(void) {
    acpi_rsdp_t* rsdp;

    // Search EBDA (Extended BIOS Data Area)
    // EBDA base segment is at physical address 0x040E (BDA)
    uint16_t ebda_seg = *(volatile uint16_t*)(uintptr_t)0x040E;
    uint64_t ebda_base = (uint64_t)ebda_seg << 4;
    if (ebda_base >= 0x80000 && ebda_base < 0xA0000) {
        rsdp = acpi_find_rsdp_in_range(ebda_base, 1024);
        if (rsdp) return rsdp;
    }

    // Search BIOS ROM area: 0xE0000 - 0xFFFFF
    rsdp = acpi_find_rsdp_in_range(0xE0000, 0x20000);
    if (rsdp) return rsdp;

    return (acpi_rsdp_t*)0;
}

// ---- Table Parsing ----

// Find a table with a given 4-byte signature in RSDT
static acpi_sdt_header_t* acpi_find_table_rsdt(uint32_t rsdt_addr, const char* sig) {
    acpi_sdt_header_t* rsdt = (acpi_sdt_header_t*)(uintptr_t)rsdt_addr;

    if (!acpi_checksum_valid(rsdt, rsdt->length)) {
        return (acpi_sdt_header_t*)0;
    }

    int entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
    uint32_t* table_ptrs = (uint32_t*)((uintptr_t)rsdt + sizeof(acpi_sdt_header_t));

    for (int i = 0; i < entries; i++) {
        acpi_sdt_header_t* table = (acpi_sdt_header_t*)(uintptr_t)table_ptrs[i];
        if (acpi_memcmp(table->signature, sig, 4) == 0) {
            if (acpi_checksum_valid(table, table->length)) {
                return table;
            }
        }
    }
    return (acpi_sdt_header_t*)0;
}

// Find a table with a given 4-byte signature in XSDT
static acpi_sdt_header_t* acpi_find_table_xsdt(uint64_t xsdt_addr, const char* sig) {
    acpi_sdt_header_t* xsdt = (acpi_sdt_header_t*)(uintptr_t)xsdt_addr;

    if (!acpi_checksum_valid(xsdt, xsdt->length)) {
        return (acpi_sdt_header_t*)0;
    }

    int entries = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
    uint64_t* table_ptrs = (uint64_t*)((uintptr_t)xsdt + sizeof(acpi_sdt_header_t));

    for (int i = 0; i < entries; i++) {
        acpi_sdt_header_t* table = (acpi_sdt_header_t*)(uintptr_t)table_ptrs[i];
        if (acpi_memcmp(table->signature, sig, 4) == 0) {
            if (acpi_checksum_valid(table, table->length)) {
                return table;
            }
        }
    }
    return (acpi_sdt_header_t*)0;
}

// Parse MADT to extract LAPIC, I/O APIC, and ISO info
static void acpi_parse_madt(acpi_madt_t* madt) {
    if (!madt) return;

    lapic_address = madt->local_apic_address;
    has_8259 = (madt->flags & MADT_FLAG_PCAT_COMPAT) ? 1 : 0;

    // Walk MADT entries
    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr < end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;

        if (entry->length == 0) break; // Safety: prevent infinite loop

        switch (entry->type) {
            case MADT_TYPE_LAPIC: {
                madt_lapic_t* lapic = (madt_lapic_t*)entry;
                if (cpu_count < ACPI_MAX_CPUS) {
                    cpus[cpu_count].apic_id = lapic->apic_id;
                    cpus[cpu_count].acpi_id = lapic->acpi_processor_id;
                    cpus[cpu_count].enabled = (lapic->flags & MADT_LAPIC_ENABLED) ? 1 : 0;
                    cpus[cpu_count].is_bsp = (cpu_count == 0) ? 1 : 0; // First LAPIC is BSP
                    cpu_count++;
                }
                break;
            }
            case MADT_TYPE_IOAPIC: {
                madt_ioapic_t* io = (madt_ioapic_t*)entry;
                if (ioapic_count < ACPI_MAX_IOAPICS) {
                    ioapics[ioapic_count].id = io->ioapic_id;
                    ioapics[ioapic_count].address = io->ioapic_address;
                    ioapics[ioapic_count].gsi_base = io->gsi_base;
                    ioapic_count++;
                }
                break;
            }
            case MADT_TYPE_ISO: {
                madt_iso_t* iso = (madt_iso_t*)entry;
                if (iso_count < ACPI_MAX_ISO) {
                    isos[iso_count].bus = iso->bus_source;
                    isos[iso_count].irq = iso->irq_source;
                    isos[iso_count].gsi = iso->gsi;
                    isos[iso_count].flags = iso->flags;
                    iso_count++;
                }
                break;
            }
            case MADT_TYPE_LAPIC_OVERRIDE: {
                // Override the Local APIC address
                uint64_t* override_addr = (uint64_t*)(ptr + 4);
                lapic_address = (uint32_t)(*override_addr);
                break;
            }
            default:
                break;
        }

        ptr += entry->length;
    }
}

// ---- Public API ----

int acpi_init(void) {
    acpi_memset(cpus, 0, sizeof(cpus));
    acpi_memset(ioapics, 0, sizeof(ioapics));
    acpi_memset(isos, 0, sizeof(isos));
    cpu_count = 0;
    ioapic_count = 0;
    iso_count = 0;
    lapic_address = 0;
    has_8259 = 0;
    fadt_ptr = (acpi_fadt_t*)0;

    // Find RSDP
    acpi_rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) {
        return -1; // No ACPI support
    }

    // Determine if we use XSDT (ACPI 2.0+) or RSDT (ACPI 1.0)
    int use_xsdt = (rsdp->revision >= 2 && rsdp->xsdt_address != 0);

    // Find MADT
    acpi_sdt_header_t* madt_header;
    if (use_xsdt) {
        madt_header = acpi_find_table_xsdt(rsdp->xsdt_address, "APIC");
    } else {
        madt_header = acpi_find_table_rsdt(rsdp->rsdt_address, "APIC");
    }

    if (madt_header) {
        acpi_parse_madt((acpi_madt_t*)madt_header);
    }

    // Find FADT
    acpi_sdt_header_t* fadt_header;
    if (use_xsdt) {
        fadt_header = acpi_find_table_xsdt(rsdp->xsdt_address, "FACP");
    } else {
        fadt_header = acpi_find_table_rsdt(rsdp->rsdt_address, "FACP");
    }

    if (fadt_header) {
        fadt_ptr = (acpi_fadt_t*)fadt_header;
    }

    acpi_initialized = 1;
    return 0;
}

uint32_t acpi_get_lapic_address(void) {
    return lapic_address;
}

int acpi_get_cpu_count(void) {
    return cpu_count;
}

acpi_cpu_t* acpi_get_cpus(void) {
    return cpus;
}

int acpi_get_ioapic_count(void) {
    return ioapic_count;
}

acpi_ioapic_info_t* acpi_get_ioapics(void) {
    return ioapics;
}

int acpi_get_iso_count(void) {
    return iso_count;
}

acpi_iso_t* acpi_get_isos(void) {
    return isos;
}

acpi_fadt_t* acpi_get_fadt(void) {
    return fadt_ptr;
}

int acpi_has_8259(void) {
    return has_8259;
}

void acpi_shutdown(void) {
    if (!fadt_ptr) return;

    // ACPI shutdown: write SLP_TYPa | SLP_EN to PM1a_CNT
    // SLP_TYPa for S5 (soft off) is typically 0x2000, SLP_EN is bit 13 (0x2000)
    // This is a simplification — real shutdown requires parsing \_S5 in DSDT/AML
    uint16_t pm1a_cnt = (uint16_t)fadt_ptr->pm1a_control_block;
    if (pm1a_cnt) {
        // S5 = power off, typical value: SLP_TYPa=5 shifted left 10, SLP_EN=1<<13
        acpi_outw(pm1a_cnt, (5 << 10) | (1 << 13));
    }

    // If that didn't work, try common QEMU/Bochs shutdown
    acpi_outw(0x604, 0x2000);

    // Last resort: keyboard controller reset
    acpi_outb(0x64, 0xFE);

    // Hang if nothing worked
    for (;;) __asm__ __volatile__("hlt");
}

void acpi_reboot(void) {
    if (fadt_ptr && fadt_ptr->header.length >= 129 && fadt_ptr->reset_reg_address) {
        // ACPI 2.0+ reset register
        if (fadt_ptr->reset_reg_space == 1) {
            // System I/O space
            acpi_outb((uint16_t)fadt_ptr->reset_reg_address, fadt_ptr->reset_value);
        } else if (fadt_ptr->reset_reg_space == 0) {
            // System memory space
            *(volatile uint8_t*)(uintptr_t)fadt_ptr->reset_reg_address = fadt_ptr->reset_value;
        }
    }

    // Fallback: keyboard controller reset
    acpi_outb(0x64, 0xFE);

    // Hang if nothing worked
    for (;;) __asm__ __volatile__("hlt");
}
