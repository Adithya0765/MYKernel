// e1000.c - Intel E1000 NIC Driver for Alteo OS
#include "e1000.h"
#include "heap.h"
#include "pci.h"

// Port I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

// Global device state
static e1000_device_t e1000_dev;
static int e1000_initialized = 0;

// MMIO read/write
static void e1000_write_reg(uint32_t reg, uint32_t val) {
    if (e1000_dev.use_mmio) {
        *(volatile uint32_t*)(uintptr_t)(e1000_dev.mmio_base + reg) = val;
    } else {
        outl(e1000_dev.io_base, reg);
        outl(e1000_dev.io_base + 4, val);
    }
}

static uint32_t e1000_read_reg(uint32_t reg) {
    if (e1000_dev.use_mmio) {
        return *(volatile uint32_t*)(uintptr_t)(e1000_dev.mmio_base + reg);
    } else {
        outl(e1000_dev.io_base, reg);
        return inl(e1000_dev.io_base + 4);
    }
}

// Detect EEPROM
static int e1000_detect_eeprom(void) {
    e1000_write_reg(E1000_EERD, 0x1);
    for (int i = 0; i < 1000; i++) {
        uint32_t val = e1000_read_reg(E1000_EERD);
        if (val & 0x10) return 1;
    }
    return 0;
}

// Read from EEPROM
static uint16_t e1000_read_eeprom(uint8_t addr) {
    uint32_t tmp;
    if (e1000_dev.has_eeprom) {
        e1000_write_reg(E1000_EERD, ((uint32_t)addr << 8) | 1);
        while (!((tmp = e1000_read_reg(E1000_EERD)) & (1 << 4)));
    } else {
        e1000_write_reg(E1000_EERD, ((uint32_t)addr << 2) | 1);
        while (!((tmp = e1000_read_reg(E1000_EERD)) & (1 << 1)));
    }
    return (uint16_t)((tmp >> 16) & 0xFFFF);
}

// Read MAC address
static void e1000_read_mac(void) {
    if (e1000_dev.has_eeprom) {
        uint16_t t;
        t = e1000_read_eeprom(0);
        e1000_dev.mac[0] = t & 0xFF;
        e1000_dev.mac[1] = t >> 8;
        t = e1000_read_eeprom(1);
        e1000_dev.mac[2] = t & 0xFF;
        e1000_dev.mac[3] = t >> 8;
        t = e1000_read_eeprom(2);
        e1000_dev.mac[4] = t & 0xFF;
        e1000_dev.mac[5] = t >> 8;
    } else {
        uint32_t mac_lo = e1000_read_reg(E1000_RAL);
        uint32_t mac_hi = e1000_read_reg(E1000_RAH);
        e1000_dev.mac[0] = mac_lo & 0xFF;
        e1000_dev.mac[1] = (mac_lo >> 8) & 0xFF;
        e1000_dev.mac[2] = (mac_lo >> 16) & 0xFF;
        e1000_dev.mac[3] = (mac_lo >> 24) & 0xFF;
        e1000_dev.mac[4] = mac_hi & 0xFF;
        e1000_dev.mac[5] = (mac_hi >> 8) & 0xFF;
    }
}

// Initialize RX ring
static void e1000_init_rx(void) {
    e1000_dev.rx_descs = (e1000_rx_desc_t*)kmalloc(sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC + 16);

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e1000_dev.rx_bufs[i] = (uint8_t*)kmalloc(E1000_RX_BUF_SIZE + 16);
        e1000_dev.rx_descs[i].addr = (uint64_t)(uintptr_t)e1000_dev.rx_bufs[i];
        e1000_dev.rx_descs[i].status = 0;
    }

    e1000_write_reg(E1000_RDBAL, (uint32_t)(uintptr_t)e1000_dev.rx_descs);
    e1000_write_reg(E1000_RDBAH, 0);
    e1000_write_reg(E1000_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write_reg(E1000_RDH, 0);
    e1000_write_reg(E1000_RDT, E1000_NUM_RX_DESC - 1);

    e1000_dev.rx_cur = 0;

    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;
    e1000_write_reg(E1000_RCTL, rctl);
}

// Initialize TX ring
static void e1000_init_tx(void) {
    e1000_dev.tx_descs = (e1000_tx_desc_t*)kmalloc(sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC + 16);

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        e1000_dev.tx_bufs[i] = (uint8_t*)kmalloc(E1000_TX_BUF_SIZE + 16);
        e1000_dev.tx_descs[i].addr = (uint64_t)(uintptr_t)e1000_dev.tx_bufs[i];
        e1000_dev.tx_descs[i].status = E1000_TXD_STAT_DD; // Mark as done
        e1000_dev.tx_descs[i].cmd = 0;
    }

    e1000_write_reg(E1000_TDBAL, (uint32_t)(uintptr_t)e1000_dev.tx_descs);
    e1000_write_reg(E1000_TDBAH, 0);
    e1000_write_reg(E1000_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    e1000_write_reg(E1000_TDH, 0);
    e1000_write_reg(E1000_TDT, 0);

    e1000_dev.tx_cur = 0;

    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (15 << E1000_TCTL_CT_SHIFT) |
                    (64 << E1000_TCTL_COLD_SHIFT);
    e1000_write_reg(E1000_TCTL, tctl);
}

// Scan PCI bus for E1000 (uses central PCI enumerator)
static int e1000_pci_scan(void) {
    // Try all known E1000 device IDs
    uint16_t device_ids[] = { E1000_DEVICE_ID, E1000_DEVICE_ID2, E1000_DEVICE_ID3 };
    pci_device_t* dev = (pci_device_t*)0;

    for (int i = 0; i < 3; i++) {
        dev = pci_find_device(E1000_VENDOR_ID, device_ids[i], (pci_device_t*)0);
        if (dev) break;
    }

    if (!dev) return 0;

    // Get BAR0
    if (pci_bar_is_io(dev, 0)) {
        e1000_dev.io_base = (uint32_t)pci_get_bar_base(dev, 0);
        e1000_dev.use_mmio = 0;
    } else {
        e1000_dev.mmio_base = (uint32_t)pci_get_bar_base(dev, 0);
        e1000_dev.use_mmio = 1;
    }

    // Enable bus mastering + memory + I/O
    pci_enable_bus_master(dev);
    pci_enable_mem_space(dev);
    pci_enable_io_space(dev);

    return 1;
}

int e1000_init(void) {
    // Clear device state
    for (int i = 0; i < 6; i++) e1000_dev.mac[i] = 0;
    e1000_dev.packets_rx = 0;
    e1000_dev.packets_tx = 0;
    e1000_dev.bytes_rx = 0;
    e1000_dev.bytes_tx = 0;
    e1000_dev.errors = 0;

    // Scan PCI for E1000
    if (!e1000_pci_scan()) {
        e1000_initialized = 0;
        return -1; // No E1000 found
    }

    // Reset device
    e1000_write_reg(E1000_CTRL, E1000_CTRL_RST);
    // Brief delay
    for (volatile int i = 0; i < 100000; i++);

    // Disable interrupts initially
    e1000_write_reg(E1000_IMC, 0xFFFFFFFF);

    // Detect EEPROM and read MAC
    e1000_dev.has_eeprom = e1000_detect_eeprom();
    e1000_read_mac();

    // Clear multicast table
    for (int i = 0; i < 128; i++)
        e1000_write_reg(E1000_MTA + i * 4, 0);

    // Set link up
    uint32_t ctrl = e1000_read_reg(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~E1000_CTRL_RST;
    e1000_write_reg(E1000_CTRL, ctrl);

    // Initialize TX and RX
    e1000_init_rx();
    e1000_init_tx();

    // Enable interrupts
    e1000_write_reg(E1000_IMS, 0x1F6DC);
    e1000_read_reg(E1000_ICR); // Clear pending

    // Check link
    uint32_t status = e1000_read_reg(E1000_STATUS);
    e1000_dev.link_up = (status & 2) ? 1 : 0;

    e1000_initialized = 1;
    return 0;
}

int e1000_is_available(void) {
    return e1000_initialized;
}

void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++)
        mac[i] = e1000_dev.mac[i];
}

int e1000_send(const uint8_t* data, uint16_t length) {
    if (!e1000_initialized || !data || length == 0 || length > E1000_MAX_PKT_SIZE)
        return -1;

    uint16_t cur = e1000_dev.tx_cur;
    e1000_tx_desc_t* desc = &e1000_dev.tx_descs[cur];

    // Wait for descriptor to be available
    while (!(desc->status & E1000_TXD_STAT_DD));

    // Copy data to TX buffer
    uint8_t* buf = e1000_dev.tx_bufs[cur];
    for (uint16_t i = 0; i < length; i++)
        buf[i] = data[i];

    desc->length = length;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;

    e1000_dev.tx_cur = (cur + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(E1000_TDT, e1000_dev.tx_cur);

    e1000_dev.packets_tx++;
    e1000_dev.bytes_tx += length;

    return length;
}

int e1000_receive(uint8_t* buffer, uint16_t max_len) {
    if (!e1000_initialized || !buffer) return 0;

    uint16_t cur = e1000_dev.rx_cur;
    e1000_rx_desc_t* desc = &e1000_dev.rx_descs[cur];

    if (!(desc->status & E1000_RXD_STAT_DD))
        return 0; // No packet available

    uint16_t len = desc->length;
    if (len > max_len) len = max_len;

    // Copy data from RX buffer
    uint8_t* src = e1000_dev.rx_bufs[cur];
    for (uint16_t i = 0; i < len; i++)
        buffer[i] = src[i];

    // Reset descriptor
    desc->status = 0;

    uint16_t old_cur = cur;
    e1000_dev.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
    e1000_write_reg(E1000_RDT, old_cur);

    e1000_dev.packets_rx++;
    e1000_dev.bytes_rx += len;

    return len;
}

void e1000_irq_handler(void) {
    if (!e1000_initialized) return;

    uint32_t icr = e1000_read_reg(E1000_ICR);

    if (icr & 0x04) {
        // Link status change
        uint32_t status = e1000_read_reg(E1000_STATUS);
        e1000_dev.link_up = (status & 2) ? 1 : 0;
    }

    if (icr & 0x80) {
        // Packet received - will be handled by polling
    }
}

int e1000_link_status(void) {
    if (!e1000_initialized) return 0;
    uint32_t status = e1000_read_reg(E1000_STATUS);
    e1000_dev.link_up = (status & 2) ? 1 : 0;
    return e1000_dev.link_up;
}

uint32_t e1000_get_rx_count(void) {
    return e1000_dev.packets_rx;
}

uint32_t e1000_get_tx_count(void) {
    return e1000_dev.packets_tx;
}
