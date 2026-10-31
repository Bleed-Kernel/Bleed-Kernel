#include <drivers/hid/hid.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

enum { APP_NONE, APP_MOUSE, APP_TOUCH, APP_TOUCHPAD, APP_CONFIG };

#define FINGER_NONE     -1
#define FINGER_SKIP     -2      // more fingers than we keep slots for

typedef struct hid_globals {
    uint16_t page;
    int32_t  logical_min;
    int32_t  logical_max;
    uint32_t logical_max_raw;   // some devices write 0xFFFF and mean it unsigned
    uint32_t size;
    uint32_t count;
    uint8_t  id;
} hid_globals_t;

typedef struct hid_parser {
    hid_input_t  *hid;
    hid_globals_t g;
    hid_globals_t stack[HID_GLOBAL_STACK];
    int           stack_depth;

    uint32_t usages[HID_MAX_USAGES];
    uint32_t usage_count;
    uint32_t usage_min;
    uint32_t usage_max;

    int  depth;
    int  app;
    int  app_depth;
    int  finger;
    int  finger_depth;
    bool touch_seen;
    bool touchpad_seen;

    uint16_t bits[256];     // how far into each report id the fields have got
    uint16_t feature_bits[256];
} hid_parser_t;

// only ever one parse at a time, probing is boot or usb_service
static hid_parser_t parser;

static int32_t hid_signed(uint32_t data, uint8_t n) {
    if (n == 1) return (int8_t)data;
    if (n == 2) return (int16_t)data;
    return (int32_t)data;
}

static void hid_take_mouse(hid_parser_t *p, uint32_t usage, bool relative, hid_axis_t axis) {
    hid_input_t *hid   = p->hid;
    hid_field_t  field = axis.field;

    if ((usage >> 16) == HID_PAGE_BUTTON) {
        // buttons 1-5 are consecutive bits in the same order mouse_event_t keeps them
        uint16_t n = (uint16_t)usage;
        if (field.size != 1 || n < 1 || n > 5) return;

        if (n == 1 && !hid->buttons.size)
            hid->buttons = field;
        else if (n == hid->buttons.size + 1 && field.bit == hid->buttons.bit + hid->buttons.size)
            hid->buttons.size++;
        return;
    }

    if (usage == HID_USAGE_X && !hid->x.field.size) {
        hid->x              = axis;
        hid->mouse_id       = p->g.id;
        hid->mouse_absolute = !relative;
    } else if (usage == HID_USAGE_Y && !hid->y.field.size && p->g.id == hid->mouse_id) {
        hid->y = axis;
    } else if (usage == HID_USAGE_WHEEL && relative && !hid->wheel.size && p->g.id == hid->mouse_id) {
        hid->wheel = field;
    }
}

static void hid_take_touch(hid_parser_t *p, uint32_t usage, bool relative, hid_axis_t axis) {
    hid_input_t *hid = p->hid;
    if (p->finger == FINGER_SKIP) return;
    if (p->touch_seen && p->g.id != hid->touch_id) return;

    // a single touch screen can skip the finger collection, those fields are finger 0
    // the switch under a touchpad, same deal as the mouse buttons
    if ((usage >> 16) == HID_PAGE_BUTTON) {
        uint16_t n = (uint16_t)usage;
        if (p->app != APP_TOUCHPAD || axis.field.size != 1 || n < 1 || n > 3) return;

        if (n == 1 && !hid->pad_buttons.size)
            hid->pad_buttons = axis.field;
        else if (n == hid->pad_buttons.size + 1 && axis.field.bit == hid->pad_buttons.bit + hid->pad_buttons.size)
            hid->pad_buttons.size++;
        return;
    }

    hid_finger_t *finger = &hid->fingers[p->finger >= 0 ? p->finger : 0];
    hid_field_t  *slot   = NULL;
    hid_axis_t   *range  = NULL;

    if (usage == HID_USAGE_CONTACT_COUNT)   slot  = &hid->contact_count;
    else if (usage == HID_USAGE_TIP_SWITCH) slot  = &finger->tip;
    else if (usage == HID_USAGE_CONFIDENCE) slot  = &finger->confidence;
    else if (usage == HID_USAGE_CONTACT_ID) slot  = &finger->contact_id;
    else if (usage == HID_USAGE_X)          range = &finger->x;
    else if (usage == HID_USAGE_Y)          range = &finger->y;

    if (range) {
        if (relative || range->field.size) return;
        *range = axis;
    } else {
        if (!slot || slot->size) return;
        *slot = axis.field;
    }

    p->touch_seen    = true;
    p->touchpad_seen = p->app == APP_TOUCHPAD;
    hid->touch_id    = p->g.id;
}

static void hid_collection(hid_parser_t *p, uint32_t type) {
    uint32_t usage = p->usage_count ? p->usages[0] : 0;
    p->depth++;

    if (type == HID_COLLECTION_APP && p->app == APP_NONE) {
        if (usage == HID_USAGE_MOUSE || usage == HID_USAGE_POINTER)
            p->app = APP_MOUSE;
        else if (usage == HID_USAGE_TOUCH_SCREEN)
            p->app = APP_TOUCH;
        else if (usage == HID_USAGE_TOUCH_PAD)
            p->app = APP_TOUCHPAD;
        else if (usage == HID_USAGE_DEVICE_CONFIG)
            p->app = APP_CONFIG;
        if (p->app != APP_NONE)
            p->app_depth = p->depth;
        return;
    }

    // each finger gets a collection of its own, logical or physical depending who wrote it
    if ((p->app == APP_TOUCH || p->app == APP_TOUCHPAD) && usage == HID_USAGE_FINGER && p->finger == FINGER_NONE) {
        p->finger       = p->hid->finger_count < HID_MAX_FINGERS ? p->hid->finger_count++ : FINGER_SKIP;
        p->finger_depth = p->depth;
    }
}

static void hid_end_collection(hid_parser_t *p) {
    if (p->depth == p->finger_depth) {
        p->finger       = FINGER_NONE;
        p->finger_depth = 0;
    }
    if (p->depth == p->app_depth) {
        p->app       = APP_NONE;
        p->app_depth = 0;
    }
    if (p->depth) p->depth--;
}

// short of usages the last one covers every field thats left
static uint32_t hid_usage_at(const hid_parser_t *p, uint32_t i) {
    if (p->usage_count)
        return p->usages[i < p->usage_count ? i : p->usage_count - 1];
    return p->usage_min + i < p->usage_max ? p->usage_min + i : p->usage_max;
}

static void hid_input_item(hid_parser_t *p, uint32_t flags) {
    hid_globals_t *g = &p->g;
    if (g->size > 32 || g->count > 256) return;

    bool wanted = p->app != APP_NONE && p->app != APP_CONFIG &&
                  !(flags & HID_INPUT_CONSTANT) && (flags & HID_INPUT_VARIABLE);
    for (uint32_t i = 0; wanted && i < g->count; i++) {
        uint32_t usage = hid_usage_at(p, i);

        hid_axis_t axis = {
            .field = { .bit = (uint16_t)(p->bits[g->id] + i * g->size), .size = (uint8_t)g->size },
            .min   = g->logical_min,
            .max   = g->logical_max < g->logical_min ? (int32_t)g->logical_max_raw : g->logical_max,
        };

        if (p->app == APP_MOUSE)
            hid_take_mouse(p, usage, flags & HID_INPUT_RELATIVE, axis);
        else
            hid_take_touch(p, usage, flags & HID_INPUT_RELATIVE, axis);
    }
    p->bits[g->id] += (uint16_t)(g->size * g->count);
}

// only the input mode is wanted, the rest just have to be counted past
static void hid_feature_item(hid_parser_t *p, uint32_t flags) {
    hid_globals_t *g   = &p->g;
    hid_input_t   *hid = p->hid;
    if (g->size > 32 || g->count > 256) return;

    bool wanted = p->app == APP_CONFIG && !(flags & HID_INPUT_CONSTANT) && (flags & HID_INPUT_VARIABLE);
    for (uint32_t i = 0; wanted && i < g->count; i++) {
        if (hid_usage_at(p, i) != HID_USAGE_INPUT_MODE || hid->input_mode.size) continue;

        hid->input_mode    = (hid_field_t){ .bit = (uint16_t)(p->feature_bits[g->id] + i * g->size),
                                            .size = (uint8_t)g->size };
        hid->input_mode_id = g->id;
    }
    p->feature_bits[g->id] += (uint16_t)(g->size * g->count);
}

bool hid_parse(hid_input_t *hid, const uint8_t *rd, size_t len) {
    hid_parser_t *p = &parser;
    memset(hid, 0, sizeof(*hid));
    memset(p, 0, sizeof(*p));
    p->hid    = hid;
    p->finger = FINGER_NONE;

    for (size_t off = 0; off < len; ) {
        uint8_t prefix = rd[off++];

        if (prefix == HID_ITEM_LONG) {
            if (off >= len) break;
            off += (size_t)rd[off] + 2;
            continue;
        }

        // a size of 3 means 4 bytes
        uint8_t n = (prefix & 3) == 3 ? 4 : (prefix & 3);
        if (off + n > len) break;

        uint32_t data = 0;
        for (uint8_t i = 0; i < n; i++)
            data |= (uint32_t)rd[off + i] << (i * 8);
        off += n;

        // a 4 byte usage brings its own page
        uint32_t usage = n == 4 ? data : HID_USAGE(p->g.page, data);

        switch (prefix & HID_ITEM_TAG_MASK) {
            case HID_ITEM_USAGE_PAGE:   p->g.page  = (uint16_t)data; break;
            case HID_ITEM_REPORT_SIZE:  p->g.size  = data;           break;
            case HID_ITEM_REPORT_COUNT: p->g.count = data;           break;
            case HID_ITEM_USAGE_MIN:    p->usage_min = usage;        break;
            case HID_ITEM_USAGE_MAX:    p->usage_max = usage;        break;
            case HID_ITEM_LOGICAL_MIN:  p->g.logical_min = hid_signed(data, n); break;

            case HID_ITEM_LOGICAL_MAX:
                p->g.logical_max     = hid_signed(data, n);
                p->g.logical_max_raw = data;
                break;

            case HID_ITEM_REPORT_ID:
                p->g.id = (uint8_t)data;
                hid->has_report_ids = true;
                break;

            case HID_ITEM_PUSH:
                if (p->stack_depth < HID_GLOBAL_STACK)
                    p->stack[p->stack_depth++] = p->g;
                break;

            case HID_ITEM_POP:
                if (p->stack_depth)
                    p->g = p->stack[--p->stack_depth];
                break;

            case HID_ITEM_USAGE:
                if (p->usage_count < HID_MAX_USAGES)
                    p->usages[p->usage_count++] = usage;
                break;

            case HID_ITEM_COLLECTION:     hid_collection(p, data); break;
            case HID_ITEM_END_COLLECTION: hid_end_collection(p);   break;
            case HID_ITEM_INPUT:          hid_input_item(p, data);   break;
            case HID_ITEM_FEATURE:        hid_feature_item(p, data); break;

            default:
                break;
        }

        // usages only last until the next main item
        if ((prefix & HID_ITEM_TYPE_MASK) == 0) {
            p->usage_count = 0;
            p->usage_min   = 0;
            p->usage_max   = 0;
        }
    }

    if (!hid->finger_count && hid->fingers[0].x.field.size)
        hid->finger_count = 1;

    bool fingers = hid->finger_count && hid->fingers[0].x.field.size && hid->fingers[0].y.field.size;
    if (hid->input_mode.size)
        hid->input_mode_len = (uint8_t)((p->feature_bits[hid->input_mode_id] + 7) / 8);

    // a touchpad nobody can switch over stays a mouse
    hid->has_mouse    = hid->x.field.size && hid->y.field.size;
    hid->has_touch    = fingers && !p->touchpad_seen;
    hid->has_touchpad = fingers && p->touchpad_seen && hid->input_mode.size &&
                        p->feature_bits[hid->input_mode_id] <= HID_MAX_FEATURE * 8;
    if (hid->has_touchpad) {
        const hid_finger_t *f = &hid->fingers[0];
        hid->pad.max_x = (uint16_t)(f->x.max - f->x.min > 0xFFFF ? 0xFFFF : f->x.max - f->x.min);
        hid->pad.max_y = (uint16_t)(f->y.max - f->y.min > 0xFFFF ? 0xFFFF : f->y.max - f->y.min);
    }
    return hid->has_mouse || hid->has_touch || hid->has_touchpad;
}

void hid_describe(const hid_input_t *hid, const char *who) {
    if (hid->has_touch) {
        const hid_finger_t *f = &hid->fingers[0];
        serial_printf(LOG_INFO "%s: touchscreen, report id %u, %u finger slot(s), x %d-%d y %d-%d%s%s\n",
                      who, hid->touch_id, hid->finger_count, f->x.min, f->x.max, f->y.min, f->y.max,
                      hid->contact_count.size ? ", contact count" : "",
                      f->contact_id.size ? ", contact ids" : "");
    }
    if (hid->has_touchpad) {
        const hid_finger_t *f = &hid->fingers[0];
        serial_printf(LOG_INFO "%s: touchpad, report id %u, %u finger slot(s), x %d-%d y %d-%d, "
                      "%u button(s), input mode in feature %u\n", who, hid->touch_id, hid->finger_count,
                      f->x.min, f->x.max, f->y.min, f->y.max, hid->pad_buttons.size, hid->input_mode_id);
    }
    if (hid->has_mouse) {
        if (hid->mouse_absolute)
            serial_printf(LOG_INFO "%s: absolute pointer, report id %u, %u button(s), x %d-%d y %d-%d\n",
                          who, hid->mouse_id, hid->buttons.size, hid->x.min, hid->x.max, hid->y.min, hid->y.max);
        else
            serial_printf(LOG_INFO "%s: mouse, report id %u, %u button(s)%s\n",
                          who, hid->mouse_id, hid->buttons.size, hid->wheel.size ? ", wheel" : "");
    }
    if (!hid->has_touch && !hid->has_touchpad && !hid->has_mouse)
        serial_printf(LOG_INFO "%s: no mouse or touchscreen in the report descriptor\n", who);
}
