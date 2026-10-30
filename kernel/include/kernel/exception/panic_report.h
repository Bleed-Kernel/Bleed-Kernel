#pragma once

#include <stddef.h>
#include <stdint.h>
#include <kernel/exception/panic.h>

#define PANIC_REPORT_URL    "https://bleedkernel.com/panic.html#"
#define PANIC_REPORT_FORMAT 3

/// @brief pack everything about the panic into a base64url string on the end of PANIC_REPORT_URL,
/// the layout is decoded by panic.html on the website so keep the two in step
/// @param caller where ke_panic was called from, only used when sf is NULL
/// @return the url, it lives in a static buffer
const char *panic_report_url(struct isr_stackframe *sf, const char *pstring, uint64_t rbp, uint64_t caller);
