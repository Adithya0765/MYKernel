// usb.c - USB Core Layer for Alteo OS
// Manages device enumeration and tracking
#include "usb.h"
#include "xhci.h"

static usb_device_t usb_devices[USB_MAX_DEVICES];
static int usb_device_count = 0;

static void usb_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

void usb_init(void) {
    usb_memset(usb_devices, 0, sizeof(usb_devices));
    usb_device_count = 0;

    // Try to initialize xHCI
    if (xhci_init() != 0) {
        return; // No USB controller found
    }

    // Scan ports for connected devices
    int ports = xhci_get_port_count();
    for (int p = 0; p < ports && usb_device_count < USB_MAX_DEVICES; p++) {
        if (xhci_port_connected(p)) {
            // Reset port
            if (xhci_port_reset(p) == 0) {
                usb_device_t* dev = &usb_devices[usb_device_count];
                usb_memset(dev, 0, sizeof(usb_device_t));
                dev->present = 1;
                dev->speed = (uint8_t)xhci_port_speed(p);
                dev->slot_id = usb_device_count + 1; // Simplified
                usb_device_count++;
            }
        }
    }
}

usb_device_t* usb_get_device(int index) {
    if (index < 0 || index >= USB_MAX_DEVICES) return (usb_device_t*)0;
    if (!usb_devices[index].present) return (usb_device_t*)0;
    return &usb_devices[index];
}

int usb_get_device_count(void) {
    return usb_device_count;
}
