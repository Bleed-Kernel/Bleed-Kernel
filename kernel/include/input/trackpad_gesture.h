#pragma once

#include <stdint.h>
#include <stdbool.h>

#define TRACKPAD_POINTER_SPAN   1600    // pixels the pointer moves for a swipe the width of the pad
#define TRACKPAD_SCROLL_NOTCHES 24      // wheel clicks for a two finger swipe the height of it
#define TRACKPAD_TAP_MS         180
#define TRACKPAD_TAP_SLOP_DIV   40      // a tap can wander max_x / this before its a drag
#define TRACKPAD_JUMP_DIV       4       // more than max_x / this in one frame isnt a finger moving

typedef struct trackpad_gesture {
    uint16_t max_x;
    uint16_t max_y;

    // what the last frame looked like
    uint8_t  fingers;
    uint8_t  slot;
    uint16_t last_x;
    uint16_t last_y;
    uint8_t  buttons;

    int32_t  rem_x;
    int32_t  rem_y;
    int32_t  scroll;

    uint64_t tap_start;     // ms
    uint8_t  tap_fingers;   // most that were down at once
    uint32_t tap_travel;
} trackpad_gesture_t;

/// @brief Turn one frame from the pad into mouse events
/// @param fingers how many are down
/// @param slot which one the pointer should follow, x and y are where it is, y counts down the pad
/// @param switches MOUSE_BTN_* for whatever is pressed under the pad
/// @return true if a finger or button is down
bool trackpad_gesture_frame(trackpad_gesture_t *g, uint8_t fingers, uint8_t slot,
                            uint16_t x, uint16_t y, uint8_t switches);

// The pad went away, let go of any buttons
void trackpad_gesture_release(trackpad_gesture_t *g);
