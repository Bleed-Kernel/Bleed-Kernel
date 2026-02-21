#pragma once

#include <stdint.h>

enum {
    MOUSE_BTN_LEFT   = 1 << 0,
    MOUSE_BTN_RIGHT  = 1 << 1,
    MOUSE_BTN_MIDDLE = 1 << 2,
    MOUSE_BTN_4      = 1 << 3,
    MOUSE_BTN_5      = 1 << 4
};

typedef struct {
    int16_t dx;
    int16_t dy;
    int8_t wheel;
    uint8_t buttons;
} mouse_event_t;
