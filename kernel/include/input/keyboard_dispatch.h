#include <stdint.h>
#include <input/keyboard_input.h>

#define MAX_INPUT_LISTENERS     16

typedef void (*input_listener_t)(const keyboard_event_t *);

int  keyboard_register_listener(input_listener_t cb);
void keyboard_input_dispatch(const keyboard_event_t *ev);