#pragma once

#include <stdint.h>

#define TOUCH_MAX 0xFFFF    // x and y run 0 to this across the whole screen

enum {
    TOUCH_FINGER = 1 << 0,  // off a touchscreen, without it its a tablet or an absolute mouse
    TOUCH_DOWN   = 1 << 1   // the finger is on the glass
};

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  contact;       // which finger, stays the same while its down
    uint8_t  flags;
    uint8_t  buttons;       // MOUSE_BTN_*, a finger is the left button while its down
    int8_t   wheel;
} touch_event_t;
