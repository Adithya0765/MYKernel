// xhci.c - xHCI (USB 3.0) Host Controller Driver for Alteo OS
// Minimal implementation: init, port detect, basic control transfers
#include "xhci.h"
#include "pci.h"
#include "heap.h"

// ---- Helpers ----
static void xhci_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void xhci_memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

// ---- State ----
static volatile uint8_t*  xhci_mmio = 0;       // Base MMIO address
static volatile uint8_t*  xhci_op = 0;         // Operational registers base
static volatile uint32_t* xhci_doorbell = 0;    // Doorbell registers
static volatile uint8_t*  xhci_runtime = 0;     // Runtime registers

static int xhci_available = 0;
static int xhci_max_slots = 0;
static int xhci_max_ports = 0;
static int xhci_page_size = 0;

// Rings
static xhci_trb_t* cmd_ring = 0;
static int cmd_ring_enqueue = 0;
static int cmd_ring_cycle = 1;

static xhci_trb_t* event_ring = 0;
static int event_ring_dequeue = 0;
static int event_ring_cycle = 1;

// Event Ring Segment Table Entry
typedef struct __attribute__((packed)) {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} xhci_erst_entry_t;

static xhci_erst_entry_t* erst = 0;

// Device Context Base Address Array
static uint64_t* dcbaap = 0;

// Device contexts (one per slot)
static uint8_t* device_contexts[XHCI_MAX_SLOTS];

// Transfer rings (one per slot, endpoint 0 only for now)
static xhci_trb_t* transfer_rings[XHCI_MAX_SLOTS];
static int transfer_ring_enqueue[XHCI_MAX_SLOTS];
static int transfer_ring_cycle[XHCI_MAX_SLOTS];

// ---- MMIO Access ----
static inline uint32_t xhci_read32(volatile uint8_t* base, uint32_t offset) {
    return *(volatile uint32_t*)(base + offset);
}

static inline void xhci_write32(volatile uint8_t* base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(base + offset) = val;
}

static inline uint64_t xhci_read64(volatile uint8_t* base, uint32_t offset) {
    uint32_t lo = *(volatile uint32_t*)(base + offset);
    uint32_t hi = *(volatile uint32_t*)(base + offset + 4);
    return ((uint64_t)hi << 32) | lo;
}

static inline void xhci_write64(volatile uint8_t* base, uint32_t offset, uint64_t val) {
    *(volatile uint32_t*)(base + offset) = (uint32_t)(val & 0xFFFFFFFF);
    *(volatile uint32_t*)(base + offset + 4) = (uint32_t)(val >> 32);
}

// ---- Command Ring Operations ----

static void xhci_cmd_ring_enqueue_trb(xhci_trb_t* trb) {
    // Copy TRB to command ring, set cycle bit
    cmd_ring[cmd_ring_enqueue].parameter = trb->parameter;
    cmd_ring[cmd_ring_enqueue].status = trb->status;
    cmd_ring[cmd_ring_enqueue].control = (trb->control & ~1u) | cmd_ring_cycle;

    cmd_ring_enqueue++;
    if (cmd_ring_enqueue >= XHCI_CMD_RING_SIZE - 1) {
        // Link TRB: wrap around
        cmd_ring[cmd_ring_enqueue].parameter = (uint64_t)(uintptr_t)cmd_ring;
        cmd_ring[cmd_ring_enqueue].status = 0;
        cmd_ring[cmd_ring_enqueue].control = (XHCI_TRB_LINK << 10) | (1 << 1) | cmd_ring_cycle;
        cmd_ring_cycle ^= 1;
        cmd_ring_enqueue = 0;
    }
}

static void xhci_ring_doorbell(int slot_id, int target) {
    xhci_doorbell[slot_id] = target;
}

// ---- Event Handling ----

static xhci_trb_t* xhci_poll_event(void) {
    xhci_trb_t* trb = &event_ring[event_ring_dequeue];
    if ((trb->control & 1) != (uint32_t)event_ring_cycle) {
        return (xhci_trb_t*)0; // No event
    }
    return trb;
}

static void xhci_advance_event(void) {
    event_ring_dequeue++;
    if (event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
        event_ring_dequeue = 0;
        event_ring_cycle ^= 1;
    }

    // Update ERDP (Event Ring Dequeue Pointer) in interrupter 0
    uint64_t erdp = (uint64_t)(uintptr_t)&event_ring[event_ring_dequeue];
    erdp |= (1 << 3); // EHB (Event Handler Busy) clear
    xhci_write64(xhci_runtime, 0x20 + 0x18, erdp); // Interrupter 0 ERDP at offset 0x38
}

// Wait for a command completion event
static int xhci_wait_event(uint32_t trb_type, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        xhci_trb_t* evt = xhci_poll_event();
        if (evt) {
            uint32_t type = (evt->control >> 10) & 0x3F;
            if (type == trb_type) {
                uint32_t cc = (evt->status >> 24) & 0xFF;
                xhci_advance_event();
                return (cc == XHCI_TRB_CC_SUCCESS || cc == XHCI_TRB_CC_SHORT_PKT) ? 0 : -1;
            }
            xhci_advance_event();
        }
        // Small delay
        for (volatile int d = 0; d < 1000; d++);
    }
    return -1; // Timeout
}

// ---- Port Operations ----

static volatile uint8_t* xhci_port_base(int port) {
    return xhci_op + 0x400 + (uint32_t)port * 0x10;
}

int xhci_port_connected(int port) {
    if (!xhci_available || port < 0 || port >= xhci_max_ports) return 0;
    volatile uint8_t* pb = xhci_port_base(port);
    uint32_t portsc = xhci_read32(pb, XHCI_PORT_SC);
    return (portsc & XHCI_PORT_CCS) ? 1 : 0;
}

int xhci_port_speed(int port) {
    if (!xhci_available || port < 0 || port >= xhci_max_ports) return 0;
    volatile uint8_t* pb = xhci_port_base(port);
    uint32_t portsc = xhci_read32(pb, XHCI_PORT_SC);
    int speed = (portsc >> 10) & 0xF;
    switch (speed) {
        case XHCI_PORT_SPEED_LS: return USB_SPEED_LOW;
        case XHCI_PORT_SPEED_FS: return USB_SPEED_FULL;
        case XHCI_PORT_SPEED_HS: return USB_SPEED_HIGH;
        case XHCI_PORT_SPEED_SS: return USB_SPEED_SUPER;
        default: return USB_SPEED_FULL;
    }
}

int xhci_port_reset(int port) {
    if (!xhci_available || port < 0 || port >= xhci_max_ports) return -1;
    volatile uint8_t* pb = xhci_port_base(port);

    uint32_t portsc = xhci_read32(pb, XHCI_PORT_SC);
    if (!(portsc & XHCI_PORT_CCS)) return -1; // No device connected

    // Write Port Reset bit (preserve PP, clear status change bits by writing 1)
    portsc = (portsc & 0x0E00) | XHCI_PORT_PR | XHCI_PORT_PP;
    xhci_write32(pb, XHCI_PORT_SC, portsc);

    // Wait for reset to complete
    for (int i = 0; i < 100000; i++) {
        portsc = xhci_read32(pb, XHCI_PORT_SC);
        if (portsc & XHCI_PORT_PRC) {
            // Clear Port Reset Change
            xhci_write32(pb, XHCI_PORT_SC, (portsc & 0x0E00) | XHCI_PORT_PRC | XHCI_PORT_PP);
            return 0;
        }
        for (volatile int d = 0; d < 100; d++);
    }
    return -1; // Reset timeout
}

// ---- Control Transfer ----

int xhci_control_transfer(int slot_id, usb_setup_packet_t* setup,
                          void* data, uint16_t data_len) {
    if (!xhci_available || slot_id < 1 || slot_id > xhci_max_slots) return -1;
    if (!transfer_rings[slot_id - 1]) return -1;

    xhci_trb_t* ring = transfer_rings[slot_id - 1];
    int* enq = &transfer_ring_enqueue[slot_id - 1];
    int* cyc = &transfer_ring_cycle[slot_id - 1];

    // Setup Stage TRB
    xhci_trb_t setup_trb;
    xhci_memset(&setup_trb, 0, sizeof(xhci_trb_t));
    xhci_memcpy(&setup_trb.parameter, setup, 8);
    setup_trb.status = 8; // Transfer length = 8 bytes for setup
    setup_trb.control = (XHCI_TRB_SETUP << 10) | (1 << 6); // IDT bit
    if (data_len > 0) {
        // TRT: 2 = OUT data, 3 = IN data
        uint32_t trt = (setup->bmRequestType & USB_DIR_IN) ? 3 : 2;
        setup_trb.control |= (trt << 16);
    }

    ring[*enq].parameter = setup_trb.parameter;
    ring[*enq].status = setup_trb.status;
    ring[*enq].control = (setup_trb.control & ~1u) | *cyc;
    (*enq)++;

    // Data Stage TRB (if data)
    if (data && data_len > 0) {
        xhci_trb_t data_trb;
        xhci_memset(&data_trb, 0, sizeof(xhci_trb_t));
        data_trb.parameter = (uint64_t)(uintptr_t)data;
        data_trb.status = data_len;
        data_trb.control = (XHCI_TRB_DATA << 10);
        if (setup->bmRequestType & USB_DIR_IN) {
            data_trb.control |= (1 << 16); // DIR = IN
        }

        ring[*enq].parameter = data_trb.parameter;
        ring[*enq].status = data_trb.status;
        ring[*enq].control = (data_trb.control & ~1u) | *cyc;
        (*enq)++;
    }

    // Status Stage TRB
    xhci_trb_t status_trb;
    xhci_memset(&status_trb, 0, sizeof(xhci_trb_t));
    status_trb.control = (XHCI_TRB_STATUS << 10) | (1 << 5); // IOC bit
    // Direction is opposite of data stage
    if (data_len == 0 || !(setup->bmRequestType & USB_DIR_IN)) {
        status_trb.control |= (1 << 16); // DIR = IN for status
    }

    ring[*enq].parameter = status_trb.parameter;
    ring[*enq].status = status_trb.status;
    ring[*enq].control = (status_trb.control & ~1u) | *cyc;
    (*enq)++;

    // Handle wrap
    if (*enq >= XHCI_TRANSFER_RING_SIZE - 1) {
        ring[*enq].parameter = (uint64_t)(uintptr_t)ring;
        ring[*enq].status = 0;
        ring[*enq].control = (XHCI_TRB_LINK << 10) | (1 << 1) | *cyc;
        *cyc ^= 1;
        *enq = 0;
    }

    // Ring doorbell for endpoint 0 (target = 1 for EP0)
    xhci_ring_doorbell(slot_id, 1);

    // Wait for completion
    return xhci_wait_event(XHCI_TRB_TRANSFER_EVENT, 500000);
}

int xhci_interrupt_transfer(int slot_id, uint8_t endpoint, void* data, uint16_t data_len) {
    if (!xhci_available || slot_id < 1 || slot_id > xhci_max_slots) return -1;
    if (!transfer_rings[slot_id - 1]) return -1;

    xhci_trb_t* ring = transfer_rings[slot_id - 1];
    int* enq = &transfer_ring_enqueue[slot_id - 1];
    int* cyc = &transfer_ring_cycle[slot_id - 1];

    // Normal TRB for interrupt transfer
    xhci_trb_t trb;
    xhci_memset(&trb, 0, sizeof(xhci_trb_t));
    trb.parameter = (uint64_t)(uintptr_t)data;
    trb.status = data_len;
    trb.control = (XHCI_TRB_NORMAL << 10) | (1 << 5); // IOC

    ring[*enq].parameter = trb.parameter;
    ring[*enq].status = trb.status;
    ring[*enq].control = (trb.control & ~1u) | *cyc;
    (*enq)++;

    if (*enq >= XHCI_TRANSFER_RING_SIZE - 1) {
        ring[*enq].parameter = (uint64_t)(uintptr_t)ring;
        ring[*enq].status = 0;
        ring[*enq].control = (XHCI_TRB_LINK << 10) | (1 << 1) | *cyc;
        *cyc ^= 1;
        *enq = 0;
    }

    // Doorbell target: DCI = endpoint * 2 + direction
    // For interrupt IN endpoint N: target = (endpoint_num * 2) + 1
    uint8_t ep_num = endpoint & USB_EP_NUM_MASK;
    uint32_t target = ep_num * 2 + 1;
    xhci_ring_doorbell(slot_id, target);

    return xhci_wait_event(XHCI_TRB_TRANSFER_EVENT, 500000);
}

// ---- Initialization ----

int xhci_init(void) {
    xhci_available = 0;

    // Find xHCI controller via PCI (class 0x0C, subclass 0x03, prog_if 0x30)
    pci_device_t* dev = pci_find_class_prog(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                                             PCI_PROG_IF_XHCI, (pci_device_t*)0);
    if (!dev) {
        return -1; // No xHCI controller found
    }

    // Get BAR0 (MMIO base)
    uint64_t bar0 = pci_get_bar_base(dev, 0);
    if (bar0 == 0) return -1;

    // Enable bus mastering and memory space
    pci_enable_bus_master(dev);
    pci_enable_mem_space(dev);

    xhci_mmio = (volatile uint8_t*)(uintptr_t)bar0;

    // Read capability registers
    uint8_t cap_length = *(volatile uint8_t*)xhci_mmio;
    uint32_t hcsparams1 = xhci_read32(xhci_mmio, XHCI_CAP_HCSPARAMS1);
    uint32_t hccparams1 = xhci_read32(xhci_mmio, XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = xhci_read32(xhci_mmio, XHCI_CAP_DBOFF);
    uint32_t rtsoff = xhci_read32(xhci_mmio, XHCI_CAP_RTSOFF);
    (void)hccparams1;

    xhci_max_slots = hcsparams1 & 0xFF;
    xhci_max_ports = (hcsparams1 >> 24) & 0xFF;
    if (xhci_max_slots > XHCI_MAX_SLOTS) xhci_max_slots = XHCI_MAX_SLOTS;
    if (xhci_max_ports > XHCI_MAX_PORTS) xhci_max_ports = XHCI_MAX_PORTS;

    // Set register bases
    xhci_op = xhci_mmio + cap_length;
    xhci_doorbell = (volatile uint32_t*)(xhci_mmio + dboff);
    xhci_runtime = xhci_mmio + rtsoff;

    // ---- Reset Controller ----

    // Stop the controller
    uint32_t usbcmd = xhci_read32(xhci_op, XHCI_OP_USBCMD);
    usbcmd &= ~XHCI_CMD_RUN;
    xhci_write32(xhci_op, XHCI_OP_USBCMD, usbcmd);

    // Wait for HCH (Halted)
    for (int i = 0; i < 100000; i++) {
        if (xhci_read32(xhci_op, XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
        for (volatile int d = 0; d < 100; d++);
    }

    // Reset
    xhci_write32(xhci_op, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 1000000; i++) {
        uint32_t cmd = xhci_read32(xhci_op, XHCI_OP_USBCMD);
        uint32_t sts = xhci_read32(xhci_op, XHCI_OP_USBSTS);
        if (!(cmd & XHCI_CMD_HCRST) && !(sts & XHCI_STS_CNR)) break;
        for (volatile int d = 0; d < 100; d++);
    }

    // Read page size
    xhci_page_size = (xhci_read32(xhci_op, XHCI_OP_PAGESIZE) & 0xFFFF) << 12;
    if (xhci_page_size == 0) xhci_page_size = 4096;

    // ---- Configure Max Slots ----
    xhci_write32(xhci_op, XHCI_OP_CONFIG, xhci_max_slots);

    // ---- Set up DCBAAP ----
    dcbaap = (uint64_t*)kmalloc(sizeof(uint64_t) * (xhci_max_slots + 1) + 64);
    if (!dcbaap) return -1;
    // Align to 64 bytes
    dcbaap = (uint64_t*)(((uintptr_t)dcbaap + 63) & ~63ULL);
    xhci_memset(dcbaap, 0, sizeof(uint64_t) * (xhci_max_slots + 1));
    xhci_write64(xhci_op, XHCI_OP_DCBAAP, (uint64_t)(uintptr_t)dcbaap);

    // ---- Set up Command Ring ----
    cmd_ring = (xhci_trb_t*)kmalloc(sizeof(xhci_trb_t) * XHCI_CMD_RING_SIZE + 64);
    if (!cmd_ring) return -1;
    cmd_ring = (xhci_trb_t*)(((uintptr_t)cmd_ring + 63) & ~63ULL);
    xhci_memset(cmd_ring, 0, sizeof(xhci_trb_t) * XHCI_CMD_RING_SIZE);
    cmd_ring_enqueue = 0;
    cmd_ring_cycle = 1;

    // Set CRCR (Command Ring Control Register)
    uint64_t crcr = (uint64_t)(uintptr_t)cmd_ring | cmd_ring_cycle;
    xhci_write64(xhci_op, XHCI_OP_CRCR, crcr);

    // ---- Set up Event Ring ----
    event_ring = (xhci_trb_t*)kmalloc(sizeof(xhci_trb_t) * XHCI_EVENT_RING_SIZE + 64);
    if (!event_ring) return -1;
    event_ring = (xhci_trb_t*)(((uintptr_t)event_ring + 63) & ~63ULL);
    xhci_memset(event_ring, 0, sizeof(xhci_trb_t) * XHCI_EVENT_RING_SIZE);
    event_ring_dequeue = 0;
    event_ring_cycle = 1;

    // Event Ring Segment Table
    erst = (xhci_erst_entry_t*)kmalloc(sizeof(xhci_erst_entry_t) + 64);
    if (!erst) return -1;
    erst = (xhci_erst_entry_t*)(((uintptr_t)erst + 63) & ~63ULL);
    erst->ring_segment_base = (uint64_t)(uintptr_t)event_ring;
    erst->ring_segment_size = XHCI_EVENT_RING_SIZE;
    erst->reserved = 0;

    // Program Interrupter 0 (runtime registers + 0x20)
    // ERSTSZ = 1 segment
    xhci_write32(xhci_runtime, 0x20 + 0x08, 1);
    // ERDP = event ring base
    xhci_write64(xhci_runtime, 0x20 + 0x18, (uint64_t)(uintptr_t)event_ring);
    // ERSTBA = ERST base address
    xhci_write64(xhci_runtime, 0x20 + 0x10, (uint64_t)(uintptr_t)erst);

    // ---- Init transfer ring arrays ----
    xhci_memset(device_contexts, 0, sizeof(device_contexts));
    xhci_memset(transfer_rings, 0, sizeof(transfer_rings));
    xhci_memset(transfer_ring_enqueue, 0, sizeof(transfer_ring_enqueue));
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) transfer_ring_cycle[i] = 1;

    // ---- Start Controller ----
    usbcmd = xhci_read32(xhci_op, XHCI_OP_USBCMD);
    usbcmd |= XHCI_CMD_RUN | XHCI_CMD_INTE;
    xhci_write32(xhci_op, XHCI_OP_USBCMD, usbcmd);

    // Wait for controller to start (HCH bit clears)
    for (int i = 0; i < 100000; i++) {
        if (!(xhci_read32(xhci_op, XHCI_OP_USBSTS) & XHCI_STS_HCH)) break;
        for (volatile int d = 0; d < 100; d++);
    }

    xhci_available = 1;
    return 0;
}

int xhci_is_available(void) {
    return xhci_available;
}

int xhci_get_port_count(void) {
    return xhci_max_ports;
}

void xhci_irq_handler(void) {
    if (!xhci_available) return;

    // Clear EINT (Event Interrupt) in USBSTS
    uint32_t sts = xhci_read32(xhci_op, XHCI_OP_USBSTS);
    if (sts & XHCI_STS_EINT) {
        xhci_write32(xhci_op, XHCI_OP_USBSTS, XHCI_STS_EINT);
    }

    // Clear Interrupt Pending (IP) in interrupter 0
    uint32_t iman = xhci_read32(xhci_runtime, 0x20);
    if (iman & 1) {
        xhci_write32(xhci_runtime, 0x20, iman | 1);
    }

    // Process events
    while (1) {
        xhci_trb_t* evt = xhci_poll_event();
        if (!evt) break;

        uint32_t type = (evt->control >> 10) & 0x3F;
        (void)type; // Port change and transfer events handled by polling for now

        xhci_advance_event();
    }
}
