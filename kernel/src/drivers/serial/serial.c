/*
    this can be really funny on UEFI, and by that i mean it may cause the system
    not to boot :/
    for now i only want to write to serial but maybe recv later?
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
static spinlock_t serial_lock = {0};

static int is_serial_transmit_empty(void) {
    return inb(PORT_COM1 + 5) & 0x20;
}

static void serial_emit_char(char c) {
    if (c == '\n') {
        while (!is_serial_transmit_empty()) { }
        outb(PORT_COM1, '\r');
    }

    while (!is_serial_transmit_empty()) { }
    outb(PORT_COM1, c);
}

static void serial_emit_str(const char *str) {
    while (*str) {
        serial_emit_char(*str++);
    }
}

static void serial_emit_n(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        serial_emit_char(buf[i]);
    }
}

/// @brief initialse and test the serial port so its ready for writing
/// @return success
int serial_init(void) {
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

    if (inb(PORT_COM1 + 0) != 0xAE) {
        serial_available = 0;
        return status_print_error(SERIAL_NOT_AVAILABLE);
    }

    outb(PORT_COM1 + 4, 0x0F);
    spinlock_init(&serial_lock);
    serial_available = 1;
    return 0;
}

/// @brief put a single char, handles newline ansii
/// @param c character to put
void serial_put_char(char c) {
    if (!serial_available) return;

    unsigned long flags = irq_push();
    asm volatile("cli" ::: "memory");
    spinlock_acquire(&serial_lock);
    serial_emit_char(c);
    spinlock_release(&serial_lock);
    irq_restore(flags);
}

/// @brief write a string to serial
/// @param str const string
void serial_write(const char *str) {
    if (!serial_available) return;

    unsigned long flags = irq_push();
    asm volatile("cli" ::: "memory");
    spinlock_acquire(&serial_lock);
    serial_emit_str(str);
    spinlock_release(&serial_lock);
    irq_restore(flags);
}

void serial_write_n(const char *buf, size_t len) {
    if (!serial_available || !buf || len == 0) return;

    unsigned long flags = irq_push();
    asm volatile("cli" ::: "memory");
    spinlock_acquire(&serial_lock);
    serial_emit_n(buf, len);
    spinlock_release(&serial_lock);
    irq_restore(flags);
}

/// @brief write a hex value to the screen from a uint
/// @param value uint value
void serial_write_hex(uint64_t value) {
    if (!serial_available) return;

    char buffer[17];
    const char *hex = "0123456789ABCDEF";

    for (int i = 0; i < 16; i++) {
        buffer[15 - i] = hex[value & 0xF];
        value >>= 4;
    }
    buffer[16] = '\0';

    unsigned long flags = irq_push();
    asm volatile("cli" ::: "memory");
    spinlock_acquire(&serial_lock);
    serial_emit_str("0x");
    serial_emit_str(buffer);
    spinlock_release(&serial_lock);
    irq_restore(flags);
}

/// @brief Write a formatted string to COM1
/// @param fmt formatted string
/// @param  VARDIC
void serial_printf(const char *fmt, ...) {
    if (!serial_available) return;

    unsigned long flags = irq_push();
    asm volatile("cli" ::: "memory");
    spinlock_acquire(&serial_lock);

    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            serial_emit_char(*fmt++);
            continue;
        }

        fmt++;
        char *str = NULL;

        switch (*fmt) {
            case 's':
                str = (char *)va_arg(args, char *);
                if (!str) str = "(null)";
                serial_emit_str(str);
                break;

            case 'c': {
                char c = (char)va_arg(args, int);
                serial_emit_char(c);
                break;
            }

            case 'd':
            case 'i': {
                int v = va_arg(args, int);
                str = itoa_signed((int64_t)v);
                if (str) { serial_emit_str(str); kfree(str, strlen(str)); }
                break;
            }

            case 'u': {
                unsigned v = va_arg(args, unsigned);
                str = utoa_base(v, 10, 0);
                if (str) { serial_emit_str(str); kfree(str, strlen(str)); }
                break;
            }

            case 'x': {
                unsigned v = va_arg(args, unsigned);
                str = utoa_base(v, 16, 0);
                if (str) { serial_emit_str(str); kfree(str, strlen(str)); }
                break;
            }

            case 'X': {
                unsigned v = va_arg(args, unsigned);
                str = utoa_base(v, 16, 1);
                if (str) { serial_emit_str(str); kfree(str, strlen(str)); }
                break;
            }

            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(args, void *);
                serial_emit_str("0x");
                str = utoa_base(v, 16, 0);
                if (str) { serial_emit_str(str); kfree(str, strlen(str)); }
                break;
            }

            case '%':
                serial_emit_char('%');
                break;

            default:
                // Unknown specifier, print literally.
                serial_emit_char('%');
                serial_emit_char(*fmt);
                break;
        }

        fmt++;
    }

    va_end(args);
    spinlock_release(&serial_lock);
    irq_restore(flags);
}
