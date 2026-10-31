#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <input/trackpad_gesture.h>

#define HID_MAX_FINGERS         10      // finger slots one report can carry that we keep track of
#define HID_MAX_USAGES          16
#define HID_MAX_REPORT_DESC     4096    // biggest report descriptor we bother reading
#define HID_GLOBAL_STACK        4

#define HID_ITEM_TAG_MASK       0xFC
#define HID_ITEM_TYPE_MASK      0x0C    // 0 = main
#define HID_ITEM_LONG           0xFE

#define HID_ITEM_INPUT          0x80
#define HID_ITEM_FEATURE        0xB0
#define HID_ITEM_COLLECTION     0xA0
#define HID_ITEM_END_COLLECTION 0xC0
#define HID_ITEM_USAGE_PAGE     0x04
#define HID_ITEM_LOGICAL_MIN    0x14
#define HID_ITEM_LOGICAL_MAX    0x24
#define HID_ITEM_REPORT_SIZE    0x74
#define HID_ITEM_REPORT_ID      0x84
#define HID_ITEM_REPORT_COUNT   0x94
#define HID_ITEM_PUSH           0xA4
#define HID_ITEM_POP            0xB4
#define HID_ITEM_USAGE          0x08
#define HID_ITEM_USAGE_MIN      0x18
#define HID_ITEM_USAGE_MAX      0x28

#define HID_INPUT_CONSTANT      (1u << 0)   // padding
#define HID_INPUT_VARIABLE      (1u << 1)   // without it the field is an array, keyboards mostly
#define HID_INPUT_RELATIVE      (1u << 2)

#define HID_COLLECTION_APP      0x01

#define HID_PAGE_DESKTOP        0x01
#define HID_PAGE_BUTTON         0x09
#define HID_PAGE_DIGITIZER      0x0D

// usages carry their page in the top half so a stray page cant match
#define HID_USAGE(page, id)     (((uint32_t)(page) << 16) | (id))
#define HID_USAGE_POINTER       HID_USAGE(HID_PAGE_DESKTOP, 0x01)
#define HID_USAGE_MOUSE         HID_USAGE(HID_PAGE_DESKTOP, 0x02)
#define HID_USAGE_X             HID_USAGE(HID_PAGE_DESKTOP, 0x30)
#define HID_USAGE_Y             HID_USAGE(HID_PAGE_DESKTOP, 0x31)
#define HID_USAGE_WHEEL         HID_USAGE(HID_PAGE_DESKTOP, 0x38)
#define HID_USAGE_TOUCH_SCREEN  HID_USAGE(HID_PAGE_DIGITIZER, 0x04)
#define HID_USAGE_TOUCH_PAD     HID_USAGE(HID_PAGE_DIGITIZER, 0x05)
#define HID_USAGE_DEVICE_CONFIG HID_USAGE(HID_PAGE_DIGITIZER, 0x0E)
#define HID_USAGE_FINGER        HID_USAGE(HID_PAGE_DIGITIZER, 0x22)
#define HID_USAGE_TIP_SWITCH    HID_USAGE(HID_PAGE_DIGITIZER, 0x42)
#define HID_USAGE_CONFIDENCE    HID_USAGE(HID_PAGE_DIGITIZER, 0x47)
#define HID_USAGE_CONTACT_ID    HID_USAGE(HID_PAGE_DIGITIZER, 0x51)
#define HID_USAGE_CONTACT_COUNT HID_USAGE(HID_PAGE_DIGITIZER, 0x54)
#define HID_USAGE_INPUT_MODE    HID_USAGE(HID_PAGE_DIGITIZER, 0x52)

#define HID_INPUT_MODE_TOUCHPAD 3
#define HID_MAX_FEATURE         16      // biggest input mode feature report we build

typedef struct hid_field {
    uint16_t bit;       // the id byte not counted
    uint8_t  size;      // in bits
} hid_field_t;

// absolute ones need their range to get scaled to the screen 
typedef struct hid_axis {
    hid_field_t field;
    int32_t     min;
    int32_t     max;
} hid_axis_t;

typedef struct hid_finger {
    hid_field_t tip;
    hid_field_t confidence;     // 0 = the device thinks its a palm
    hid_field_t contact_id;
    hid_axis_t  x;
    hid_axis_t  y;
} hid_finger_t;

// a finger thats down, kept so it can be lifted if the device forgets to say
typedef struct hid_contact {
    uint8_t  id;
    bool     seen;
    uint16_t x;
    uint16_t y;
} hid_contact_t;

typedef struct hid_input {
    bool has_report_ids;

    // a mouse, or a tablet when x and y are absolute
    bool        has_mouse;
    bool        mouse_absolute;
    uint8_t     mouse_id;
    hid_field_t buttons;
    hid_axis_t  x;
    hid_axis_t  y;
    hid_field_t wheel;

    // a touchscreen, one slot per finger a report has room for
    bool          has_touch;
    uint8_t       touch_id;
    uint8_t       finger_count;
    hid_field_t   contact_count;
    hid_finger_t  fingers[HID_MAX_FINGERS];

    // a precision touchpad
    bool               has_touchpad;
    hid_field_t        pad_buttons;
    uint8_t            input_mode_id;
    hid_field_t        input_mode;
    uint8_t            input_mode_len;
    trackpad_gesture_t pad;
    uint8_t            pad_fingers;     // what the frame so far added up to
    uint8_t            pad_slot;
    uint16_t           pad_x;
    uint16_t           pad_y;
    uint8_t            pad_switches;

    // what the reports so far added up to
    uint8_t       expected;     // hybrid mode spreads them out
    uint8_t       contact_total;
    hid_contact_t contacts[HID_MAX_FINGERS];
    uint16_t      pointer_x;
    uint16_t      pointer_y;
    uint8_t       pointer_buttons;
} hid_input_t;

/// @brief Work out where the mouse or touchscreen fields sit in the reports
/// @param hid filled in, wiped first
/// @param rd the report descriptor
/// @param len length of rd
/// @return true if theres a mouse or a touchscreen in there
bool hid_parse(hid_input_t *hid, const uint8_t *rd, size_t len);

// Log what hid_parse found, who is the driver name the line starts with
void hid_describe(const hid_input_t *hid, const char *who);

// Fill in the feature report that switches a touchpad to sending fingers
size_t hid_touchpad_mode(const hid_input_t *hid, uint8_t *buf, size_t max);

// Turn one input report into mouse or touch events, report starts at the id byte
bool hid_report(hid_input_t *hid, const uint8_t *report, size_t len);
void hid_release(hid_input_t *hid);
