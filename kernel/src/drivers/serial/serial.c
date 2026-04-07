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

static void serial_print_num(uint64_t val, int base, int is_signed, int uppercase, int width, char pad_char) {
    char buf[32]; 
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    int negative = 0;

    // Handle signed numbers
    if (is_signed && (int64_t)val < 0) {
        negative = 1;
        val = (uint64_t)(-(int64_t)val);
    }

    // Convert number to string (in reverse order)
    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }

    // Calculate how much padding we actually need
    int pad_len = width - i - negative;
    if (pad_len < 0) pad_len = 0;

    // Emit space padding BEFORE the minus sign
    if (pad_char == ' ' && pad_len > 0) {
        while (pad_len--) serial_emit_char(' ');
    }

    // Emit minus sign
    if (negative) {
        serial_emit_char('-');
    }

    // Emit zero padding AFTER the minus sign
    if (pad_char == '0' && pad_len > 0) {
        while (pad_len--) serial_emit_char('0');
    }

    // Emit the actual digits
    while (i > 0) {
        serial_emit_char(buf[--i]);
    }
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
        if (!*fmt) break;

        char pad_char = ' ';
        int width = 0;

        //Parse Zero-Padding Flag
        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }

        // Parse Field Width
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // Parse Length Modifiers
        int is_long = 0;
        int is_long_long = 0;
        int is_size_t = 0;

        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') {
            if (*fmt == 'l') {
                if (is_long) is_long_long = 1;
                else is_long = 1;
            } else if (*fmt == 'z') {
                is_size_t = 1;
            }
            fmt++;
        }

        // Parse Type Specifier
        switch (*fmt) {
            case 's': {
                char *str = va_arg(args, char *);
                if (!str) str = "(null)";
                serial_emit_str(str);
                break;
            }

            case 'c': {
                char c = (char)va_arg(args, int);
                serial_emit_char(c);
                break;
            }

            case 'd':
            case 'i': {
                int64_t v;
                if (is_long_long) v = va_arg(args, long long);
                else if (is_size_t) v = va_arg(args, ssize_t);
                else if (is_long) v = va_arg(args, long);
                else v = va_arg(args, int);
                
                serial_print_num((uint64_t)v, 10, 1, 0, width, pad_char);
                break;
            }

            case 'u': {
                uint64_t v;
                if (is_long_long) v = va_arg(args, unsigned long long);
                else if (is_size_t) v = va_arg(args, size_t);
                else if (is_long) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);
                
                serial_print_num(v, 10, 0, 0, width, pad_char);
                break;
            }

            case 'x':
            case 'X': {
                uint64_t v;
                if (is_long_long) v = va_arg(args, unsigned long long);
                else if (is_size_t) v = va_arg(args, size_t);
                else if (is_long) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);
                
                serial_print_num(v, 16, 0, *fmt == 'X', width, pad_char);
                break;
            }

            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(args, void *);
                serial_emit_str("0x");
                serial_print_num(v, 16, 0, 0, 16, '0'); 
                break;
            }

            case '%':
                serial_emit_char('%');
                break;

            default:
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