#pragma once

#include <input/mouse_input.h>

#define MAX_MOUSE_INPUT_LISTENERS 64

typedef void (*mouse_input_listener_t)(const mouse_event_t *);

int mouse_register_listener(mouse_input_listener_t cb);
void mouse_input_dispatch(const mouse_event_t *ev);
