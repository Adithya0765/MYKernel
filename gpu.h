// gpu.h - NVIDIA GPU Driver for Alteo OS
// Stage 1: PCI Discovery + MMIO BAR Mapping + Framebuffer
// Targets NV50 (Tesla) architecture first, with fallback for pre-NV50 and Fermi+
#ifndef GPU_H
#define GPU_H

#include "stdint.h"

// NVIDIA PCI Vendor ID
#define NV_VENDOR_ID            0x10DE

// GPU Architecture generations (from PCI device ID ranges and PMC.BOOT0)
#define NV_ARCH_UNKNOWN         0
#define NV_ARCH_NV04            0x04    // RIVA TNT / TNT2 (1998-1999)
#define NV_ARCH_NV10            0x10    // GeForce 256 / GeForce2 (1999-2001)
#define NV_ARCH_NV20            0x20    // GeForce3 / GeForce4 Ti (2001-2002)
#define NV_ARCH_NV30            0x30    // GeForce FX (2003)
#define NV_ARCH_NV40            0x40    // GeForce 6/7 (2004-2006)
#define NV_ARCH_NV50            0x50    // Tesla: GeForce 8/9/GT200 (2006-2010) — primary target
#define NV_ARCH_NVC0            0xC0    // Fermi: GF100+ (2010-2012)
#define NV_ARCH_NVE0            0xE0    // Kepler: GK104+ (2012-2014)
#define NV_ARCH_GM100           0x110   // Maxwell (2014-2016)
#define NV_ARCH_GP100           0x130   // Pascal (2016-2018)
#define NV_ARCH_GV100           0x140   // Volta (2017-2018)
#define NV_ARCH_TU100           0x160   // Turing (2018-2020)
#define NV_ARCH_GA100           0x170   // Ampere (2020-2022)

// NVIDIA MMIO Register Blocks (BAR0 offsets)
// Reference: envytools MMIO map
#define NV_PMC                  0x000000    // Master Control
#define NV_PBUS                 0x001000    // Bus Control
#define NV_PFIFO                0x002000    // FIFO engine
#define NV_PFIFO_CACHE          0x003000    // FIFO cache
#define NV_PTIMER               0x009000    // Timer / clock
#define NV_PFB                  0x100000    // Framebuffer / Memory controller
#define NV_PEXTDEV              0x101000    // External devices (crystal freq)
#define NV_PROM                 0x300000    // ROM (VBIOS)
#define NV_PGRAPH               0x400000    // Graphics engine
#define NV_PCRTC                0x600000    // CRTC (display timing)
#define NV_PRAMDAC              0x680000    // RAMDAC / PLL
#define NV_PDISPLAY             0x610000    // NV50+ unified display engine
#define NV_PGR                  0x400000    // Fermi+ graphics

// Key PMC registers
#define NV_PMC_BOOT_0           0x000000    // Chip identification
#define NV_PMC_BOOT_1           0x000004    // Additional ID info
#define NV_PMC_ENABLE           0x000200    // Engine enable mask
#define NV_PMC_INTR_0           0x000100    // Pending interrupts
#define NV_PMC_INTR_EN_0        0x000140    // Interrupt enable mask

// PMC interrupt bits
#define NV_PMC_INTR_PFIFO       (1 << 8)
#define NV_PMC_INTR_PGRAPH      (1 << 12)
#define NV_PMC_INTR_PCRTC       (1 << 24)
#define NV_PMC_INTR_PDISPLAY    (1 << 26)
#define NV_PMC_INTR_PTIMER      (1 << 20)
#define NV_PMC_INTR_PBUS        (1 << 28)

// PMC Engine enable bits
#define NV_PMC_ENABLE_PFIFO     (1 << 8)
#define NV_PMC_ENABLE_PGRAPH    (1 << 12)

// PTIMER registers
#define NV_PTIMER_INTR_0        0x009100    // Timer interrupt status
#define NV_PTIMER_INTR_EN_0     0x009140    // Timer interrupt enable
#define NV_PTIMER_NUMERATOR     0x009200    // Clock numerator
#define NV_PTIMER_DENOMINATOR   0x009210    // Clock denominator
#define NV_PTIMER_TIME_0        0x009400    // Current time (low)
#define NV_PTIMER_TIME_1        0x009410    // Current time (high)

// PFB (memory controller) registers
#define NV_PFB_CFG0             0x100200    // Memory configuration 0
#define NV_PFB_CFG1             0x100204    // Memory configuration 1
#define NV_PFB_CSTATUS          0x10020C    // Memory status (size indicator)

// PROM (VBIOS) access
#define NV_PROM_DATA            0x300000    // VBIOS ROM data start

// NV50+ display engine registers
#define NV50_PDISPLAY_INTR_0    0x610020    // Display interrupt status
#define NV50_PDISPLAY_INTR_EN   0x610028    // Display interrupt enable
#define NV50_PDISPLAY_CAPS      0x610010    // Display capabilities

// BAR indices for NVIDIA GPUs
#define NV_BAR0_INDEX           0       // MMIO registers (typically 16-32 MB)
#define NV_BAR1_INDEX           1       // VRAM aperture (maps GPU VRAM into PCI space)
#define NV_BAR2_INDEX           3       // NV50+: RAMIN (instance memory) aperture
// Note: BAR1 may be 64-bit, consuming BAR index 1+2

// GPU VRAM types
#define NV_VRAM_TYPE_UNKNOWN    0
#define NV_VRAM_TYPE_SDR        1
#define NV_VRAM_TYPE_DDR1       2
#define NV_VRAM_TYPE_DDR2       3
#define NV_VRAM_TYPE_DDR3       4
#define NV_VRAM_TYPE_GDDR3      5
#define NV_VRAM_TYPE_GDDR5      6

// Maximum supported display outputs
#define NV_MAX_HEADS            2
#define NV_MAX_CONNECTORS       4

// GPU state structure
typedef struct {
    // PCI identification
    uint16_t pci_device_id;
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;
    uint8_t  pci_irq;

    // Chip identification (from BOOT_0)
    uint32_t chipset;           // Full chipset ID from PMC.BOOT_0
    uint32_t arch;              // Architecture family (NV_ARCH_*)
    uint32_t chiprev;           // Chip revision
    char     chip_name[32];     // Human-readable name

    // BAR mappings
    volatile uint32_t* mmio;    // BAR0: MMIO registers (virtual address)
    uint64_t mmio_phys;         // BAR0 physical base
    uint64_t mmio_size;         // BAR0 size

    volatile uint32_t* vram;    // BAR1: VRAM aperture (virtual address)
    uint64_t vram_phys;         // BAR1 physical base
    uint64_t vram_size;         // BAR1 aperture size (PCI window, NOT total VRAM)

    volatile uint32_t* ramin;   // BAR2/3: Instance memory (NV50+)
    uint64_t ramin_phys;
    uint64_t ramin_size;

    // Memory info
    uint64_t vram_total;        // Total VRAM in bytes (from PFB)
    uint8_t  vram_type;         // VRAM type (NV_VRAM_TYPE_*)

    // Display state
    int      num_heads;         // Number of active CRTC heads
    int      head_active[NV_MAX_HEADS];
    uint32_t fb_offset;         // Offset into VRAM for scanout framebuffer
    int      display_width;
    int      display_height;
    int      display_bpp;
    int      display_pitch;

    // Engine status
    int      pfifo_enabled;
    int      pgraph_enabled;

    // BIOS
    uint32_t vbios_size;

    // State flags
    int      initialized;
    int      mmio_mapped;
    int      vram_mapped;
    int      display_active;
} gpu_state_t;

// ---- MMIO Access Helpers ----
static inline uint32_t nv_rd32(volatile uint32_t* mmio, uint32_t reg) {
    return mmio[reg / 4];
}

static inline void nv_wr32(volatile uint32_t* mmio, uint32_t reg, uint32_t val) {
    mmio[reg / 4] = val;
}

static inline uint32_t nv_mask(volatile uint32_t* mmio, uint32_t reg, uint32_t mask, uint32_t val) {
    uint32_t old = nv_rd32(mmio, reg);
    nv_wr32(mmio, reg, (old & ~mask) | val);
    return old;
}

// ---- Core GPU Functions ----
int  gpu_init(void);                        // Probe PCI, map BARs, identify chip
void gpu_shutdown(void);                    // Disable engines, unmap BARs
gpu_state_t* gpu_get_state(void);           // Get global GPU state

// ---- Identification ----
int  gpu_identify_chip(void);               // Read BOOT_0, determine architecture
const char* gpu_arch_name(uint32_t arch);   // Architecture name string

// ---- MMIO ----
int  gpu_map_mmio(void);                    // Map BAR0 into kernel virtual address space
int  gpu_map_vram(void);                    // Map BAR1 VRAM aperture
int  gpu_map_ramin(void);                   // Map BAR2/3 instance memory (NV50+)

// ---- Engine Control ----
void gpu_engine_reset(void);                // Reset all GPU engines via PMC
void gpu_enable_engines(void);              // Enable PFIFO + PGRAPH
void gpu_disable_interrupts(void);          // Mask all GPU interrupts
void gpu_enable_interrupts(void);           // Unmask GPU interrupts

// ---- Memory Detection ----
uint64_t gpu_detect_vram_size(void);        // Query PFB for VRAM amount
uint8_t  gpu_detect_vram_type(void);        // Query PFB for memory type

// ---- Timer ----
void     gpu_timer_init(void);              // Initialize PTIMER
uint64_t gpu_timer_read(void);              // Read PTIMER nanosecond counter

// ---- VBIOS ----
int  gpu_read_vbios(uint8_t* buf, int max_size);   // Read VBIOS from PROM

// ---- Framebuffer Integration ----
int  gpu_setup_scanout(int width, int height, int bpp);  // Configure scanout from VRAM
void gpu_flip(uint32_t* src, int width, int height);     // Copy backbuf to VRAM

#endif
