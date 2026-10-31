#include <input/trackpad_gesture.h>
#include <input/mouse_dispatch.h>
#include <input/mouse_input.h>
#include <ACPI/acpi_hpet.h>

// pad units to pixels, the remainder is kept so slow movement isnt rounded away
static int16_t trackpad_scale(const trackpad_gesture_t *g, int32_t delta, int32_t *rem) {
    int32_t accum  = delta * TRACKPAD_POINTER_SPAN + *rem;
    int32_t scaled = accum / g->max_x;
    *rem = accum % g->max_x;
    return (int16_t)scaled;
}

static void trackpad_send(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons) {
    mouse_event_t ev = {
        .dx      = dx,
        .dy      = dy,
        .wheel   = wheel,
        .buttons = buttons
    };
    mouse_input_dispatch(&ev);
}

// one finger moves, two scroll, a quick touch is a click
bool trackpad_gesture_frame(trackpad_gesture_t *g, uint8_t fingers, uint8_t slot,
                            uint16_t x, uint16_t y, uint8_t switches) {
    if (!g->max_x || !g->max_y) return false;

    // a finger arriving or leaving changes which one that is, dont turn it into a jump
    int32_t dx = 0;
    int32_t dy = 0;
    if (fingers && fingers == g->fingers && slot == g->slot) {
        dx = (int32_t)x - g->last_x;
        dy = (int32_t)y - g->last_y;
    }

    int32_t travel = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (travel > g->max_x / TRACKPAD_JUMP_DIV) {
        dx = 0;
        dy = 0;
    }

    uint64_t now = hpet_get_femtoseconds() / femtosecondsPerMillisecond;
    if (fingers && !g->fingers) {
        g->tap_start   = now;
        g->tap_fingers = 0;
        g->tap_travel  = 0;
    }
    if (fingers) {
        if (fingers > g->tap_fingers) g->tap_fingers = fingers;
        g->tap_travel += (uint32_t)travel;

        // pressing the pad down is a click of its own
        if (switches)
            g->tap_travel = UINT32_MAX / 2;
    }
    bool tapped = !fingers && g->fingers &&
                  now - g->tap_start <= TRACKPAD_TAP_MS &&
                  g->tap_travel <= (uint32_t)g->max_x / TRACKPAD_TAP_SLOP_DIV;

    // theres one switch under the whole pad, two fingers on it makes it the right button
    uint8_t buttons = switches & (MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE);
    if (switches & MOUSE_BTN_LEFT)
        buttons |= fingers >= 2 ? MOUSE_BTN_RIGHT : MOUSE_BTN_LEFT;

    int16_t move_x = 0;
    int16_t move_y = 0;
    int8_t  wheel  = 0;

    if (fingers == 1) {
        move_x = trackpad_scale(g, dx, &g->rem_x);
        move_y = trackpad_scale(g, dy, &g->rem_y);
    } else if (fingers == 2) {
        // fingers going up the pad is the wheel rolling away from you
        int32_t notch = g->max_y / TRACKPAD_SCROLL_NOTCHES;
        if (notch < 1) notch = 1;

        g->scroll -= dy;
        wheel      = (int8_t)(g->scroll / notch);
        g->scroll -= wheel * notch;
    }
    if (fingers != 2)
        g->scroll = 0;

    if (move_x || move_y || wheel || buttons != g->buttons)
        trackpad_send(move_x, move_y, wheel, buttons);

    if (tapped) {
        uint8_t tap = g->tap_fingers >= 2 ? MOUSE_BTN_RIGHT : MOUSE_BTN_LEFT;
        trackpad_send(0, 0, 0, buttons | tap);
        trackpad_send(0, 0, 0, buttons);
    }

    g->fingers = fingers;
    g->slot    = slot;
    g->last_x  = x;
    g->last_y  = y;
    g->buttons = buttons;

    return fingers || buttons || tapped;
}

void trackpad_gesture_release(trackpad_gesture_t *g) {
    if (g->buttons)
        trackpad_send(0, 0, 0, 0);
    g->buttons = 0;
    g->fingers = 0;
}
