// xhci.h - xHCI (USB 3.0) Host Controller Driver for Alteo OS
#ifndef XHCI_H
#define XHCI_H

#include "stdint.h"
#include "usb.h"
#include "pci.h"

// xHCI Capability Register offsets
#define XHCI_CAP_CAPLENGTH     0x00    // Capability Register Length (1 byte)
#define XHCI_CAP_HCIVERSION    0x02    // Interface Version (2 bytes)
#define XHCI_CAP_HCSPARAMS1    0x04    // Structural Parameters 1
#define XHCI_CAP_HCSPARAMS2    0x08    // Structural Parameters 2
#define XHCI_CAP_HCSPARAMS3    0x0C    // Structural Parameters 3
#define XHCI_CAP_HCCPARAMS1    0x10    // Capability Parameters 1
#define XHCI_CAP_DBOFF         0x14    // Doorbell Offset
#define XHCI_CAP_RTSOFF        0x18    // Runtime Register Space Offset

// xHCI Operational Register offsets (relative to op base)
#define XHCI_OP_USBCMD         0x00    // USB Command
#define XHCI_OP_USBSTS         0x04    // USB Status
#define XHCI_OP_PAGESIZE       0x08    // Page Size
#define XHCI_OP_DNCTRL         0x14    // Device Notification Control
#define XHCI_OP_CRCR           0x18    // Command Ring Control (64-bit)
#define XHCI_OP_DCBAAP         0x30    // Device Context Base Address Array Pointer (64-bit)
#define XHCI_OP_CONFIG         0x38    // Configure

// Port register set (relative to op base + 0x400 + port_index * 0x10)
#define XHCI_PORT_SC           0x00    // Port Status and Control
#define XHCI_PORT_PMSC         0x04    // Port Power Management
#define XHCI_PORT_LI           0x08    // Port Link Info
#define XHCI_PORT_HLPMC        0x0C    // Port Hardware LPM Control

// USBCMD bits
#define XHCI_CMD_RUN            (1 << 0)
#define XHCI_CMD_HCRST          (1 << 1)    // Host Controller Reset
#define XHCI_CMD_INTE           (1 << 2)    // Interrupter Enable
#define XHCI_CMD_HSEE           (1 << 3)    // Host System Error Enable

// USBSTS bits
#define XHCI_STS_HCH            (1 << 0)    // HC Halted
#define XHCI_STS_HSE            (1 << 2)    // Host System Error
#define XHCI_STS_EINT           (1 << 3)    // Event Interrupt
#define XHCI_STS_PCD            (1 << 4)    // Port Change Detect
#define XHCI_STS_CNR            (1 << 11)   // Controller Not Ready

// Port Status bits
#define XHCI_PORT_CCS           (1 << 0)    // Current Connect Status
#define XHCI_PORT_PED           (1 << 1)    // Port Enabled/Disabled
#define XHCI_PORT_OCA           (1 << 3)    // Over-current Active
#define XHCI_PORT_PR            (1 << 4)    // Port Reset
#define XHCI_PORT_PP            (1 << 9)    // Port Power
#define XHCI_PORT_CSC           (1 << 17)   // Connect Status Change
#define XHCI_PORT_PEC           (1 << 18)   // Port Enabled Change
#define XHCI_PORT_PRC           (1 << 21)   // Port Reset Change
#define XHCI_PORT_SPEED_MASK    (0xF << 10) // Port Speed

// Port speed values (bits 13:10 of PORTSC)
#define XHCI_PORT_SPEED_FS      1       // Full Speed (12 Mbps)
#define XHCI_PORT_SPEED_LS      2       // Low Speed (1.5 Mbps)
#define XHCI_PORT_SPEED_HS      3       // High Speed (480 Mbps)
#define XHCI_PORT_SPEED_SS      4       // Super Speed (5 Gbps)

// TRB types
#define XHCI_TRB_NORMAL         1
#define XHCI_TRB_SETUP          2
#define XHCI_TRB_DATA           3
#define XHCI_TRB_STATUS         4
#define XHCI_TRB_LINK           6
#define XHCI_TRB_EVENT_DATA     7
#define XHCI_TRB_NOOP           8
#define XHCI_TRB_ENABLE_SLOT    9
#define XHCI_TRB_DISABLE_SLOT   10
#define XHCI_TRB_ADDRESS_DEV    11
#define XHCI_TRB_CONFIG_EP      12
#define XHCI_TRB_EVALUATE_CTX   13
#define XHCI_TRB_RESET_EP       14
#define XHCI_TRB_STOP_EP        15
#define XHCI_TRB_SET_TR_DEQUEUE 16
#define XHCI_TRB_NOOP_CMD       23

// TRB completion codes
#define XHCI_TRB_CC_SUCCESS     1
#define XHCI_TRB_CC_SHORT_PKT   13

// Event TRB types
#define XHCI_TRB_TRANSFER_EVENT 32
#define XHCI_TRB_CMD_COMPLETE   33
#define XHCI_TRB_PORT_STATUS    34

// TRB (Transfer Request Block) - 16 bytes
typedef struct __attribute__((packed)) {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

// Max values
#define XHCI_MAX_SLOTS          64
#define XHCI_MAX_PORTS          16
#define XHCI_CMD_RING_SIZE      64
#define XHCI_EVENT_RING_SIZE    64
#define XHCI_TRANSFER_RING_SIZE 64

// ---- API ----

// Initialize xHCI controller (finds via PCI)
int xhci_init(void);

// Check if xHCI is available
int xhci_is_available(void);

// Get number of ports
int xhci_get_port_count(void);

// Check port connect status
int xhci_port_connected(int port);

// Get port speed (returns USB_SPEED_* constant)
int xhci_port_speed(int port);

// Reset a port (triggers device enumeration)
int xhci_port_reset(int port);

// Submit a control transfer
int xhci_control_transfer(int slot_id, usb_setup_packet_t* setup,
                          void* data, uint16_t data_len);

// Submit an interrupt IN transfer (for HID polling)
int xhci_interrupt_transfer(int slot_id, uint8_t endpoint, void* data, uint16_t data_len);

// IRQ handler for xHCI
void xhci_irq_handler(void);

#endif
