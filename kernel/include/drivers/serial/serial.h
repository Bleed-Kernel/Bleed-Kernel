#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

/// @brief initialse and test the serial ports so theyre ready for writing
/// @return success if any port came up
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

// raw to COM1, not kept in the log
void serial_send_n(const char *buf, size_t len);

// whether COM1 passed its loopback test and is available
int serial_present(void);

// whether the log has COM3 to itself instead of sharing COM1 with the shell
int serial_log_separate(void);

// rx gets whatever came in on COM1 each interrupt, called from the irq
void serial_rx_listen(serial_rx_t rx);

void serial_interrupt(void);

/// @brief Write a formatted string to the log port
/// @param fmt formatted string
/// @param  VARDIC
void serial_printf(const char* fmt, ...);

#endif
