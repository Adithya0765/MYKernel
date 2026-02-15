// pci.h - PCI Bus Enumerator for Alteo OS
// Full bus/device/function enumeration with device tree and BAR mapping
#ifndef PCI_H
#define PCI_H

#include "stdint.h"

// PCI Configuration Space ports
#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

// PCI Configuration Space offsets
#define PCI_VENDOR_ID       0x00    // 16-bit
#define PCI_DEVICE_ID       0x02    // 16-bit
#define PCI_COMMAND         0x04    // 16-bit
#define PCI_STATUS          0x06    // 16-bit
#define PCI_REVISION_ID     0x08    // 8-bit
#define PCI_PROG_IF         0x09    // 8-bit
#define PCI_SUBCLASS        0x0A    // 8-bit
#define PCI_CLASS           0x0B    // 8-bit
#define PCI_CACHE_LINE      0x0C    // 8-bit
#define PCI_LATENCY_TIMER   0x0D    // 8-bit
#define PCI_HEADER_TYPE     0x0E    // 8-bit
#define PCI_BIST            0x0F    // 8-bit
#define PCI_BAR0            0x10    // 32-bit
#define PCI_BAR1            0x14    // 32-bit
#define PCI_BAR2            0x18    // 32-bit
#define PCI_BAR3            0x1C    // 32-bit
#define PCI_BAR4            0x20    // 32-bit
#define PCI_BAR5            0x24    // 32-bit
#define PCI_CARDBUS_CIS     0x28    // 32-bit
#define PCI_SUBSYS_VENDOR   0x2C    // 16-bit
#define PCI_SUBSYS_ID       0x2E    // 16-bit
#define PCI_EXP_ROM_BASE    0x30    // 32-bit
#define PCI_CAP_PTR         0x34    // 8-bit
#define PCI_INT_LINE        0x3C    // 8-bit
#define PCI_INT_PIN         0x3D    // 8-bit
#define PCI_MIN_GNT         0x3E    // 8-bit
#define PCI_MAX_LAT         0x3F    // 8-bit

// PCI Command Register bits
#define PCI_CMD_IO_SPACE        (1 << 0)    // I/O Space enable
#define PCI_CMD_MEM_SPACE       (1 << 1)    // Memory Space enable
#define PCI_CMD_BUS_MASTER      (1 << 2)    // Bus Master enable
#define PCI_CMD_SPECIAL_CYCLES  (1 << 3)
#define PCI_CMD_MWI             (1 << 4)    // Memory Write & Invalidate
#define PCI_CMD_VGA_PALETTE     (1 << 5)
#define PCI_CMD_PARITY_ERR      (1 << 6)
#define PCI_CMD_SERR            (1 << 8)
#define PCI_CMD_FAST_B2B        (1 << 9)
#define PCI_CMD_INT_DISABLE     (1 << 10)

// PCI Header Type bits
#define PCI_HEADER_TYPE_MASK    0x7F
#define PCI_HEADER_MULTIFUNCTION 0x80

// PCI Class Codes
#define PCI_CLASS_UNCLASSIFIED  0x00
#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_NETWORK       0x02
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_CLASS_MEMORY        0x05
#define PCI_CLASS_BRIDGE        0x06
#define PCI_CLASS_COMMUNICATION 0x07
#define PCI_CLASS_SYSTEM        0x08
#define PCI_CLASS_INPUT         0x09
#define PCI_CLASS_DOCKING       0x0A
#define PCI_CLASS_PROCESSOR     0x0B
#define PCI_CLASS_SERIAL_BUS    0x0C

// Common subclasses
#define PCI_SUBCLASS_IDE        0x01    // Storage: IDE
#define PCI_SUBCLASS_SATA       0x06    // Storage: SATA
#define PCI_SUBCLASS_NVMe       0x08    // Storage: NVMe
#define PCI_SUBCLASS_ETHERNET   0x00    // Network: Ethernet
#define PCI_SUBCLASS_VGA        0x00    // Display: VGA
#define PCI_SUBCLASS_AUDIO      0x01    // Multimedia: Audio
#define PCI_SUBCLASS_USB        0x03    // Serial Bus: USB
#define PCI_SUBCLASS_PCI_BRIDGE 0x04    // Bridge: PCI-to-PCI

// USB Program Interface values
#define PCI_PROG_IF_UHCI        0x00
#define PCI_PROG_IF_OHCI        0x10
#define PCI_PROG_IF_EHCI        0x20
#define PCI_PROG_IF_XHCI        0x30

// BAR types
#define PCI_BAR_IO              0x01    // I/O space BAR
#define PCI_BAR_MEM32           0x00    // 32-bit memory BAR
#define PCI_BAR_MEM64           0x04    // 64-bit memory BAR
#define PCI_BAR_PREFETCHABLE    0x08    // Prefetchable memory

// Maximum devices we track
#define PCI_MAX_DEVICES         64
#define PCI_MAX_BARS            6

// BAR info
typedef struct {
    uint64_t base;          // Base address (physical)
    uint64_t size;          // Size in bytes
    uint8_t  type;          // 0 = memory, 1 = I/O
    uint8_t  is_64bit;      // This is the lower half of a 64-bit BAR
    uint8_t  prefetchable;  // Memory is prefetchable
    uint8_t  present;       // BAR is valid
} pci_bar_t;

// PCI device descriptor
typedef struct {
    // Location
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint8_t  present;

    // Identification
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor_id;
    uint16_t subsys_device_id;

    // Classification
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;

    // Header type
    uint8_t  header_type;
    uint8_t  multifunction;

    // Interrupt info
    uint8_t  irq_line;
    uint8_t  irq_pin;

    // BARs
    pci_bar_t bars[PCI_MAX_BARS];
} pci_device_t;

// ---- Core PCI Config Space Access ----

// Read 32-bit value from PCI config space
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);

// Read 16-bit value from PCI config space
uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);

// Read 8-bit value from PCI config space
uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);

// Write 32-bit value to PCI config space
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t val);

// Write 16-bit value to PCI config space
void pci_config_write16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint16_t val);

// Write 8-bit value to PCI config space
void pci_config_write8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint8_t val);

// ---- Bus Enumeration ----

// Initialize PCI subsystem: enumerate all devices
void pci_init(void);

// Get the device tree (array of discovered devices)
pci_device_t* pci_get_devices(void);

// Get number of discovered devices
int pci_get_device_count(void);

// ---- Device Lookup ----

// Find device by vendor + device ID. Returns NULL if not found.
// If `prev` is not NULL, starts searching after that device.
pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t* prev);

// Find device by class + subclass. Returns NULL if not found.
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass, pci_device_t* prev);

// Find device by class + subclass + prog_if. Returns NULL if not found.
pci_device_t* pci_find_class_prog(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                                   pci_device_t* prev);

// ---- Device Control ----

// Enable bus mastering for a device
void pci_enable_bus_master(pci_device_t* dev);

// Enable memory space access
void pci_enable_mem_space(pci_device_t* dev);

// Enable I/O space access
void pci_enable_io_space(pci_device_t* dev);

// Enable interrupts (clear interrupt disable bit)
void pci_enable_interrupts(pci_device_t* dev);

// Get a BAR's mapped address (returns physical base)
uint64_t pci_get_bar_base(pci_device_t* dev, int bar_index);

// Get a BAR's size
uint64_t pci_get_bar_size(pci_device_t* dev, int bar_index);

// Check if a BAR is I/O or memory
int pci_bar_is_io(pci_device_t* dev, int bar_index);

#endif
