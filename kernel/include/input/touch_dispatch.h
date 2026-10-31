#pragma once

#include <input/touch_input.h>

#define MAX_TOUCH_INPUT_LISTENERS 16

typedef void (*touch_input_listener_t)(const touch_event_t *);

int touch_register_listener(touch_input_listener_t cb);
void touch_input_dispatch(const touch_event_t *ev);
