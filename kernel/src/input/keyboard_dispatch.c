#include <input/keyboard_dispatch.h>
#include <stdio.h>

static input_listener_t listeners[MAX_INPUT_LISTENERS];
static int listener_count = 0;

int keyboard_register_listener(input_listener_t cb) {
    if (listener_count >= MAX_INPUT_LISTENERS)
        return -1;
    listeners[listener_count++] = cb;
    return 0;
}

void keyboard_input_dispatch(const keyboard_event_t *ev) {
    for (int i = 0; i < listener_count; i++)
        listeners[i](ev);
}
