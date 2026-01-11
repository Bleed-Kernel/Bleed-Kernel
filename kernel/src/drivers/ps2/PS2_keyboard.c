#include <drivers/ps2/PS2_keyboard.h>
#include <drivers/pic/pic.h>
#include <devices/type/tty_device.h>
#include <cpu/io.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <ansii.h>
#include <console/console.h>

#define KBD_PORT 0x60

static bool shift = false;
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

/// @brief push a char onto the keyboard buffer stack
static void push_char(char c) {
    int next = (head + 1) % BUF_SIZE;
    if (next != tail) {
        buf[head] = c;
        head = next;
    }
}

/// @brief pop a char off the keyboard buffer stack 
static int pop_char(void) {
    if (tail == head) return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return c;
}

/// @brief Handle PS2 Keyboard Interrupt from the PIC
/// @param irq value
void PS2_Keyboard_Interrupt(uint8_t irq) {
    uint8_t sc = inb(KBD_PORT);

    if (sc & 0x80) {
        sc &= ~0x80;
        if (sc == 0x2A || sc == 0x36) shift = false;
        PIC_EOI(irq);
        return;
    }

    if (sc == 0x2A || sc == 0x36) { shift = true; PIC_EOI(irq); return; }
    if (sc == 0x3A) { caps = !caps; PIC_EOI(irq); return; }

    char c = 0;
    if (sc < 128) {
        c = shift ? keymap_shift[sc] : keymap[sc];
        if (c >= 'a' && c <= 'z' && (caps ^ shift)) c -= 32;
    }

    if (c && kb_callback) {
        kb_callback(c);
    }

    if (c) push_char(c);
    PIC_EOI(irq);
}

/// @brief blocking get char
/// @return char ascii code int
int PS2_Keyboard_get_char(void) {
    int c;
    while ((c = pop_char()) == -1)
        __asm__ volatile("hlt");
    return c;
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
    pic_unmask(1);
    PS2_Flush();
    PS2_Keyboard_set_callback(PS2_Keyboard_callback);
}
