#include <drivers/ps2/PS2_keyboard.h>
#include <devices/type/tty_device.h>
#include <drivers/serial/serial.h>
#include <cpu/io.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <ansii.h>
#include <console/console.h>
#include <input/keyboard_dispatch.h>
#include <input/keyboard_input.h>
#include <ACPI/acpi.h>

#define KBD_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_AUX_DATA 0x20

static bool shift = false;
static bool ctrl = false;
static bool alt = false;
static bool caps = false;

#define BUF_SIZE 128
static volatile char buf[BUF_SIZE];
static volatile int head = 0, tail = 0;

static keyboard_callback_t kb_callback = NULL;

/// @brief flush the PS2 port of data
static inline void PS2_Flush(void) {
    if ((inb(0x64) & 0x01))
        (void)inb(0x60);
}

char tty_key_to_ascii(const keyboard_event_t *ev) {
    uint16_t sc = ev->keycode;
    if (sc >= 128) return 0;
    // this doesnt really work on my end in qemu, its weird but most places should be fine with > instead
    if (sc == 0x56)
        return (ev->keymod & KEYMOD_SHIFT) ? '|' : '\\';
    if (ev->keymod & KEYMOD_SHIFT)
        return keymap_shift[sc];
    if (ev->keymod & KEYMOD_CAPS) {
        char c = keymap[sc];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        return c;
    }
    return keymap[sc];
}

/// @brief Handle PS2 Keyboard Interrupt from the PIC
/// @param irq value
void PS2_Keyboard_Interrupt(uint8_t irq) {
    (void)irq;
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUT_FULL))
        return;
    if (status & PS2_STATUS_AUX_DATA)
        return;

    uint8_t sc = inb(KBD_PORT);
    uint8_t released = sc & 0x80;
    sc &= 0x7F;

    if (sc == 0x2A || sc == 0x36)
        shift = !released;
    else if (sc == 0x1D)
        ctrl = !released;
    else if (sc == 0x38)
        alt = !released;
    else if (sc == 0x3A && !released)
        caps ^= 1;

    keyboard_event_t ev = {
        .keycode = sc,
        .action  = released ? KEY_RELEASE : KEY_DOWN,
        .keymod  =
            (shift ? KEYMOD_SHIFT : 0) |
            (ctrl  ? KEYMOD_CTRL  : 0) |
            (alt   ? KEYMOD_ALT   : 0) |
            (caps  ? KEYMOD_CAPS  : 0)
    };

    keyboard_input_dispatch(&ev);
}

/// @brief set the PS2 Keyboard Handler
/// @param cb 
void PS2_Keyboard_set_callback(keyboard_callback_t cb) {
    kb_callback = cb;
}

void PS2_Keyboard_callback(char c){
    tty_process_input((tty_t*)console_get_active_console()->internal_data, c);
}

/// @brief flush the PS2 keyboard and prepare for execution
void PS2_Keyboard_init(){
    PS2_Flush();
    PS2_Keyboard_set_callback(PS2_Keyboard_callback);
}
