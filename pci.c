// pci.c - PCI Bus Enumerator for Alteo OS
// Full bus/device/function enumeration with device tree and BAR mapping
#include "pci.h"

// ---- Port I/O ----
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ---- Device Tree Storage ----
static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int pci_device_count = 0;

// Helper: zero memory
static void pci_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

// ---- PCI Config Space Access ----

// Build a PCI config address
static inline uint32_t pci_addr(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    return (1u << 31) |                         // Enable bit
           ((uint32_t)bus << 16) |
           ((uint32_t)(device & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) |
           ((uint32_t)(offset & 0xFC));          // Align to 32-bit
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, device, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, device, func, offset & ~3);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, device, func, offset & ~3);
    return (uint8_t)((val >> ((offset & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, device, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t tmp = pci_config_read32(bus, device, func, offset & ~3);
    int shift = (offset & 2) * 8;
    tmp &= ~(0xFFFF << shift);
    tmp |= ((uint32_t)val << shift);
    pci_config_write32(bus, device, func, offset & ~3, tmp);
}

void pci_config_write8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t tmp = pci_config_read32(bus, device, func, offset & ~3);
    int shift = (offset & 3) * 8;
    tmp &= ~(0xFF << shift);
    tmp |= ((uint32_t)val << shift);
    pci_config_write32(bus, device, func, offset & ~3, tmp);
}

// ---- BAR Decoding ----

// Probe a BAR to determine base address, size, and type
static void pci_probe_bar(pci_device_t* dev, int bar_idx) {
    if (bar_idx >= PCI_MAX_BARS) return;

    uint8_t bar_offset = PCI_BAR0 + (uint8_t)(bar_idx * 4);
    pci_bar_t* bar = &dev->bars[bar_idx];

    // Read current BAR value
    uint32_t bar_val = pci_config_read32(dev->bus, dev->device, dev->function, bar_offset);

    if (bar_val == 0) {
        bar->present = 0;
        return;
    }

    if (bar_val & PCI_BAR_IO) {
        // I/O space BAR
        bar->type = 1;
        bar->base = bar_val & ~0x3u;
        bar->is_64bit = 0;
        bar->prefetchable = 0;

        // Size: write all 1s, read back, mask, invert, add 1
        pci_config_write32(dev->bus, dev->device, dev->function, bar_offset, 0xFFFFFFFF);
        uint32_t size_val = pci_config_read32(dev->bus, dev->device, dev->function, bar_offset);
        pci_config_write32(dev->bus, dev->device, dev->function, bar_offset, bar_val); // restore

        size_val &= ~0x3u;
        size_val = ~size_val + 1;
        bar->size = size_val & 0xFFFF; // I/O is 16-bit range
        bar->present = (bar->size > 0) ? 1 : 0;
    } else {
        // Memory space BAR
        bar->type = 0;
        bar->prefetchable = (bar_val & PCI_BAR_PREFETCHABLE) ? 1 : 0;
        uint8_t mem_type = (bar_val >> 1) & 0x03;

        if (mem_type == 0x02) {
            // 64-bit BAR: spans this and next BAR slot
            bar->is_64bit = 1;
            uint32_t bar_hi = pci_config_read32(dev->bus, dev->device, dev->function,
                                                 bar_offset + 4);
            bar->base = ((uint64_t)bar_hi << 32) | (bar_val & ~0xFu);

            // Probe size: write all 1s to both halves
            pci_config_write32(dev->bus, dev->device, dev->function, bar_offset, 0xFFFFFFFF);
            pci_config_write32(dev->bus, dev->device, dev->function, bar_offset + 4, 0xFFFFFFFF);
            uint32_t lo = pci_config_read32(dev->bus, dev->device, dev->function, bar_offset);
            uint32_t hi = pci_config_read32(dev->bus, dev->device, dev->function, bar_offset + 4);
            // Restore
            pci_config_write32(dev->bus, dev->device, dev->function, bar_offset, bar_val);
            pci_config_write32(dev->bus, dev->device, dev->function, bar_offset + 4, bar_hi);

            uint64_t size_mask = ((uint64_t)hi << 32) | (lo & ~0xFu);
            bar->size = ~size_mask + 1;
            bar->present = (bar->size > 0) ? 1 : 0;

            // Mark next BAR slot as consumed
            if (bar_idx + 1 < PCI_MAX_BARS) {
                dev->bars[bar_idx + 1].present = 0;
                dev->bars[bar_idx + 1].is_64bit = 0;
            }
        } else {
            // 32-bit BAR
            bar->is_64bit = 0;
            bar->base = bar_val & ~0xFu;

            pci_config_write32(dev->bus, dev->device, dev->function, bar_offset, 0xFFFFFFFF);
            uint32_t size_val = pci_config_read32(dev->bus, dev->device, dev->function, bar_offset);
            pci_config_write32(dev->bus, dev->device, dev->function, bar_offset, bar_val); // restore

            size_val &= ~0xFu;
            bar->size = ~size_val + 1;
            bar->present = (bar->size > 0) ? 1 : 0;
        }
    }
}

// ---- Bus Enumeration ----

// Probe a single function at bus:dev:func
static void pci_probe_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t id = pci_config_read32(bus, dev, func, PCI_VENDOR_ID);
    uint16_t vendor = id & 0xFFFF;

    if (vendor == 0xFFFF || vendor == 0x0000) return;
    if (pci_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &pci_devices[pci_device_count];
    pci_memset(d, 0, sizeof(pci_device_t));

    d->bus = bus;
    d->device = dev;
    d->function = func;
    d->present = 1;

    d->vendor_id = vendor;
    d->device_id = (id >> 16) & 0xFFFF;

    uint32_t class_reg = pci_config_read32(bus, dev, func, PCI_REVISION_ID);
    d->revision    = class_reg & 0xFF;
    d->prog_if     = (class_reg >> 8) & 0xFF;
    d->subclass    = (class_reg >> 16) & 0xFF;
    d->class_code  = (class_reg >> 24) & 0xFF;

    d->header_type = pci_config_read8(bus, dev, func, PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
    d->multifunction = (pci_config_read8(bus, dev, func, PCI_HEADER_TYPE) & PCI_HEADER_MULTIFUNCTION) ? 1 : 0;

    d->irq_line = pci_config_read8(bus, dev, func, PCI_INT_LINE);
    d->irq_pin  = pci_config_read8(bus, dev, func, PCI_INT_PIN);

    // Subsystem IDs (only for header type 0)
    if (d->header_type == 0) {
        d->subsys_vendor_id = pci_config_read16(bus, dev, func, PCI_SUBSYS_VENDOR);
        d->subsys_device_id = pci_config_read16(bus, dev, func, PCI_SUBSYS_ID);

        // Probe all 6 BARs
        for (int i = 0; i < PCI_MAX_BARS; i++) {
            pci_probe_bar(d, i);
            // Skip next BAR if this was a 64-bit BAR
            if (d->bars[i].is_64bit) i++;
        }
    }

    pci_device_count++;

    // If this is a PCI-to-PCI bridge (class 0x06, subclass 0x04), enumerate subordinate bus
    if (d->class_code == PCI_CLASS_BRIDGE && d->subclass == PCI_SUBCLASS_PCI_BRIDGE) {
        // Secondary bus number is at offset 0x19
        uint8_t secondary_bus = pci_config_read8(bus, dev, func, 0x19);
        if (secondary_bus != 0) {
            // Recursively enumerate the secondary bus
            for (int sd = 0; sd < 32; sd++) {
                uint32_t sid = pci_config_read32(secondary_bus, sd, 0, PCI_VENDOR_ID);
                uint16_t svendor = sid & 0xFFFF;
                if (svendor == 0xFFFF || svendor == 0x0000) continue;

                uint8_t sht = pci_config_read8(secondary_bus, sd, 0, PCI_HEADER_TYPE);
                int smf = (sht & PCI_HEADER_MULTIFUNCTION) ? 8 : 1;

                for (int sf = 0; sf < smf; sf++) {
                    pci_probe_function(secondary_bus, sd, sf);
                }
            }
        }
    }
}

void pci_init(void) {
    pci_memset(pci_devices, 0, sizeof(pci_devices));
    pci_device_count = 0;

    // Enumerate all buses, devices, and functions
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            // Check function 0 first
            uint32_t id = pci_config_read32(bus, dev, 0, PCI_VENDOR_ID);
            uint16_t vendor = id & 0xFFFF;
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            // Check if multifunction device
            uint8_t header = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
            int max_func = (header & PCI_HEADER_MULTIFUNCTION) ? 8 : 1;

            for (int func = 0; func < max_func; func++) {
                pci_probe_function(bus, dev, func);
            }
        }
    }
}

// ---- Getters ----

pci_device_t* pci_get_devices(void) {
    return pci_devices;
}

int pci_get_device_count(void) {
    return pci_device_count;
}

// ---- Lookup ----

pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t* prev) {
    int start = 0;
    if (prev) {
        // Find the index of prev, then start from the next one
        for (int i = 0; i < pci_device_count; i++) {
            if (&pci_devices[i] == prev) {
                start = i + 1;
                break;
            }
        }
    }

    for (int i = start; i < pci_device_count; i++) {
        if (pci_devices[i].present &&
            pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }
    return (pci_device_t*)0;
}

pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass, pci_device_t* prev) {
    int start = 0;
    if (prev) {
        for (int i = 0; i < pci_device_count; i++) {
            if (&pci_devices[i] == prev) {
                start = i + 1;
                break;
            }
        }
    }

    for (int i = start; i < pci_device_count; i++) {
        if (pci_devices[i].present &&
            pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return (pci_device_t*)0;
}

pci_device_t* pci_find_class_prog(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                                   pci_device_t* prev) {
    int start = 0;
    if (prev) {
        for (int i = 0; i < pci_device_count; i++) {
            if (&pci_devices[i] == prev) {
                start = i + 1;
                break;
            }
        }
    }

    for (int i = start; i < pci_device_count; i++) {
        if (pci_devices[i].present &&
            pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass &&
            pci_devices[i].prog_if == prog_if) {
            return &pci_devices[i];
        }
    }
    return (pci_device_t*)0;
}

// ---- Device Control ----

void pci_enable_bus_master(pci_device_t* dev) {
    if (!dev) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

void pci_enable_mem_space(pci_device_t* dev) {
    if (!dev) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

void pci_enable_io_space(pci_device_t* dev) {
    if (!dev) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

void pci_enable_interrupts(pci_device_t* dev) {
    if (!dev) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd &= ~PCI_CMD_INT_DISABLE;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

uint64_t pci_get_bar_base(pci_device_t* dev, int bar_index) {
    if (!dev || bar_index < 0 || bar_index >= PCI_MAX_BARS) return 0;
    if (!dev->bars[bar_index].present) return 0;
    return dev->bars[bar_index].base;
}

uint64_t pci_get_bar_size(pci_device_t* dev, int bar_index) {
    if (!dev || bar_index < 0 || bar_index >= PCI_MAX_BARS) return 0;
    if (!dev->bars[bar_index].present) return 0;
    return dev->bars[bar_index].size;
}

int pci_bar_is_io(pci_device_t* dev, int bar_index) {
    if (!dev || bar_index < 0 || bar_index >= PCI_MAX_BARS) return 0;
    if (!dev->bars[bar_index].present) return 0;
    return dev->bars[bar_index].type == 1;
}
