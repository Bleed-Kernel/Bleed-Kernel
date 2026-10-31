#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <drivers/virtio/virtio_pci.h>
#include <drivers/virtio/virtio_queue.h>

#define VIRTIO_INPUT_TRACE          0       // 1 logs every event in and every mouse/touch event out
#define VIRTIO_ID_INPUT             18
#define VIRTIO_INPUT_MAX            4
#define VIRTIO_INPUT_SLOTS          10      // fingers we follow on a multitouch one

// the class qemu gives them, input device / other
#define PCI_CLASS_INPUT             0x09
#define PCI_SUBCLASS_INPUT_OTHER    0x80

// device config, select a question then read the answer out of the union
#define VIRTIO_INPUT_CFG_SELECT     0
#define VIRTIO_INPUT_CFG_SUBSEL     1
#define VIRTIO_INPUT_CFG_SIZE       2
#define VIRTIO_INPUT_CFG_DATA       8
#define VIRTIO_INPUT_CFG_DATA_MAX   128
#define VIRTIO_INPUT_CFG_ID_NAME    0x01
#define VIRTIO_INPUT_CFG_EV_BITS    0x11
#define VIRTIO_INPUT_CFG_ABS_INFO   0x12

// what the events are, theyre linux evdev codes
#define EV_SYN                  0x00
#define EV_KEY                  0x01
#define EV_REL                  0x02
#define EV_ABS                  0x03
#define SYN_REPORT              0
#define REL_X                   0x00
#define REL_Y                   0x01
#define REL_WHEEL               0x08
#define ABS_X                   0x00
#define ABS_Y                   0x01
#define ABS_MT_SLOT             0x2F
#define ABS_MT_POSITION_X       0x35
#define ABS_MT_POSITION_Y       0x36
#define ABS_MT_TRACKING_ID      0x39
#define BTN_LEFT                0x110
#define BTN_RIGHT               0x111
#define BTN_MIDDLE              0x112

typedef enum {
    VIRTIO_INPUT_OTHER,
    VIRTIO_INPUT_MOUSE,
    VIRTIO_INPUT_TABLET,
    VIRTIO_INPUT_TOUCH,
} virtio_input_kind_t;

typedef struct __attribute__((packed)) virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} virtio_input_event_t;

typedef struct virtio_input_axis {
    int32_t min;
    int32_t max;
} virtio_input_axis_t;

typedef struct virtio_input_slot {
    int32_t id;         // tracking id, -1 = no finger
    int32_t x;
    int32_t y;
    bool    changed;
    bool    down;       // what we last told everyone
} virtio_input_slot_t;

typedef struct virtio_input {
    bool                 used;
    virtio_pci_t         vp;
    virtq_t              eventq;
    virtio_input_event_t *events;   // one per queue entry, the device writes into them
    paddr_t              events_phys;
    char                 name[64];
    virtio_input_kind_t  kind;
    virtio_input_axis_t  x_axis;
    virtio_input_axis_t  y_axis;

    // built up until the device says SYN_REPORT
    int                  slot;
    virtio_input_slot_t  slots[VIRTIO_INPUT_SLOTS];
    int32_t              x;
    int32_t              y;
    int32_t              dx;
    int32_t              dy;
    int32_t              wheel;
    uint8_t              buttons;
    bool                 moved;
} virtio_input_t;

// Find every virtio input device and start the mice, tablets and touchscreens
void virtio_input_init(void);

// Timer tick work, hands on whatever the devices sent since last time
void virtio_input_poll(void);

// One event off the queue, turned into mouse or touch events when a report finishes
void virtio_input_event(virtio_input_t *in, const virtio_input_event_t *ev);
