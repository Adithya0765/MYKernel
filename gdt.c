// gdt.c - Global Descriptor Table and TSS for Alteo OS
// Sets up kernel/user segments and TSS for Ring 0/Ring 3 separation
#include "gdt.h"

// GDT entry structure (8 bytes)
typedef struct {
    uint16_t limit_low;     // Limit 0:15
    uint16_t base_low;      // Base 0:15
    uint8_t  base_mid;      // Base 16:23
    uint8_t  access;        // Access byte (P, DPL, S, Type)
    uint8_t  flags_limit;   // Flags (G, D/B, L, AVL) | Limit 16:19
    uint8_t  base_high;     // Base 24:31
} __attribute__((packed)) gdt_entry_t;

// GDT pointer structure
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

// GDT entries (7 slots: null, kcode, kdata, udata, ucode, tss_lo, tss_hi)
static gdt_entry_t gdt_entries[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static gdt_ptr_t gdt_ptr;

// Task State Segment
static tss_t tss __attribute__((aligned(16)));

// Kernel interrupt stack (used by TSS RSP0 when transitioning from Ring 3)
static uint8_t kernel_interrupt_stack[8192] __attribute__((aligned(16)));

// Helper: set a standard GDT entry
static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    gdt_entries[index].limit_low   = limit & 0xFFFF;
    gdt_entries[index].base_low    = base & 0xFFFF;
    gdt_entries[index].base_mid    = (base >> 16) & 0xFF;
    gdt_entries[index].access      = access;
    gdt_entries[index].flags_limit = ((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F);
    gdt_entries[index].base_high   = (base >> 24) & 0xFF;
}

void gdt_init(void) {
    // Clear TSS
    uint8_t* tss_ptr = (uint8_t*)&tss;
    for (uint64_t i = 0; i < sizeof(tss_t); i++) tss_ptr[i] = 0;

    // Set TSS RSP0 to kernel interrupt stack
    tss.rsp0 = (uint64_t)&kernel_interrupt_stack[8192];
    tss.iomap_base = sizeof(tss_t);

    // --- GDT Entries ---

    // Entry 0: Null descriptor
    gdt_set_entry(0, 0, 0, 0, 0);

    // Entry 1 (0x08): Kernel Code 64-bit
    // Access: P=1, DPL=00, S=1, Type=1010 (Execute/Read) = 0x9A
    // Flags:  G=0, D=0, L=1 (Long mode), AVL=0 = 0x02
    gdt_set_entry(1, 0, 0, 0x9A, 0x02);

    // Entry 2 (0x10): Kernel Data
    // Access: P=1, DPL=00, S=1, Type=0010 (Read/Write) = 0x92
    // Flags:  G=0, D=0, L=0, AVL=0 = 0x00
    gdt_set_entry(2, 0, 0, 0x92, 0x00);

    // Entry 3 (0x18): User Data (DPL=3)
    // Access: P=1, DPL=11, S=1, Type=0010 (Read/Write) = 0xF2
    // Flags:  G=0, D=0, L=0, AVL=0 = 0x00
    gdt_set_entry(3, 0, 0, 0xF2, 0x00);

    // Entry 4 (0x20): User Code 64-bit (DPL=3)
    // Access: P=1, DPL=11, S=1, Type=1010 (Execute/Read) = 0xFA
    // Flags:  G=0, D=0, L=1 (Long mode), AVL=0 = 0x02
    gdt_set_entry(4, 0, 0, 0xFA, 0x02);

    // Entry 5-6 (0x28): TSS descriptor (16 bytes in 64-bit mode)
    // The TSS descriptor is a system segment (S=0) and takes two GDT slots
    uint64_t tss_addr = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss_t) - 1;

    // Low 8 bytes of TSS descriptor
    // Access: P=1, DPL=00, S=0, Type=1001 (Available 64-bit TSS) = 0x89
    // Flags:  G=0, D=0, L=0, AVL=0 = 0x00
    gdt_entries[5].limit_low   = tss_limit & 0xFFFF;
    gdt_entries[5].base_low    = tss_addr & 0xFFFF;
    gdt_entries[5].base_mid    = (tss_addr >> 16) & 0xFF;
    gdt_entries[5].access      = 0x89;
    gdt_entries[5].flags_limit = ((tss_limit >> 16) & 0x0F);
    gdt_entries[5].base_high   = (tss_addr >> 24) & 0xFF;

    // High 8 bytes of TSS descriptor (base bits 32-63 + reserved)
    // We cast the GDT entry slot to raw uint32_t* to write the upper base
    uint32_t* tss_high = (uint32_t*)&gdt_entries[6];
    tss_high[0] = (uint32_t)(tss_addr >> 32);  // Base 32:63
    tss_high[1] = 0;                            // Reserved

    // Set up GDT pointer
    gdt_ptr.limit = (sizeof(gdt_entry_t) * GDT_ENTRY_COUNT) - 1;
    gdt_ptr.base = (uint64_t)&gdt_entries;

    // Load GDT and reload segment registers
    gdt_flush((uint64_t)&gdt_ptr);

    // Load TSS
    tss_flush(GDT_TSS);
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

uint64_t tss_get_rsp0(void) {
    return tss.rsp0;
}
