// usb.h - USB Core Layer for Alteo OS
// Common USB definitions, device management, and transfer abstraction
#ifndef USB_H
#define USB_H

#include "stdint.h"

// USB speeds
#define USB_SPEED_LOW       0   // 1.5 Mbps
#define USB_SPEED_FULL      1   // 12 Mbps
#define USB_SPEED_HIGH      2   // 480 Mbps
#define USB_SPEED_SUPER     3   // 5 Gbps

// USB request types (bmRequestType)
#define USB_DIR_OUT         0x00
#define USB_DIR_IN          0x80
#define USB_TYPE_STANDARD   0x00
#define USB_TYPE_CLASS      0x20
#define USB_TYPE_VENDOR     0x40
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02

// Standard USB requests (bRequest)
#define USB_REQ_GET_STATUS      0
#define USB_REQ_CLEAR_FEATURE   1
#define USB_REQ_SET_FEATURE     3
#define USB_REQ_SET_ADDRESS     5
#define USB_REQ_GET_DESCRIPTOR  6
#define USB_REQ_SET_DESCRIPTOR  7
#define USB_REQ_GET_CONFIG      8
#define USB_REQ_SET_CONFIG      9
#define USB_REQ_GET_INTERFACE   10
#define USB_REQ_SET_INTERFACE   11

// Descriptor types
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIGURATION  2
#define USB_DESC_STRING         3
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22

// USB class codes
#define USB_CLASS_HID           0x03
#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09

// HID protocol codes
#define USB_HID_PROTOCOL_KEYBOARD   1
#define USB_HID_PROTOCOL_MOUSE      2

// HID class requests
#define USB_HID_GET_REPORT      0x01
#define USB_HID_SET_REPORT      0x09
#define USB_HID_SET_IDLE        0x0A
#define USB_HID_SET_PROTOCOL    0x0B

// Endpoint direction
#define USB_EP_DIR_OUT          0x00
#define USB_EP_DIR_IN           0x80
#define USB_EP_DIR_MASK         0x80
#define USB_EP_NUM_MASK         0x0F

// Endpoint transfer type
#define USB_EP_TYPE_CONTROL     0x00
#define USB_EP_TYPE_ISOCHRONOUS 0x01
#define USB_EP_TYPE_BULK        0x02
#define USB_EP_TYPE_INTERRUPT   0x03
#define USB_EP_TYPE_MASK        0x03

// Max USB devices
#define USB_MAX_DEVICES         32
#define USB_MAX_ENDPOINTS       16

// ---- Standard USB Descriptors (packed) ----

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

// ---- USB Device Tracking ----

typedef struct {
    int      present;
    int      slot_id;       // xHCI slot ID
    uint8_t  speed;
    uint8_t  address;       // USB address assigned by host controller
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  num_endpoints;
    uint8_t  max_packet_size;
    // Endpoint info
    struct {
        uint8_t  address;   // Endpoint address (includes direction)
        uint8_t  type;      // Transfer type
        uint16_t max_packet;
        uint8_t  interval;
    } endpoints[USB_MAX_ENDPOINTS];
} usb_device_t;

// ---- API ----

// Initialize USB subsystem
void usb_init(void);

// Get a device by index
usb_device_t* usb_get_device(int index);

// Get number of USB devices found
int usb_get_device_count(void);

#endif
