#include <drivers/hid/hid.h>
#include <input/mouse_dispatch.h>
#include <input/mouse_input.h>
#include <input/touch_dispatch.h>
#include <input/touch_input.h>
#include <string.h>

static int32_t hid_field_get(const uint8_t *report, size_t len,
                             const hid_field_t *field, bool is_signed) {
    uint32_t val = 0;
    for (uint8_t i = 0; i < field->size; i++) {
        size_t bit = (size_t)field->bit + i;
        if (bit / 8 >= len) break;
        val |= (uint32_t)((report[bit / 8] >> (bit % 8)) & 1) << i;
    }

    if (is_signed && field->size && field->size < 32 && (val & (1u << (field->size - 1))))
        val |= ~0u << field->size;
    return (int32_t)val;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// stretch whatever range the device uses over 0 to TOUCH_MAX
static uint16_t hid_axis_scale(const uint8_t *report, size_t len, const hid_axis_t *axis) {
    if (axis->max <= axis->min) return 0;

    int32_t v = clamp(hid_field_get(report, len, &axis->field, axis->min < 0), axis->min, axis->max);
    return (uint16_t)(((int64_t)(v - axis->min) * TOUCH_MAX) / ((int64_t)axis->max - axis->min));
}

// where on the pad, in its own units from 0
static uint16_t hid_axis_raw(const uint8_t *report, size_t len, const hid_axis_t *axis) {
    if (axis->max <= axis->min) return 0;

    int32_t v = clamp(hid_field_get(report, len, &axis->field, axis->min < 0), axis->min, axis->max);
    return (uint16_t)(v - axis->min > 0xFFFF ? 0xFFFF : v - axis->min);
}

static void hid_mouse_report(hid_input_t *hid, const uint8_t *report, size_t len) {
    uint8_t buttons = (uint8_t)hid_field_get(report, len, &hid->buttons, false);
    int32_t wheel   = clamp(hid_field_get(report, len, &hid->wheel, true), -127, 127);

    if (hid->mouse_absolute) {
        touch_event_t ev = {
            .x       = hid_axis_scale(report, len, &hid->x),
            .y       = hid_axis_scale(report, len, &hid->y),
            .buttons = buttons,
            .wheel   = (int8_t)wheel,
        };
        hid->pointer_x       = ev.x;
        hid->pointer_y       = ev.y;
        hid->pointer_buttons = buttons;
        touch_input_dispatch(&ev);
        return;
    }

    // hid counts +y as down the screen same as usb, no flip here either
    mouse_event_t ev = {
        .dx      = (int16_t)clamp(hid_field_get(report, len, &hid->x.field, true), -32768, 32767),
        .dy      = (int16_t)clamp(hid_field_get(report, len, &hid->y.field, true), -32768, 32767),
        .wheel   = (int8_t)wheel,
        .buttons = buttons,
    };
    mouse_input_dispatch(&ev);
}

static int hid_contact_find(const hid_input_t *hid, uint8_t id) {
    for (int i = 0; i < hid->contact_total; i++) {
        if (hid->contacts[i].id == id)
            return i;
    }
    return -1;
}

static void hid_contact_send(const hid_contact_t *c, bool down) {
    touch_event_t ev = {
        .x       = c->x,
        .y       = c->y,
        .contact = c->id,
        .flags   = TOUCH_FINGER | (down ? TOUCH_DOWN : 0),
        .buttons = down ? MOUSE_BTN_LEFT : 0,
    };
    touch_input_dispatch(&ev);
}

// up where it was last seen, then the last one fills the gap
static void hid_contact_lift(hid_input_t *hid, int i) {
    hid_contact_send(&hid->contacts[i], false);
    hid->contacts[i] = hid->contacts[--hid->contact_total];
}

static void hid_touch_report(hid_input_t *hid, const uint8_t *report, size_t len) {
    uint32_t slots = hid->finger_count;

    // hybrid mode, the first report of a frame says how many fingers are coming and the rest say 0
    if (hid->contact_count.size) {
        uint32_t count = (uint32_t)hid_field_get(report, len, &hid->contact_count, false);
        if (count)
            hid->expected = count > 255 ? 255 : (uint8_t)count;
        if (slots > hid->expected)
            slots = hid->expected;
        hid->expected -= (uint8_t)slots;
    }

    for (uint32_t f = 0; f < slots; f++) {
        const hid_finger_t *finger = &hid->fingers[f];

        bool down = !finger->tip.size || hid_field_get(report, len, &finger->tip, false);
        if (finger->confidence.size && !hid_field_get(report, len, &finger->confidence, false))
            down = false;

        uint8_t id = finger->contact_id.size
                   ? (uint8_t)hid_field_get(report, len, &finger->contact_id, false) : (uint8_t)f;
        int     i  = hid_contact_find(hid, id);

        if (!down) {
            if (i >= 0)
                hid_contact_lift(hid, i);
            continue;
        }

        if (i < 0) {
            if (hid->contact_total >= HID_MAX_FINGERS) continue;
            i = hid->contact_total++;
            hid->contacts[i].id = id;
        }

        hid_contact_t *c = &hid->contacts[i];
        c->seen = true;
        c->x    = hid_axis_scale(report, len, &finger->x);
        c->y    = hid_axis_scale(report, len, &finger->y);
        hid_contact_send(c, true);
    }

    // end of the frame, a finger that wasnt mentioned is gone and nobody said
    if (hid->expected) return;
    for (int i = hid->contact_total - 1; i >= 0; i--) {
        if (!hid->contacts[i].seen)
            hid_contact_lift(hid, i);
        else
            hid->contacts[i].seen = false;
    }
}

// frames can be spread over reports like a touchscreens, the gesture only gets whole ones
static void hid_touchpad_report(hid_input_t *hid, const uint8_t *report, size_t len) {
    uint32_t slots = hid->finger_count;

    if (!hid->expected) {
        hid->pad_fingers  = 0;
        hid->pad_switches = 0;
    }
    if (hid->contact_count.size) {
        uint32_t count = (uint32_t)hid_field_get(report, len, &hid->contact_count, false);
        if (count)
            hid->expected = count > 255 ? 255 : (uint8_t)count;
        if (slots > hid->expected)
            slots = hid->expected;
        hid->expected -= (uint8_t)slots;
    }

    for (uint32_t f = 0; f < slots; f++) {
        const hid_finger_t *finger = &hid->fingers[f];

        bool down = !finger->tip.size || hid_field_get(report, len, &finger->tip, false);
        if (finger->confidence.size && !hid_field_get(report, len, &finger->confidence, false))
            down = false;
        if (!down || hid->pad_fingers++) continue;

        hid->pad_slot = finger->contact_id.size
                      ? (uint8_t)hid_field_get(report, len, &finger->contact_id, false) : (uint8_t)f;
        hid->pad_x    = hid_axis_raw(report, len, &finger->x);
        hid->pad_y    = hid_axis_raw(report, len, &finger->y);
    }

    // buttons 1 2 3 are left right middle, the same bits mouse_event_t uses
    hid->pad_switches |= (uint8_t)hid_field_get(report, len, &hid->pad_buttons, false);

    if (hid->expected) return;
    trackpad_gesture_frame(&hid->pad, hid->pad_fingers, hid->pad_slot,
                           hid->pad_x, hid->pad_y, hid->pad_switches);
}

size_t hid_touchpad_mode(const hid_input_t *hid, uint8_t *buf, size_t max) {
    size_t len = hid->input_mode_len;
    if (!hid->input_mode.size || !len || len > max) return 0;

    memset(buf, 0, len);
    for (uint8_t i = 0; i < hid->input_mode.size && i < 8; i++) {
        size_t bit = (size_t)hid->input_mode.bit + i;
        if ((HID_INPUT_MODE_TOUCHPAD >> i) & 1)
            buf[bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
    return len;
}

bool hid_report(hid_input_t *hid, const uint8_t *report, size_t len) {
    uint8_t id = 0;
    if (hid->has_report_ids) {
        if (!len) return false;
        id = report[0];
        report++;
        len--;
    }

    // touchscreens often carry a mouse too for whoever cant do touch, thats not us
    if (hid->has_touch) {
        if (id != hid->touch_id) return false;
        hid_touch_report(hid, report, len);
        return true;
    }
    // until its switched over it carries on as the mouse
    if (hid->has_touchpad && id == hid->touch_id) {
        hid_touchpad_report(hid, report, len);
        return true;
    }
    if (hid->has_mouse && id == hid->mouse_id) {
        hid_mouse_report(hid, report, len);
        return true;
    }
    return false;
}

void hid_release(hid_input_t *hid) {
    if (hid->has_touchpad)
        trackpad_gesture_release(&hid->pad);

    while (hid->contact_total)
        hid_contact_lift(hid, hid->contact_total - 1);

    if (hid->mouse_absolute && hid->pointer_buttons) {
        touch_event_t ev = { .x = hid->pointer_x, .y = hid->pointer_y };
        hid->pointer_buttons = 0;
        touch_input_dispatch(&ev);
    }
}
