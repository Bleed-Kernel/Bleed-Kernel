#include <input/mouse_dispatch.h>

static mouse_input_listener_t listeners[MAX_MOUSE_INPUT_LISTENERS];
static int listener_count = 0;

int mouse_register_listener(mouse_input_listener_t cb) {
    if (listener_count >= MAX_MOUSE_INPUT_LISTENERS)
        return -1;
    listeners[listener_count++] = cb;
    return 0;
}

void mouse_input_dispatch(const mouse_event_t *ev) {
    for (int i = 0; i < listener_count; i++)
        listeners[i](ev);
}
