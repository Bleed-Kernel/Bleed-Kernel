#pragma once
#include <stdint.h>

typedef enum {
    KEY_RELEASE,
    KEY_DOWN
} key_action_t;

enum {
    KEYMOD_SHIFT    = 1 << 0,
    KEYMOD_CTRL     = 1 << 1,
    KEYMOD_ALT      = 1 << 2,
    KEYMOD_CAPS     = 1 << 3
};

typedef struct {
    uint16_t    keycode;
    uint8_t     action;
    uint8_t     keymod;
} keyboard_event_t;