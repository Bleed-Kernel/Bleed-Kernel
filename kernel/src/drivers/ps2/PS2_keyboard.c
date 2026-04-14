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

#define KBD_DATA_PORT   0x60
#define KBD_CMD_PORT    0x64
#define PS2_STATUS_PORT 0x64
#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02
#define PS2_STATUS_AUX_DATA 0x20

// PS/2 controller config register bits 
#define PS2_CFG_KBD_IRQ     (1 << 0)   // IRQ1  enable 
#define PS2_CFG_AUX_IRQ     (1 << 1)   // IRQ12 enable 
#define PS2_CFG_KBD_CLOCK   (1 << 4)   // 0 = enabled  
#define PS2_CFG_AUX_CLOCK   (1 << 5)   // 0 = enabled  
#define PS2_CFG_XLAT        (1 << 6)   // scancode translation 

#define PS2_CMD_READ_CFG    0x20
#define PS2_CMD_WRITE_CFG   0x60
#define PS2_CMD_SELF_TEST   0xAA
#define PS2_CMD_KBD_TEST    0xAB

// PS/2 keyboard commands 
#define KBD_CMD_RESET       0xFF
#define KBD_CMD_ENABLE      0xF4
#define KBD_CMD_SCANSET     0xF0
#define KBD_ACK             0xFA
#define KBD_SELF_TEST_OK    0xAA

static bool shift = false;
static bool ctrl  = false;
static bool alt   = false;
static bool caps  = false;

static keyboard_callback_t kb_callback = NULL;

// helpers
static void ps2_flush(void) {
    // drain anything the controller left in the output buffer 
    int limit = 128;
    while (limit-- > 0 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL))
        (void)inb(KBD_DATA_PORT);
}

static void ps2_wait_write(void) {
    int t = 100000;
    while (t-- > 0 && (inb(PS2_STATUS_PORT) & PS2_STATUS_IN_FULL));
}

static int ps2_wait_read(void) {
    int t = 100000;
    while (t-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL)
            return 0;
    }
    return -1;
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(KBD_CMD_PORT, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(KBD_DATA_PORT, data);
}

static int ps2_read_data(uint8_t *out) {
    if (ps2_wait_read() < 0) return -1;
    *out = inb(KBD_DATA_PORT);
    return 0;
}

// Send a command directly to the keyboard device 
static int kbd_send(uint8_t cmd) {
    uint8_t ack;
    int retries = 3;
    while (retries-- > 0) {
        ps2_write_data(cmd);
        if (ps2_read_data(&ack) < 0) return -1;
        if (ack == KBD_ACK) return 0;
    }
    return -1;
}

// keymap
char tty_key_to_ascii(const keyboard_event_t *ev) {
    uint16_t sc = ev->keycode;
    if (sc >= 128) return 0;
    if (sc == 0x56)
        return (ev->keymod & KEYMOD_SHIFT) ? '|' : '\\';
    if (ev->keymod & KEYMOD_SHIFT)
        return keymap_shift[sc];
    if (ev->keymod & KEYMOD_CAPS) {
        char c = keymap[sc];
        if (c >= 'a' && c <= 'z') c -= 32;
        return c;
    }
    return keymap[sc];
}

// irq handler
void PS2_Keyboard_Interrupt(uint8_t irq) {
    (void)irq;

    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUT_FULL)) return;
    if (status & PS2_STATUS_AUX_DATA)   return; // belongs to mouse 

    uint8_t sc = inb(KBD_DATA_PORT);
    uint8_t released = sc & 0x80;
    sc &= 0x7F;

    if (sc == 0x2A || sc == 0x36) shift = !released;
    else if (sc == 0x1D)           ctrl  = !released;
    else if (sc == 0x38)           alt   = !released;
    else if (sc == 0x3A && !released) caps ^= 1;

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

void PS2_Keyboard_set_callback(keyboard_callback_t cb) {
    kb_callback = cb;
}

void PS2_Keyboard_callback(char c) {
    tty_process_input((tty_t *)console_get_active_console()->internal_data, c);
}


void PS2_Keyboard_init(void) {
    uint8_t cfg = 0;

    ps2_flush();

    ps2_write_cmd(PS2_CMD_READ_CFG);
    if (ps2_read_data(&cfg) < 0) return;
     
    cfg |=  PS2_CFG_KBD_IRQ;
    cfg &= ~PS2_CFG_KBD_CLOCK;
    
    // scancode translation has to be enabled
    cfg |= PS2_CFG_XLAT;        

    ps2_write_cmd(PS2_CMD_WRITE_CFG);
    ps2_write_data(cfg);

    // reset and enable the keyboard 
    if (kbd_send(KBD_CMD_RESET) < 0) {
        // reset failed :/
        kbd_send(KBD_CMD_ENABLE);
    } else {
        uint8_t result = 0;
        ps2_read_data(&result); // consume self-test result (0xAA = ok) 
        kbd_send(KBD_CMD_ENABLE);
    }

    ps2_flush(); // eat any pending ACKs and go

    PS2_Keyboard_set_callback(PS2_Keyboard_callback);
}