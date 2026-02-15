// gpu.c - NVIDIA GPU Driver for Alteo OS
// Stage 1: PCI Discovery, BAR Mapping, Chip Identification, Memory Detection
// Primary target: NV50 Tesla architecture (envytools documented)
// Falls back gracefully if no NVIDIA GPU or unsupported generation

#include "gpu.h"
#include "pci.h"
#include "vmm.h"
#include "heap.h"
#include "graphics.h"

// ---- String helpers (freestanding) ----
static void gpu_strcpy(char* d, const char* s) {
    while ((*d++ = *s++));
}

static void gpu_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void gpu_memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

// ---- Global GPU state ----
gpu_state_t gpu_state;

// ---- MMIO virtual address allocation region ----
// We map GPU BARs starting at this kernel virtual address
#define GPU_MMIO_VBASE      0xFFFF800080000000ULL   // 16 MB region for BAR0
#define GPU_VRAM_VBASE      0xFFFF800090000000ULL   // Up to 256 MB for BAR1
#define GPU_RAMIN_VBASE     0xFFFF8000A0000000ULL   // 16 MB for BAR2/3

gpu_state_t* gpu_get_state(void) {
    return &gpu_state;
}

// ---- Architecture name lookup ----
const char* gpu_arch_name(uint32_t arch) {
    switch (arch) {
        case NV_ARCH_NV04:  return "NV04 (RIVA TNT)";
        case NV_ARCH_NV10:  return "NV10 (GeForce 256)";
        case NV_ARCH_NV20:  return "NV20 (GeForce3)";
        case NV_ARCH_NV30:  return "NV30 (GeForce FX)";
        case NV_ARCH_NV40:  return "NV40 (GeForce 6/7)";
        case NV_ARCH_NV50:  return "NV50 (Tesla)";
        case NV_ARCH_NVC0:  return "NVC0 (Fermi)";
        case NV_ARCH_NVE0:  return "NVE0 (Kepler)";
        case NV_ARCH_GM100: return "GM100 (Maxwell)";
        case NV_ARCH_GP100: return "GP100 (Pascal)";
        case NV_ARCH_GV100: return "GV100 (Volta)";
        case NV_ARCH_TU100: return "TU100 (Turing)";
        case NV_ARCH_GA100: return "GA100 (Ampere)";
        default:            return "Unknown";
    }
}

// ============================================================
// PCI Discovery
// ============================================================

// Find NVIDIA GPU on PCI bus
// Returns pointer to PCI device or NULL
static pci_device_t* gpu_find_nvidia(void) {
    // First try to find any NVIDIA VGA-class device
    pci_device_t* dev = pci_find_class(PCI_CLASS_DISPLAY, PCI_SUBCLASS_VGA, 0);
    while (dev) {
        if (dev->vendor_id == NV_VENDOR_ID) {
            return dev;
        }
        dev = pci_find_class(PCI_CLASS_DISPLAY, PCI_SUBCLASS_VGA, dev);
    }

    // Also check display class 0x03 subclass 0x80 (other display controller)
    dev = pci_find_device(NV_VENDOR_ID, 0, 0);
    while (dev) {
        if (dev->class_code == PCI_CLASS_DISPLAY) {
            return dev;
        }
        dev = pci_find_device(NV_VENDOR_ID, 0, dev);
    }

    return 0;
}

// ============================================================
// BAR Mapping
// ============================================================

int gpu_map_mmio(void) {
    if (gpu_state.mmio_phys == 0 || gpu_state.mmio_size == 0) return -1;

    // Map BAR0 (MMIO registers) into kernel virtual address space
    // MMIO must be uncacheable (write-through or uncacheable)
    uint64_t vbase = GPU_MMIO_VBASE;
    uint64_t pages = (gpu_state.mmio_size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    pte_t* pml4 = vmm_get_kernel_pml4();

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = gpu_state.mmio_phys + (i * VMM_PAGE_SIZE);
        uint64_t virt = vbase + (i * VMM_PAGE_SIZE);
        vmm_map_page(pml4, virt, phys,
                      VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE);
    }

    gpu_state.mmio = (volatile uint32_t*)vbase;
    gpu_state.mmio_mapped = 1;
    return 0;
}

int gpu_map_vram(void) {
    if (gpu_state.vram_phys == 0 || gpu_state.vram_size == 0) return -1;

    // Map BAR1 (VRAM aperture) into kernel virtual address space
    // VRAM can be write-combining for better performance, but we use uncacheable for safety
    uint64_t vbase = GPU_VRAM_VBASE;
    uint64_t size = gpu_state.vram_size;

    // Limit mapping to 256 MB to avoid excessive page table entries
    if (size > 256 * 1024 * 1024) {
        size = 256 * 1024 * 1024;
    }

    uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    pte_t* pml4 = vmm_get_kernel_pml4();

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = gpu_state.vram_phys + (i * VMM_PAGE_SIZE);
        uint64_t virt = vbase + (i * VMM_PAGE_SIZE);
        vmm_map_page(pml4, virt, phys,
                      VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE);
    }

    gpu_state.vram = (volatile uint32_t*)vbase;
    gpu_state.vram_mapped = 1;
    return 0;
}

int gpu_map_ramin(void) {
    if (gpu_state.ramin_phys == 0 || gpu_state.ramin_size == 0) return -1;

    uint64_t vbase = GPU_RAMIN_VBASE;
    uint64_t pages = (gpu_state.ramin_size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    pte_t* pml4 = vmm_get_kernel_pml4();

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = gpu_state.ramin_phys + (i * VMM_PAGE_SIZE);
        uint64_t virt = vbase + (i * VMM_PAGE_SIZE);
        vmm_map_page(pml4, virt, phys,
                      VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE);
    }

    gpu_state.ramin = (volatile uint32_t*)vbase;
    return 0;
}

// ============================================================
// Chip Identification
// ============================================================

// Parse BOOT_0 register to determine GPU architecture
// BOOT_0 format (varies by generation):
//   NV04-NV40: bits[7:4] = stepping, bits[23:20] = chipset family
//   NV50+: bits[23:20] = chipset family, bits[27:24] = sub-family
int gpu_identify_chip(void) {
    if (!gpu_state.mmio_mapped || !gpu_state.mmio) return -1;

    uint32_t boot0 = nv_rd32(gpu_state.mmio, NV_PMC_BOOT_0);
    gpu_state.chipset = boot0;

    // Determine architecture from BOOT_0
    // Reference: envytools PMC documentation
    uint32_t chipset_id = 0;

    if ((boot0 & 0x0F000000) != 0) {
        // NV10+ encoding: chipset in bits [27:20]
        chipset_id = (boot0 >> 20) & 0x1FF;
    } else {
        // Pre-NV10 or NV04
        chipset_id = NV_ARCH_NV04;
    }

    // Map chipset ID ranges to architecture families
    if (chipset_id >= 0x170) {
        gpu_state.arch = NV_ARCH_GA100;
        gpu_strcpy(gpu_state.chip_name, "Ampere");
    } else if (chipset_id >= 0x160) {
        gpu_state.arch = NV_ARCH_TU100;
        gpu_strcpy(gpu_state.chip_name, "Turing");
    } else if (chipset_id >= 0x140) {
        gpu_state.arch = NV_ARCH_GV100;
        gpu_strcpy(gpu_state.chip_name, "Volta");
    } else if (chipset_id >= 0x130) {
        gpu_state.arch = NV_ARCH_GP100;
        gpu_strcpy(gpu_state.chip_name, "Pascal");
    } else if (chipset_id >= 0x110) {
        gpu_state.arch = NV_ARCH_GM100;
        gpu_strcpy(gpu_state.chip_name, "Maxwell");
    } else if (chipset_id >= 0xE0) {
        gpu_state.arch = NV_ARCH_NVE0;
        gpu_strcpy(gpu_state.chip_name, "Kepler");
    } else if (chipset_id >= 0xC0) {
        gpu_state.arch = NV_ARCH_NVC0;
        gpu_strcpy(gpu_state.chip_name, "Fermi");
    } else if (chipset_id >= 0x50) {
        gpu_state.arch = NV_ARCH_NV50;
        gpu_strcpy(gpu_state.chip_name, "Tesla");
    } else if (chipset_id >= 0x40) {
        gpu_state.arch = NV_ARCH_NV40;
        gpu_strcpy(gpu_state.chip_name, "Curie");
    } else if (chipset_id >= 0x30) {
        gpu_state.arch = NV_ARCH_NV30;
        gpu_strcpy(gpu_state.chip_name, "Rankine");
    } else if (chipset_id >= 0x20) {
        gpu_state.arch = NV_ARCH_NV20;
        gpu_strcpy(gpu_state.chip_name, "Kelvin");
    } else if (chipset_id >= 0x10) {
        gpu_state.arch = NV_ARCH_NV10;
        gpu_strcpy(gpu_state.chip_name, "Celsius");
    } else {
        gpu_state.arch = NV_ARCH_NV04;
        gpu_strcpy(gpu_state.chip_name, "Fahrenheit");
    }

    gpu_state.chiprev = boot0 & 0xFF;
    return 0;
}

// ============================================================
// Memory Detection
// ============================================================

uint64_t gpu_detect_vram_size(void) {
    if (!gpu_state.mmio_mapped || !gpu_state.mmio) return 0;

    uint64_t vram_bytes = 0;

    if (gpu_state.arch >= NV_ARCH_NVC0) {
        // Fermi+: VRAM size in PFB.CFG0 or readable from GPC register
        // Read from BAR0 offset 0x10020C (CSTATUS contains memory size info)
        uint32_t cfg = nv_rd32(gpu_state.mmio, NV_PFB_CSTATUS);
        vram_bytes = (uint64_t)cfg;  // In bytes on Fermi+
        if (vram_bytes == 0) {
            // Fallback: use BAR1 size as estimate
            vram_bytes = gpu_state.vram_size;
        }
    } else if (gpu_state.arch >= NV_ARCH_NV50) {
        // NV50 (Tesla): PFB_CSTATUS contains VRAM size in bytes
        uint32_t cfg = nv_rd32(gpu_state.mmio, NV_PFB_CSTATUS);
        vram_bytes = (uint64_t)cfg;
        if (vram_bytes == 0) {
            vram_bytes = gpu_state.vram_size;
        }
    } else if (gpu_state.arch >= NV_ARCH_NV40) {
        // NV40 (Curie): PFB.CFG0 bits encode memory size
        uint32_t cfg0 = nv_rd32(gpu_state.mmio, NV_PFB_CFG0);
        uint32_t mem_amount = (cfg0 >> 12) & 0xF;
        // Encoding: 0=32MB, 1=64MB, 2=128MB, 3=256MB, etc.
        vram_bytes = (uint64_t)(32 * 1024 * 1024) << mem_amount;
        // Sanity clamp
        if (vram_bytes > 2ULL * 1024 * 1024 * 1024) vram_bytes = gpu_state.vram_size;
    } else {
        // Pre-NV40: use BAR1 size as rough estimate
        vram_bytes = gpu_state.vram_size;
    }

    gpu_state.vram_total = vram_bytes;
    return vram_bytes;
}

uint8_t gpu_detect_vram_type(void) {
    if (!gpu_state.mmio_mapped || !gpu_state.mmio) return NV_VRAM_TYPE_UNKNOWN;

    // Memory type detection varies by architecture
    // For NV50+, PFB registers describe memory technology
    if (gpu_state.arch >= NV_ARCH_NV50) {
        uint32_t pfb0 = nv_rd32(gpu_state.mmio, NV_PFB_CFG0);
        uint32_t mem_tech = (pfb0 >> 0) & 0xF;

        switch (mem_tech) {
            case 1:  gpu_state.vram_type = NV_VRAM_TYPE_DDR2; break;
            case 2:  gpu_state.vram_type = NV_VRAM_TYPE_DDR3; break;
            case 3:  gpu_state.vram_type = NV_VRAM_TYPE_GDDR3; break;
            case 5:  gpu_state.vram_type = NV_VRAM_TYPE_GDDR5; break;
            default: gpu_state.vram_type = NV_VRAM_TYPE_UNKNOWN; break;
        }
    } else {
        gpu_state.vram_type = NV_VRAM_TYPE_UNKNOWN;
    }

    return gpu_state.vram_type;
}

// ============================================================
// Engine Control
// ============================================================

void gpu_engine_reset(void) {
    if (!gpu_state.mmio_mapped) return;

    // Reset PFIFO and PGRAPH by toggling their enable bits
    uint32_t pmc_enable = nv_rd32(gpu_state.mmio, NV_PMC_ENABLE);

    // Disable engines
    nv_wr32(gpu_state.mmio, NV_PMC_ENABLE,
            pmc_enable & ~(NV_PMC_ENABLE_PFIFO | NV_PMC_ENABLE_PGRAPH));

    // Small delay (busy wait ~1ms at ~1GHz)
    for (volatile int i = 0; i < 1000000; i++);

    // Re-enable engines
    nv_wr32(gpu_state.mmio, NV_PMC_ENABLE,
            pmc_enable | NV_PMC_ENABLE_PFIFO | NV_PMC_ENABLE_PGRAPH);

    // Another small delay for engine init
    for (volatile int i = 0; i < 1000000; i++);
}

void gpu_enable_engines(void) {
    if (!gpu_state.mmio_mapped) return;

    uint32_t enable = nv_rd32(gpu_state.mmio, NV_PMC_ENABLE);
    enable |= NV_PMC_ENABLE_PFIFO | NV_PMC_ENABLE_PGRAPH;
    nv_wr32(gpu_state.mmio, NV_PMC_ENABLE, enable);

    gpu_state.pfifo_enabled = 1;
    gpu_state.pgraph_enabled = 1;
}

void gpu_disable_interrupts(void) {
    if (!gpu_state.mmio_mapped) return;

    // Disable all interrupts
    nv_wr32(gpu_state.mmio, NV_PMC_INTR_EN_0, 0x00000000);

    // Clear any pending interrupts
    nv_wr32(gpu_state.mmio, NV_PMC_INTR_0, 0xFFFFFFFF);
}

void gpu_enable_interrupts(void) {
    if (!gpu_state.mmio_mapped) return;

    // Enable relevant interrupts
    uint32_t mask = NV_PMC_INTR_PFIFO | NV_PMC_INTR_PGRAPH | NV_PMC_INTR_PTIMER;

    if (gpu_state.arch >= NV_ARCH_NV50) {
        mask |= NV_PMC_INTR_PDISPLAY;  // NV50+ uses unified display engine
    } else {
        mask |= NV_PMC_INTR_PCRTC;     // Pre-NV50 uses PCRTC
    }

    nv_wr32(gpu_state.mmio, NV_PMC_INTR_EN_0, mask);
}

// ============================================================
// Timer
// ============================================================

void gpu_timer_init(void) {
    if (!gpu_state.mmio_mapped) return;

    // Set up PTIMER to count in nanoseconds
    // PTIMER uses a fractional divider: time = ticks * (NUMERATOR / DENOMINATOR)
    // For 1ns resolution with a ~27MHz base clock:
    // numerator = 8, denominator = 216 (27MHz crystal / 1GHz ns)
    // Actual values depend on crystal frequency; these are common defaults

    nv_wr32(gpu_state.mmio, NV_PTIMER_NUMERATOR, 0x00000008);
    nv_wr32(gpu_state.mmio, NV_PTIMER_DENOMINATOR, 0x000000D8);

    // Clear timer interrupts
    nv_wr32(gpu_state.mmio, NV_PTIMER_INTR_0, 0xFFFFFFFF);
    nv_wr32(gpu_state.mmio, NV_PTIMER_INTR_EN_0, 0x00000000);
}

uint64_t gpu_timer_read(void) {
    if (!gpu_state.mmio_mapped) return 0;

    // Read 64-bit nanosecond counter (TIME_1:TIME_0)
    // Must read TIME_1 first, then TIME_0 for atomicity on NV50+
    uint32_t hi, lo, hi2;
    do {
        hi = nv_rd32(gpu_state.mmio, NV_PTIMER_TIME_1);
        lo = nv_rd32(gpu_state.mmio, NV_PTIMER_TIME_0);
        hi2 = nv_rd32(gpu_state.mmio, NV_PTIMER_TIME_1);
    } while (hi != hi2);  // Retry if TIME_1 rolled over

    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// ============================================================
// VBIOS
// ============================================================

int gpu_read_vbios(uint8_t* buf, int max_size) {
    if (!gpu_state.mmio_mapped || !buf || max_size <= 0) return -1;

    // NVIDIA VBIOS can be read from:
    // 1. PROM (BAR0 + 0x300000) - ROM shadow in MMIO space
    // 2. PCI Expansion ROM BAR

    // Try PROM first
    // Enable ROM access by setting PBUS register
    uint32_t old_pbus = nv_rd32(gpu_state.mmio, NV_PBUS);
    nv_wr32(gpu_state.mmio, NV_PBUS, old_pbus | 0x00000001);

    // Check for BIOS signature (0x55AA)
    uint8_t sig0 = (uint8_t)nv_rd32(gpu_state.mmio, NV_PROM_DATA);
    uint8_t sig1 = (uint8_t)nv_rd32(gpu_state.mmio, NV_PROM_DATA + 4);

    if (sig0 != 0x55 || sig1 != 0xAA) {
        // No valid BIOS found in PROM
        nv_wr32(gpu_state.mmio, NV_PBUS, old_pbus);
        gpu_state.vbios_size = 0;
        return -1;
    }

    // BIOS size in 512-byte blocks at offset 2
    uint8_t size_blocks = (uint8_t)nv_rd32(gpu_state.mmio, NV_PROM_DATA + 8);
    int bios_size = (int)size_blocks * 512;
    if (bios_size > max_size) bios_size = max_size;
    if (bios_size > 256 * 1024) bios_size = 256 * 1024;  // Sanity limit

    // Read BIOS byte by byte from PROM
    for (int i = 0; i < bios_size; i++) {
        buf[i] = (uint8_t)nv_rd32(gpu_state.mmio, NV_PROM_DATA + (i * 4));
    }

    // Restore PBUS
    nv_wr32(gpu_state.mmio, NV_PBUS, old_pbus);

    gpu_state.vbios_size = (uint32_t)bios_size;
    return bios_size;
}

// ============================================================
// Framebuffer / Scanout Integration
// ============================================================

int gpu_setup_scanout(int width, int height, int bpp) {
    if (!gpu_state.vram_mapped || !gpu_state.vram) return -1;

    // For Stage 1, we simply use the VRAM aperture as a linear framebuffer
    // The actual display engine (CRTC/mode setting) is configured in nv_display.c
    // Here we just set up the software state

    gpu_state.display_width = width;
    gpu_state.display_height = height;
    gpu_state.display_bpp = bpp;
    gpu_state.display_pitch = width * (bpp / 8);
    gpu_state.fb_offset = 0;  // Framebuffer starts at VRAM offset 0

    gpu_state.display_active = 1;
    return 0;
}

void gpu_flip(uint32_t* src, int width, int height) {
    if (!gpu_state.vram_mapped || !gpu_state.vram || !gpu_state.display_active) return;

    // Copy software backbuffer → GPU VRAM via BAR1 aperture
    // This is still a CPU copy, but writes go to GPU VRAM instead of VBE framebuffer
    // 2D acceleration (nv_2d.c) will replace this with DMA blits
    int stride = gpu_state.display_pitch / 4;
    volatile uint32_t* dst = gpu_state.vram + (gpu_state.fb_offset / 4);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            dst[y * stride + x] = src[y * width + x];
        }
    }
}

// ============================================================
// Main Initialization
// ============================================================

int gpu_init(void) {
    // Clear state
    gpu_memset(&gpu_state, 0, sizeof(gpu_state_t));

    // --- Step 1: Find NVIDIA GPU on PCI bus ---
    pci_device_t* dev = gpu_find_nvidia();
    if (!dev) {
        // No NVIDIA GPU found — this is expected on QEMU without -device
        return -1;
    }

    // Store PCI info
    gpu_state.pci_device_id = dev->device_id;
    gpu_state.pci_bus = dev->bus;
    gpu_state.pci_dev = dev->device;
    gpu_state.pci_func = dev->function;
    gpu_state.pci_irq = dev->irq_line;

    // --- Step 2: Enable PCI device ---
    pci_enable_bus_master(dev);
    pci_enable_mem_space(dev);

    // --- Step 3: Read BAR addresses ---

    // BAR0 = MMIO registers (typically 16-32 MB, memory-mapped)
    if (dev->bars[NV_BAR0_INDEX].present && !dev->bars[NV_BAR0_INDEX].type) {
        gpu_state.mmio_phys = dev->bars[NV_BAR0_INDEX].base;
        gpu_state.mmio_size = dev->bars[NV_BAR0_INDEX].size;
        if (gpu_state.mmio_size == 0) gpu_state.mmio_size = 16 * 1024 * 1024;  // Default 16 MB
    } else {
        return -2;  // No MMIO BAR — can't control GPU
    }

    // BAR1 = VRAM aperture (varies: 64MB-256MB+)
    // May be 64-bit BAR consuming indices 1+2
    if (dev->bars[NV_BAR1_INDEX].present && !dev->bars[NV_BAR1_INDEX].type) {
        gpu_state.vram_phys = dev->bars[NV_BAR1_INDEX].base;
        gpu_state.vram_size = dev->bars[NV_BAR1_INDEX].size;
        if (gpu_state.vram_size == 0) gpu_state.vram_size = 64 * 1024 * 1024;  // Default 64 MB
    }

    // BAR2/3 = Instance memory / RAMIN (NV50+, index 3 if BAR1 is 64-bit)
    int ramin_idx = NV_BAR2_INDEX;
    if (dev->bars[ramin_idx].present && !dev->bars[ramin_idx].type) {
        gpu_state.ramin_phys = dev->bars[ramin_idx].base;
        gpu_state.ramin_size = dev->bars[ramin_idx].size;
        if (gpu_state.ramin_size == 0) gpu_state.ramin_size = 16 * 1024 * 1024;
    }

    // --- Step 4: Map BAR0 (MMIO) into virtual address space ---
    if (gpu_map_mmio() != 0) {
        return -3;  // Failed to map MMIO
    }

    // --- Step 5: Identify GPU chip ---
    gpu_identify_chip();

    // --- Step 6: Disable interrupts during init ---
    gpu_disable_interrupts();

    // --- Step 7: Reset GPU engines ---
    gpu_engine_reset();

    // --- Step 8: Initialize PTIMER ---
    gpu_timer_init();

    // --- Step 9: Detect VRAM ---
    gpu_detect_vram_size();
    gpu_detect_vram_type();

    // --- Step 10: Map VRAM aperture ---
    if (gpu_state.vram_phys) {
        gpu_map_vram();
    }

    // --- Step 11: Map RAMIN (NV50+) ---
    if (gpu_state.ramin_phys && gpu_state.arch >= NV_ARCH_NV50) {
        gpu_map_ramin();
    }

    // --- Step 12: Enable engines ---
    gpu_enable_engines();

    gpu_state.initialized = 1;
    return 0;
}

void gpu_shutdown(void) {
    if (!gpu_state.initialized) return;

    gpu_disable_interrupts();

    // Disable engines
    if (gpu_state.mmio_mapped) {
        uint32_t enable = nv_rd32(gpu_state.mmio, NV_PMC_ENABLE);
        enable &= ~(NV_PMC_ENABLE_PFIFO | NV_PMC_ENABLE_PGRAPH);
        nv_wr32(gpu_state.mmio, NV_PMC_ENABLE, enable);
    }

    gpu_state.pfifo_enabled = 0;
    gpu_state.pgraph_enabled = 0;
    gpu_state.display_active = 0;
    gpu_state.initialized = 0;
}
