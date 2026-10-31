#include <drivers/virtio/virtio_input.h>
#include <input/mouse_dispatch.h>
#include <input/mouse_input.h>
#include <input/touch_dispatch.h>
#include <input/touch_input.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <stdio.h>

#define virtio_trace(...) do { if (VIRTIO_INPUT_TRACE) serial_printf(LOG_INFO __VA_ARGS__); } while (0)

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static uint16_t virtio_input_scale(int32_t v, const virtio_input_axis_t *axis) {
    if (axis->max <= axis->min) return 0;
    v = clamp(v, axis->min, axis->max);
    return (uint16_t)(((int64_t)(v - axis->min) * TOUCH_MAX) / ((int64_t)axis->max - axis->min));
}

static void virtio_input_key(virtio_input_t *in, uint16_t code, uint32_t value) {
    uint8_t bit = code == BTN_LEFT   ? MOUSE_BTN_LEFT
                : code == BTN_RIGHT  ? MOUSE_BTN_RIGHT
                : code == BTN_MIDDLE ? MOUSE_BTN_MIDDLE : 0;
    if (!bit) return;

    if (value)
        in->buttons |= bit;
    else
        in->buttons &= (uint8_t)~bit;
    in->moved = true;
}

static void virtio_input_abs_event(virtio_input_t *in, uint16_t code, int32_t value) {
    virtio_input_slot_t *slot = &in->slots[in->slot];

    switch (code) {
        case ABS_MT_SLOT:
            // past the slots we keep, park it somewhere it cant do damage
            in->slot = value >= 0 && value < VIRTIO_INPUT_SLOTS ? value : 0;
            break;
        case ABS_MT_TRACKING_ID: slot->id = value; slot->changed = true; break;
        case ABS_MT_POSITION_X:  slot->x  = value; slot->changed = true; break;
        case ABS_MT_POSITION_Y:  slot->y  = value; slot->changed = true; break;
        case ABS_X:              in->x    = value; in->moved     = true; break;
        case ABS_Y:              in->y    = value; in->moved     = true; break;
        default: break;
    }
}

// every finger that changed this report, the slot number is the contact since it
// stays the same for as long as the finger is down
static void virtio_input_fingers(virtio_input_t *in) {
    for (int s = 0; s < VIRTIO_INPUT_SLOTS; s++) {
        virtio_input_slot_t *slot = &in->slots[s];
        if (!slot->changed) continue;
        slot->changed = false;

        bool down = slot->id >= 0;
        if (!down && !slot->down) continue;
        slot->down = down;

        touch_event_t ev = {
            .x       = virtio_input_scale(slot->x, &in->x_axis),
            .y       = virtio_input_scale(slot->y, &in->y_axis),
            .contact = (uint8_t)s,
            .flags   = TOUCH_FINGER | (down ? TOUCH_DOWN : 0),
            .buttons = down ? MOUSE_BTN_LEFT : 0,
        };
        virtio_trace("virtio: -> finger %u %s at %u,%u\n", ev.contact, down ? "down" : "up", ev.x, ev.y);
        touch_input_dispatch(&ev);
    }
}

static void virtio_input_report(virtio_input_t *in) {
    if (in->kind == VIRTIO_INPUT_TOUCH) {
        virtio_input_fingers(in);
        return;
    }
    if (!in->moved) return;

    int8_t wheel = (int8_t)clamp(in->wheel, -127, 127);
    if (in->kind == VIRTIO_INPUT_TABLET) {
        touch_event_t ev = {
            .x       = virtio_input_scale(in->x, &in->x_axis),
            .y       = virtio_input_scale(in->y, &in->y_axis),
            .buttons = in->buttons,
            .wheel   = wheel,
        };
        touch_input_dispatch(&ev);
    } else {
        // evdev counts +y as down the screen like usb does
        mouse_event_t ev = {
            .dx      = (int16_t)clamp(in->dx, -32768, 32767),
            .dy      = (int16_t)clamp(in->dy, -32768, 32767),
            .wheel   = wheel,
            .buttons = in->buttons,
        };
        mouse_input_dispatch(&ev);
    }

    in->dx    = 0;
    in->dy    = 0;
    in->wheel = 0;
    in->moved = false;
}

void virtio_input_event(virtio_input_t *in, const virtio_input_event_t *ev) {
    int32_t value = (int32_t)ev->value;
    virtio_trace("virtio: <- type 0x%02x code 0x%03x value %d\n", ev->type, ev->code, value);

    switch (ev->type) {
        case EV_SYN:
            if (ev->code == SYN_REPORT)
                virtio_input_report(in);
            break;
        case EV_KEY:
            virtio_input_key(in, ev->code, ev->value);
            break;
        case EV_ABS:
            virtio_input_abs_event(in, ev->code, value);
            break;
        case EV_REL:
            if (ev->code == REL_X)     in->dx    += value;
            if (ev->code == REL_Y)     in->dy    += value;
            if (ev->code == REL_WHEEL) in->wheel += value;
            in->moved = true;
            break;
        default:
            break;
    }
}
