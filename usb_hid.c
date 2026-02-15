// usb_hid.c - USB HID (Human Interface Device) Driver for Alteo OS
#include "usb_hid.h"
#include "xhci.h"

static int hid_keyboard_slot = -1;
static int hid_keyboard_ep = 0;
static int hid_mouse_slot = -1;
static int hid_mouse_ep = 0;

static usb_hid_keyboard_report_t last_kb_report;
static usb_hid_mouse_report_t last_mouse_report;

static void hid_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

// HID Usage ID to ASCII conversion table (US QWERTY)
// Index = HID usage code (0x04 = 'a', etc.)
static const char hid_keymap_lower[128] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd',  // 0x00-0x07
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',  // 0x08-0x0F
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't',  // 0x10-0x17
    'u', 'v', 'w', 'x', 'y', 'z', '1', '2',  // 0x18-0x1F
    '3', '4', '5', '6', '7', '8', '9', '0',  // 0x20-0x27
    '\n', 27,  '\b', '\t', ' ', '-', '=', '[', // 0x28-0x2F
    ']', '\\', 0,   ';', '\'', '`', ',', '.', // 0x30-0x37
    '/', 0,   0,   0,   0,   0,   0,   0,     // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   0,     // 0x40-0x47
    0,   0,   0,   0,   0,   0,   0,   0,     // 0x48-0x4F
    0,   0,   0,   0,   '/', '*', '-', '+',   // 0x50-0x57
    '\n', '1', '2', '3', '4', '5', '6', '7',  // 0x58-0x5F
    '8', '9', '0', '.', 0,   0,   0,   0,     // 0x60-0x67
    0,   0,   0,   0,   0,   0,   0,   0,     // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,     // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0,     // 0x78-0x7F
};

static const char hid_keymap_upper[128] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D',
    'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@',
    '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27,  '\b', '\t', ' ', '_', '+', '{',
    '}', '|', 0,   ':', '"', '~', '<', '>',
    '?', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   '/', '*', '-', '+',
    '\n', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

void usb_hid_init(void) {
    hid_memset(&last_kb_report, 0, sizeof(last_kb_report));
    hid_memset(&last_mouse_report, 0, sizeof(last_mouse_report));

    hid_keyboard_slot = -1;
    hid_mouse_slot = -1;

    // Scan USB devices for HID keyboard and mouse
    int count = usb_get_device_count();
    for (int i = 0; i < count; i++) {
        usb_device_t* dev = usb_get_device(i);
        if (!dev) continue;

        if (dev->class_code == USB_CLASS_HID) {
            if (dev->protocol == USB_HID_PROTOCOL_KEYBOARD && hid_keyboard_slot < 0) {
                hid_keyboard_slot = dev->slot_id;
                // Find interrupt IN endpoint
                for (int e = 0; e < dev->num_endpoints; e++) {
                    if ((dev->endpoints[e].type == USB_EP_TYPE_INTERRUPT) &&
                        (dev->endpoints[e].address & USB_EP_DIR_IN)) {
                        hid_keyboard_ep = dev->endpoints[e].address & USB_EP_NUM_MASK;
                        break;
                    }
                }
            }
            if (dev->protocol == USB_HID_PROTOCOL_MOUSE && hid_mouse_slot < 0) {
                hid_mouse_slot = dev->slot_id;
                for (int e = 0; e < dev->num_endpoints; e++) {
                    if ((dev->endpoints[e].type == USB_EP_TYPE_INTERRUPT) &&
                        (dev->endpoints[e].address & USB_EP_DIR_IN)) {
                        hid_mouse_ep = dev->endpoints[e].address & USB_EP_NUM_MASK;
                        break;
                    }
                }
            }
        }
    }

    // Set boot protocol for discovered HID devices
    if (hid_keyboard_slot >= 0) {
        usb_setup_packet_t setup;
        hid_memset(&setup, 0, sizeof(setup));
        setup.bmRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT;
        setup.bRequest = USB_HID_SET_PROTOCOL;
        setup.wValue = 0; // Boot protocol
        setup.wIndex = 0;
        setup.wLength = 0;
        xhci_control_transfer(hid_keyboard_slot, &setup, (void*)0, 0);

        // Set idle (suppress duplicate reports)
        hid_memset(&setup, 0, sizeof(setup));
        setup.bmRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT;
        setup.bRequest = USB_HID_SET_IDLE;
        setup.wValue = 0;
        setup.wIndex = 0;
        setup.wLength = 0;
        xhci_control_transfer(hid_keyboard_slot, &setup, (void*)0, 0);
    }

    if (hid_mouse_slot >= 0) {
        usb_setup_packet_t setup;
        hid_memset(&setup, 0, sizeof(setup));
        setup.bmRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT;
        setup.bRequest = USB_HID_SET_PROTOCOL;
        setup.wValue = 0; // Boot protocol
        setup.wIndex = 0;
        setup.wLength = 0;
        xhci_control_transfer(hid_mouse_slot, &setup, (void*)0, 0);
    }
}

int usb_hid_keyboard_available(void) {
    return (hid_keyboard_slot >= 0) ? 1 : 0;
}

int usb_hid_mouse_available(void) {
    return (hid_mouse_slot >= 0) ? 1 : 0;
}

int usb_hid_keyboard_poll(usb_hid_keyboard_report_t* report) {
    if (hid_keyboard_slot < 0 || !report) return 0;

    usb_hid_keyboard_report_t new_report;
    hid_memset(&new_report, 0, sizeof(new_report));

    int ret = xhci_interrupt_transfer(hid_keyboard_slot, hid_keyboard_ep,
                                       &new_report, sizeof(new_report));
    if (ret != 0) return 0;

    // Check if report changed
    const uint8_t* a = (const uint8_t*)&new_report;
    const uint8_t* b = (const uint8_t*)&last_kb_report;
    int changed = 0;
    for (int i = 0; i < (int)sizeof(usb_hid_keyboard_report_t); i++) {
        if (a[i] != b[i]) { changed = 1; break; }
    }

    if (changed) {
        last_kb_report = new_report;
        *report = new_report;
        return 1;
    }
    return 0;
}

int usb_hid_mouse_poll(usb_hid_mouse_report_t* report) {
    if (hid_mouse_slot < 0 || !report) return 0;

    usb_hid_mouse_report_t new_report;
    hid_memset(&new_report, 0, sizeof(new_report));

    int ret = xhci_interrupt_transfer(hid_mouse_slot, hid_mouse_ep,
                                       &new_report, sizeof(new_report));
    if (ret != 0) return 0;

    *report = new_report;
    return 1; // Mouse reports are always "new" (they contain deltas)
}

char usb_hid_to_ascii(uint8_t keycode, uint8_t modifiers) {
    if (keycode >= 128) return 0;
    int shift = (modifiers & (USB_HID_MOD_LSHIFT | USB_HID_MOD_RSHIFT)) ? 1 : 0;
    return shift ? hid_keymap_upper[keycode] : hid_keymap_lower[keycode];
}
