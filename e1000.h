// e1000.h - Intel E1000 NIC Driver for Alteo OS
#ifndef E1000_H
#define E1000_H

#include "stdint.h"

// PCI Configuration
#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_ID     0x100E  // 82540EM
#define E1000_DEVICE_ID2    0x100F  // 82545EM
#define E1000_DEVICE_ID3    0x10D3  // 82574L

// Register offsets
#define E1000_CTRL          0x0000  // Device Control
#define E1000_STATUS        0x0008  // Device Status
#define E1000_EECD          0x0010  // EEPROM Control
#define E1000_EERD          0x0014  // EEPROM Read
#define E1000_ICR           0x00C0  // Interrupt Cause Read
#define E1000_IMS           0x00D0  // Interrupt Mask Set
#define E1000_IMC           0x00D8  // Interrupt Mask Clear
#define E1000_RCTL          0x0100  // Receive Control
#define E1000_TCTL          0x0400  // Transmit Control
#define E1000_RDBAL         0x2800  // RX Descriptor Base Low
#define E1000_RDBAH         0x2804  // RX Descriptor Base High
#define E1000_RDLEN         0x2808  // RX Descriptor Length
#define E1000_RDH           0x2810  // RX Descriptor Head
#define E1000_RDT           0x2818  // RX Descriptor Tail
#define E1000_TDBAL         0x3800  // TX Descriptor Base Low
#define E1000_TDBAH         0x3804  // TX Descriptor Base High
#define E1000_TDLEN         0x3808  // TX Descriptor Length
#define E1000_TDH           0x3810  // TX Descriptor Head
#define E1000_TDT           0x3818  // TX Descriptor Tail
#define E1000_RAL           0x5400  // Receive Address Low
#define E1000_RAH           0x5404  // Receive Address High
#define E1000_MTA           0x5200  // Multicast Table Array

// Control register bits
#define E1000_CTRL_SLU      (1 << 6)   // Set Link Up
#define E1000_CTRL_RST      (1 << 26)  // Device Reset
#define E1000_CTRL_ASDE     (1 << 5)   // Auto-Speed Detection

// Receive control bits
#define E1000_RCTL_EN       (1 << 1)   // Receiver Enable
#define E1000_RCTL_SBP      (1 << 2)   // Store Bad Packets
#define E1000_RCTL_UPE      (1 << 3)   // Unicast Promisc
#define E1000_RCTL_MPE      (1 << 4)   // Multicast Promisc
#define E1000_RCTL_BAM      (1 << 15)  // Broadcast Accept
#define E1000_RCTL_BSIZE_256  (3 << 16)
#define E1000_RCTL_BSIZE_512  (2 << 16)
#define E1000_RCTL_BSIZE_1024 (1 << 16)
#define E1000_RCTL_BSIZE_2048 (0 << 16)
#define E1000_RCTL_BSIZE_4096 ((3 << 16) | (1 << 25))
#define E1000_RCTL_SECRC    (1 << 26)  // Strip CRC

// Transmit control bits
#define E1000_TCTL_EN       (1 << 1)   // Transmitter Enable
#define E1000_TCTL_PSP      (1 << 3)   // Pad Short Packets
#define E1000_TCTL_CT_SHIFT 4          // Collision Threshold
#define E1000_TCTL_COLD_SHIFT 12       // Collision Distance

// TX descriptor command bits
#define E1000_TXD_CMD_EOP   (1 << 0)  // End of Packet
#define E1000_TXD_CMD_IFCS  (1 << 1)  // Insert FCS
#define E1000_TXD_CMD_RS    (1 << 3)  // Report Status
#define E1000_TXD_STAT_DD   (1 << 0)  // Descriptor Done

// RX descriptor status bits
#define E1000_RXD_STAT_DD   (1 << 0)  // Descriptor Done
#define E1000_RXD_STAT_EOP  (1 << 1)  // End of Packet

// Ring buffer sizes
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32
#define E1000_RX_BUF_SIZE   2048
#define E1000_TX_BUF_SIZE   2048
#define E1000_MAX_PKT_SIZE  1518

// RX Descriptor
typedef struct __attribute__((packed)) {
    uint64_t addr;       // Buffer address
    uint16_t length;     // Packet length
    uint16_t checksum;   // Packet checksum
    uint8_t  status;     // Descriptor status
    uint8_t  errors;     // Descriptor errors
    uint16_t special;
} e1000_rx_desc_t;

// TX Descriptor
typedef struct __attribute__((packed)) {
    uint64_t addr;       // Buffer address
    uint16_t length;     // Data length
    uint8_t  cso;        // Checksum offset
    uint8_t  cmd;        // Command
    uint8_t  status;     // Status
    uint8_t  css;        // Checksum start
    uint16_t special;
} e1000_tx_desc_t;

// E1000 device structure
typedef struct {
    uint32_t mmio_base;     // MMIO base address
    uint32_t io_base;       // I/O base address (legacy)
    int      use_mmio;      // Whether to use MMIO
    uint8_t  mac[6];        // MAC address
    int      has_eeprom;    // EEPROM present
    int      link_up;       // Link status

    // RX ring buffer
    e1000_rx_desc_t* rx_descs;
    uint8_t* rx_bufs[E1000_NUM_RX_DESC];
    uint16_t rx_cur;

    // TX ring buffer
    e1000_tx_desc_t* tx_descs;
    uint8_t* tx_bufs[E1000_NUM_TX_DESC];
    uint16_t tx_cur;

    // Statistics
    uint32_t packets_rx;
    uint32_t packets_tx;
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    uint32_t errors;
} e1000_device_t;

// Initialize E1000 driver (scan PCI, set up rings)
int  e1000_init(void);

// Check if NIC is present and initialized
int  e1000_is_available(void);

// Get MAC address
void e1000_get_mac(uint8_t mac[6]);

// Send a raw ethernet frame
int  e1000_send(const uint8_t* data, uint16_t length);

// Receive a raw ethernet frame (returns bytes read, 0 if none)
int  e1000_receive(uint8_t* buffer, uint16_t max_len);

// Handle E1000 interrupt
void e1000_irq_handler(void);

// Get link status
int  e1000_link_status(void);

// Get statistics
uint32_t e1000_get_rx_count(void);
uint32_t e1000_get_tx_count(void);

#endif
