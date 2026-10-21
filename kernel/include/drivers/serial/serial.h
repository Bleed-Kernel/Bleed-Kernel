#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

/// @brief initialse and test the serial port so its ready for writing
/// @return success
int serial_init();

/// @brief write a string to serial
/// @param str const string
void serial_write(const char* str);

/// @brief write a fixed-size buffer to serial
/// @param buf bytes to write
/// @param len number of bytes
void serial_write_n(const char *buf, size_t len);

/// @brief write a hex value to the screen from a uint
/// @param value uint value
void serial_write_hex(uint64_t value);

/// @brief total bytes ever emitted to serial
size_t serial_log_size(void);

/// @brief copy out of the serial log ring
/// @return bytes copied
size_t serial_log_read(char *dst, size_t len, size_t offset);

typedef void (*serial_rx_t)(const char *buf, size_t len);

// raw to the port, not kept in the log
void serial_send_n(const char *buf, size_t len);

// whether COM1 passed its loopback test and is available
int serial_present(void);

// rx gets whatever came in on each interrupt, called from the irq
void serial_rx_listen(serial_rx_t rx);

void serial_interrupt(void);

/// @brief Write a formatted string to COM1
/// @param fmt formatted string
/// @param  VARDIC
void serial_printf(const char* fmt, ...);

#endif
