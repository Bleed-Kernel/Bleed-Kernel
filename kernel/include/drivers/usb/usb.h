#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// bmRequestType bits
#define USB_REQ_DIR_OUT         0x00
#define USB_REQ_DIR_IN          0x80
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_RCPT_DEVICE     0x00
#define USB_REQ_RCPT_INTERFACE  0x01

// standard requests
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09

// descriptor types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

// endpoint descriptor bits
#define USB_ENDPOINT_IN         0x80
#define USB_ENDPOINT_NUM_MASK   0x0F
#define USB_ENDPOINT_TYPE_MASK  0x03
#define USB_ENDPOINT_INTERRUPT  0x03

// HID, the boot protocol is the fixed report layout the BIOS uses so
// we never have to parse a report descriptor
#define USB_CLASS_HID           0x03
#define USB_HID_SUBCLASS_BOOT   0x01
#define USB_HID_PROTO_KEYBOARD  0x01
#define USB_HID_PROTO_MOUSE     0x02

#define USB_HID_SET_IDLE        0x0A
#define USB_HID_SET_PROTOCOL    0x0B
#define USB_HID_PROTOCOL_BOOT   0

#define USB_MAX_CONFIG_LEN      512 // biggest config descriptor we bother reading

typedef struct __attribute__((packed)) usb_setup {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usb_setup_t;

typedef struct __attribute__((packed)) usb_device_desc {
    uint8_t  length;
    uint8_t  type;
    uint16_t usb_version;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  max_packet0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t  manufacturer_str;
    uint8_t  product_str;
    uint8_t  serial_str;
    uint8_t  config_count;
} usb_device_desc_t;

typedef struct __attribute__((packed)) usb_config_desc {
    uint8_t  length;
    uint8_t  type;
    uint16_t total_length;
    uint8_t  interface_count;
    uint8_t  config_value;
    uint8_t  config_str;
    uint8_t  attributes;
    uint8_t  max_power;
} usb_config_desc_t;

typedef struct __attribute__((packed)) usb_interface_desc {
    uint8_t  length;
    uint8_t  type;
    uint8_t  interface_number;
    uint8_t  alt_setting;
    uint8_t  endpoint_count;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  interface_str;
} usb_interface_desc_t;

typedef struct __attribute__((packed)) usb_endpoint_desc {
    uint8_t  length;
    uint8_t  type;
    uint8_t  address;
    uint8_t  attributes;
    uint16_t max_packet;
    uint8_t  interval;
} usb_endpoint_desc_t;

struct ohci_hc;

// one of these per thing plugged into a root port
typedef struct usb_device {
    struct ohci_hc *hc;
    uint8_t  address;
    uint8_t  port;
    bool     low_speed;
    uint8_t  max_packet0;
    uint16_t vendor_id;
    uint16_t product_id;
} usb_device_t;

// called from the timer tick with each report an interrupt endpoint hands back
typedef void (*usb_interrupt_cb_t)(void *ctx, const uint8_t *data, size_t len);
