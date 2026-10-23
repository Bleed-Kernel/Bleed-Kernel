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
#define USB_REQ_RCPT_ENDPOINT   0x02

// standard requests
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_SET_INTERFACE   0x0B

// descriptor types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_SS_COMPANION   0x30

#define USB_FEATURE_ENDPOINT_HALT 0x00

// endpoint descriptor bits
#define USB_ENDPOINT_IN         0x80
#define USB_ENDPOINT_NUM_MASK   0x0F
#define USB_ENDPOINT_TYPE_MASK  0x03
#define USB_ENDPOINT_BULK       0x02
#define USB_ENDPOINT_INTERRUPT  0x03
#define USB_ENDPOINT_MPS_MASK   0x07FF  // the bits above are high speed extra transactions

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

// transfer results, anything else negative is a plain failure
#define USB_ERR                 -1
#define USB_ERR_TIMEOUT         -2
#define USB_ERR_STALL           -3      // endpoint is halted until someone clears it
#define USB_ERR_GONE            -4      // device was unplugged

#define USB_HOTPLUG_POLL_MS     100     // how often usb_service looks at the ports

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

typedef struct __attribute__((packed)) usb_ss_companion_desc {
    uint8_t  length;
    uint8_t  type;
    uint8_t  max_burst;
    uint8_t  attributes;
    uint16_t bytes_per_interval;
} usb_ss_companion_desc_t;

typedef enum usb_speed {
    USB_SPEED_LOW,
    USB_SPEED_FULL,
    USB_SPEED_HIGH,
    USB_SPEED_SUPER,
} usb_speed_t;

struct usb_device;

// called from the timer tick with each report an interrupt endpoint hands back
typedef void (*usb_interrupt_cb_t)(void *ctx, const uint8_t *data, size_t len);

// what a host controller driver hands the class drivers, NULL means it cant do that one.
// bulk buffers have to be HHDM addresses of pmm pages, the controller dmas straight into them
typedef struct usb_hc_ops {
    const char *name;
    int  (*control)(struct usb_device *dev, const usb_setup_t *setup, void *data);
    int  (*interrupt_in)(struct usb_device *dev, const usb_endpoint_desc_t *ep,
                         usb_interrupt_cb_t cb, void *ctx);
    int  (*bulk_open)(struct usb_device *dev, const usb_endpoint_desc_t *ep,
                      const usb_ss_companion_desc_t *ss);
    int  (*bulk)(struct usb_device *dev, uint8_t endpoint, void *buf, size_t len);
    int  (*reset_endpoint)(struct usb_device *dev, uint8_t endpoint);
} usb_hc_ops_t;

// one of these per thing plugged into a root port
typedef struct usb_device {
    const usb_hc_ops_t *ops;
    void       *hc;         // whichever controller struct ops belongs to
    void       *hc_priv;    // per device controller state, xhci keeps its slot here
    uint8_t     address;
    uint8_t     port;
    usb_speed_t speed;
    uint16_t    max_packet0;
    uint16_t    vendor_id;
    uint16_t    product_id;
    volatile bool gone;     // unplugged, everything on it fails straight away from now on
} usb_device_t;

// Find every USB controller we have a driver for and enumerate whats on them
void usb_init(void);

// Timer tick work for every controller and the keyboard repeat
void usb_poll(void);

int usb_get_descriptor(usb_device_t *dev, uint8_t type, void *buf, uint16_t len);
int usb_request(usb_device_t *dev, uint8_t request, uint16_t value);

// Run a control transfer on endpoint 0
int usb_control(usb_device_t *dev, const usb_setup_t *setup, void *data);

// Start polling an interrupt IN endpoint, cb fires off the timer tick for every report
int usb_interrupt_in(usb_device_t *dev, const usb_endpoint_desc_t *ep,
                     usb_interrupt_cb_t cb, void *ctx);

// Get a bulk endpoint ready, ss is the usb3 comp descriptor 
int usb_bulk_open(usb_device_t *dev, const usb_endpoint_desc_t *ep,
                  const usb_ss_companion_desc_t *ss);

// Move len bytes over a bulk endpoint, returns bytes moved
int usb_bulk(usb_device_t *dev, uint8_t endpoint, void *buf, size_t len);

// Unhalt an endpoint on both ends, the controller and the device
int usb_clear_halt(usb_device_t *dev, uint8_t endpoint);

// Read the descriptors, pick the first configuration
bool usb_device_configure(usb_device_t *dev);

// The device got pulled, we also tell everything
void usb_device_disconnect(usb_device_t *dev);

// usb_service, picks up hotplug on every controller
void usb_hotplug_watch(void);

const char *usb_speed_name(usb_speed_t speed);
