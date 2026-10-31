#include <input/touch_dispatch.h>

static touch_input_listener_t listeners[MAX_TOUCH_INPUT_LISTENERS];
static int listener_count = 0;

int touch_register_listener(touch_input_listener_t cb) {
    if (listener_count >= MAX_TOUCH_INPUT_LISTENERS)
        return -1;
    listeners[listener_count++] = cb;
    return 0;
}

void touch_input_dispatch(const touch_event_t *ev) {
    for (int i = 0; i < listener_count; i++)
        listeners[i](ev);
}
