// usb_hid.h - USB HID (Human Interface Device) Driver for Alteo OS
// Supports USB keyboard and mouse via HID protocol
#ifndef USB_HID_H
#define USB_HID_H

#include "stdint.h"
#include "usb.h"

// HID boot protocol report sizes
#define USB_HID_KEYBOARD_REPORT_SIZE    8
#define USB_HID_MOUSE_REPORT_SIZE       4

// Keyboard boot protocol report
typedef struct __attribute__((packed)) {
    uint8_t modifiers;      // Bit flags: LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI
    uint8_t reserved;
    uint8_t keys[6];        // Up to 6 simultaneous key codes
} usb_hid_keyboard_report_t;

// Mouse boot protocol report
typedef struct __attribute__((packed)) {
    uint8_t buttons;        // Bit 0=Left, 1=Right, 2=Middle
    int8_t  x_movement;    // Relative X movement
    int8_t  y_movement;    // Relative Y movement
    int8_t  wheel;         // Scroll wheel
} usb_hid_mouse_report_t;

// Modifier key bits
#define USB_HID_MOD_LCTRL   (1 << 0)
#define USB_HID_MOD_LSHIFT  (1 << 1)
#define USB_HID_MOD_LALT    (1 << 2)
#define USB_HID_MOD_LGUI    (1 << 3)
#define USB_HID_MOD_RCTRL   (1 << 4)
#define USB_HID_MOD_RSHIFT  (1 << 5)
#define USB_HID_MOD_RALT    (1 << 6)
#define USB_HID_MOD_RGUI    (1 << 7)

// ---- API ----

// Initialize USB HID subsystem (after usb_init)
void usb_hid_init(void);

// Check if USB keyboard is available
int usb_hid_keyboard_available(void);

// Check if USB mouse is available
int usb_hid_mouse_available(void);

// Poll USB keyboard (returns 1 if new data, 0 if no change)
int usb_hid_keyboard_poll(usb_hid_keyboard_report_t* report);

// Poll USB mouse (returns 1 if new data, 0 if no change)
int usb_hid_mouse_poll(usb_hid_mouse_report_t* report);

// Convert HID usage code to ASCII (basic mapping)
char usb_hid_to_ascii(uint8_t keycode, uint8_t modifiers);

#endif
