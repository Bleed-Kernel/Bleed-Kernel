#pragma once

#include <stdint.h>
#include <devices/type/tty_device.h>
#include <input/keyboard_input.h>

typedef void (*keyboard_callback_t)(char c);

/// @brief Handle PS2 Keyboard Interrupt from the PIC
/// @param irq value
void PS2_Keyboard_Interrupt(uint8_t irq);

/// @brief set the PS2 Keyboard Handler
/// @param cb 
void PS2_Keyboard_set_callback(keyboard_callback_t cb);

void PS2_Keyboard_callback(char c);

void PS2_Keyboard_init();

static const char keymap[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a',
    's','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c',
    'v','b','n','m',',','.','/',0,'*',0,' ',0
};

static const char keymap_shift[128] = {
    0,27,'!','"','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A',
    'S','D','F','G','H','J','K','L',':','@','~',0,'|','Z','X','C',
    'V','B','N','M','<','>','?',0,'*',0,' ',0
};

char tty_key_to_ascii(const keyboard_event_t *ev);