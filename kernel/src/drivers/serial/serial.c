/*
    I am aware i dont have to use spinlocks here because you can wait for serial
    to be ready, but for consistancy across all devices im gonna just use my spinlock
*/

#include <cpu/io.h>
#include <stdint.h>
#include <stddef.h>
#include <mm/kalloc.h>
#include <stdarg.h>
#include <format.h>
#include <string.h>
#include <status.h>
#include <mm/spinlock.h>

#define PORT_COM1   0x3F8
#define SERIAL_TIMEOUT 100000

static int serial_available = 0;
static spinlock_t serial_lock;

static int is_serial_transmit_empty(){
    return inb(PORT_COM1 + 5) & 0x20;
}

int serial_init(){
    outb(PORT_COM1 + 4, 0x0F);
    uint8_t test = inb(PORT_COM1 + 4);
    if (test != 0x0F) {
        serial_available = 0;
        return status_print_error(SERIAL_NOT_AVAILABLE);
    }

    outb(PORT_COM1 + 1, 0x00);
    outb(PORT_COM1 + 3, 0x80);
    outb(PORT_COM1 + 0, 0x03);
    outb(PORT_COM1 + 1, 0x00);
    outb(PORT_COM1 + 3, 0x03);
    outb(PORT_COM1 + 2, 0xC7);
    outb(PORT_COM1 + 4, 0x0B);
    outb(PORT_COM1 + 4, 0x1E);
    outb(PORT_COM1 + 0, 0xAE);

    if (inb(PORT_COM1 + 0) != 0xAE){
        serial_available = 0;
        return status_print_error(SERIAL_NOT_AVAILABLE);
    }

    outb(PORT_COM1 + 4, 0x0F);
    serial_available = 1;
    spinlock_init(&serial_lock);
    return 0;
}

void serial_put_char(char c){
    if (!serial_available) return;

    if (c == '\n') {
        serial_put_char('\r');
    }

    uint32_t timeout = SERIAL_TIMEOUT;
    while (!is_serial_transmit_empty() && timeout--) { }
    if (timeout == 0) return;

    outb(PORT_COM1, c);
}

void serial_write(const char* str) {
    if (!serial_available) return;

    while (*str) {
        serial_put_char(*str++);
    }
}

void serial_write_hex(uint64_t value){
    if (!serial_available) return;

    char buffer[17];
    const char* hex = "0123456789ABCDEF";

    for (int i = 0; i < 16; i++){
        buffer[15-i] = hex[value & 0xF];
        value >>= 4;
    }
    buffer[16] = '\0';

    serial_write("0x");
    serial_write(buffer);
}

void serial_printf(const char* fmt, ...) {
    if (!serial_available) return;

    va_list args;
    va_start(args, fmt);
    
    unsigned long flags = irq_push();
    spinlock_acquire(&serial_lock);
    while (*fmt) {
        if (*fmt != '%') {
            serial_put_char(*fmt++);
            continue;
        }

        fmt++;
        char* str = NULL;

        switch (*fmt) {
            case 's':
                str = (char*)va_arg(args, char*);
                if (!str) str = "(null)";
                serial_write(str);
                break;

            case 'c': {
                char c = (char)va_arg(args, int);
                serial_put_char(c);
                break;
            }

            case 'd':
            case 'i': {
                int v = va_arg(args, int);
                str = itoa_signed((int64_t)v);
                if (str) { serial_write(str); kfree(str, strlen(str)); }
                break;
            }

            case 'u': {
                unsigned v = va_arg(args, unsigned);
                str = utoa_base(v, 10, 0);
                if (str) { serial_write(str); kfree(str, strlen(str)); }
                break;
            }

            case 'x': {
                unsigned v = va_arg(args, unsigned);
                str = utoa_base(v, 16, 0);
                if (str) { serial_write(str); kfree(str, strlen(str)); }
                break;
            }

            case 'X': {
                unsigned v = va_arg(args, unsigned);
                str = utoa_base(v, 16, 1);
                if (str) { serial_write(str); kfree(str, strlen(str)); }
                break;
            }

            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(args, void*);
                serial_write("0x");
                str = utoa_base(v, 16, 0);
                if (str) { serial_write(str); kfree(str, strlen(str)); }
                break;
            }

            case '%':
                serial_put_char('%');
                break;

            default:
                serial_put_char('%');
                serial_put_char(*fmt);
                break;
        }

        fmt++;
    }
    spinlock_release(&serial_lock);
    irq_restore(flags);
    va_end(args);
}
