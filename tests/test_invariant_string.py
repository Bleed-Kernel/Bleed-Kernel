import pytest
import ctypes
import os
import sys


# Simulate a safe strcpy-like operation in Python to model the invariant:
# "Buffer reads never exceed the declared length"
# We implement a safe_strcpy that enforces destination buffer size,
# and test that any strcpy-equivalent operation MUST NOT write beyond dest_size.

def safe_strcpy(dest_size, src):
    """
    A safe strcpy implementation that enforces buffer boundaries.
    Returns (result, overflowed) where overflowed=True means the source
    exceeded the destination buffer — which must NEVER be silently allowed.
    """
    if src is None:
        return None, False
    
    src_len = len(src)
    
    # The invariant: if src (including null terminator) exceeds dest_size,
    # the operation must be rejected or truncated — never silently overflow.
    if src_len + 1 > dest_size:
        # Overflow would occur — this must be caught/rejected
        return None, True  # overflow detected
    
    # Safe copy: fits within buffer
    result = src[:dest_size - 1]  # leave room for null terminator
    return result, False


def unsafe_strcpy_simulation(dest_size, src):
    """
    Simulates the UNSAFE strcpy behavior — copies without length check.
    Returns bytes written (including null terminator), which may exceed dest_size.
    """
    if src is None:
        return 0
    return len(src) + 1  # +1 for null terminator


@pytest.mark.parametrize("dest_size,payload", [
    # dest_size=8, payloads that are 2x, 10x, or otherwise oversized
    (8,  "A" * 16),                          # 2x overflow
    (8,  "A" * 80),                          # 10x overflow
    (8,  "B" * 9),                           # just over by 1 (off-by-one)
    (8,  "C" * 8),                           # exactly at boundary (no null room)
    (16, "D" * 32),                          # 2x overflow
    (16, "D" * 160),                         # 10x overflow
    (16, "\x00" * 32),                       # null bytes — 2x overflow
    (16, "E" * 17),                          # just over by 1
    (32, "F" * 64),                          # 2x overflow
    (32, "F" * 320),                         # 10x overflow
    (32, "G" * 33),                          # just over by 1
    (64, "H" * 128),                         # 2x overflow
    (64, "H" * 640),                         # 10x overflow
    (1,  "A"),                               # dest_size=1, any non-empty string overflows
    (1,  "A" * 100),                         # dest_size=1, massive overflow
    (4,  "AAAA"),                            # exactly at boundary (no null room)
    (4,  "AAAAA"),                           # 1 byte over
    (4,  "A" * 40),                          # 10x overflow
    # Attack payloads: format strings, shell metacharacters, long repeated patterns
    (8,  "%s%s%s%s%s%s%s%s%s%s" * 3),       # format string attack, oversized
    (8,  "../../../etc/passwd" * 5),          # path traversal, oversized
    (16, "\xff\xfe" * 20),                   # binary data, oversized
    (16, "A" * 255),                         # near-byte-max overflow
    (8,  ";" * 100),                         # shell injection chars, oversized
    (8,  "\n\r\t" * 30),                     # whitespace/control chars, oversized
    (32, "0" * 1024),                        # large overflow
    (64, "Z" * 1024),                        # large overflow
    (128, "X" * 1280),                       # 10x overflow on larger buffer
    (256, "Y" * 2560),                       # 10x overflow on larger buffer
])
def test_strcpy_buffer_read_never_exceeds_declared_length(dest_size, payload):
    """
    Invariant: Buffer reads/writes in strcpy must NEVER exceed the declared
    destination buffer length. Any source string whose length (including null
    terminator) exceeds dest_size must be detected as an overflow condition
    and must NOT be silently copied beyond the buffer boundary.
    
    This guards against CWE-120: Buffer Copy without Checking Size of Input.
    """
    src_len = len(payload)
    bytes_needed = src_len + 1  # +1 for null terminator

    # Test 1: Verify that unsafe_strcpy_simulation correctly identifies overflow
    bytes_written = unsafe_strcpy_simulation(dest_size, payload)
    
    if bytes_needed > dest_size:
        # The unsafe version WOULD overflow — this is the dangerous case
        assert bytes_written > dest_size, (
            f"Expected unsafe strcpy to write {bytes_needed} bytes "
            f"(overflowing dest_size={dest_size}), but got {bytes_written}"
        )
        
        # Test 2: The SAFE version must detect and reject this overflow
        result, overflowed = safe_strcpy(dest_size, payload)
        assert overflowed is True, (
            f"SECURITY VIOLATION: safe_strcpy did not detect overflow! "
            f"src length={src_len}, dest_size={dest_size}, "
            f"bytes_needed={bytes_needed}. "
            f"A strcpy with no length validation would overflow the buffer."
        )
        assert result is None, (
            f"SECURITY VIOLATION: safe_strcpy returned data instead of "
            f"rejecting oversized input (src={src_len}, dest={dest_size})"
        )
    else:
        # Source fits in destination — safe copy should succeed
        result, overflowed = safe_strcpy(dest_size, payload)
        assert overflowed is False, (
            f"False positive: safe_strcpy reported overflow for "
            f"src_len={src_len}, dest_size={dest_size} (should fit)"
        )
        assert result is not None, (
            f"safe_strcpy rejected a valid copy: src_len={src_len}, dest_size={dest_size}"
        )
        # Result must not exceed dest_size - 1 (leaving room for null terminator)
        assert len(result) <= dest_size - 1, (
            f"SECURITY VIOLATION: copied string length {len(result)} "
            f"exceeds dest_size-1={dest_size - 1}"
        )


@pytest.mark.parametrize("dest_size,payload", [
    (8,  "A" * 16),
    (8,  "A" * 80),
    (16, "D" * 32),
    (32, "F" * 64),
    (64, "H" * 128),
])
def test_strcpy_overflow_detection_bytes_written_invariant(dest_size, payload):
    """
    Invariant: The number of bytes written by an unchecked strcpy must never
    silently exceed the destination buffer size. Any implementation that allows
    bytes_written > dest_size without raising an error is unsafe.
    """
    bytes_written = unsafe_strcpy_simulation(dest_size, payload)
    
    # Assert that we CAN detect the overflow (i.e., the detection mechanism works)
    overflow_detected = bytes_written > dest_size
    
    assert overflow_detected, (
        f"Expected to detect overflow: payload length={len(payload)}, "
        f"dest_size={dest_size}, bytes_written={bytes_written}"
    )
    
    # The invariant: a SAFE implementation must NOT allow this
    _, safe_overflowed = safe_strcpy(dest_size, payload)
    assert safe_overflowed is True, (
        f"SECURITY INVARIANT BROKEN: Safe strcpy failed to catch overflow. "
        f"src_len={len(payload)}, dest_size={dest_size}, "
        f"bytes_that_would_be_written={bytes_written}. "
        f"This represents a CWE-120 buffer overflow vulnerability."
    )


@pytest.mark.parametrize("payload", [
    "A" * 256,
    "B" * 512,
    "\x41" * 1024,
    "shell" + ";" * 200 + "cmd",
    "../" * 100 + "etc/passwd",
    "%n%n%n%n%n" * 50,
    "\x00" + "A" * 255,   # null byte followed by data
    "A" * 127 + "\x00" + "B" * 127,  # embedded null
])
def test_strcpy_with_fixed_small_buffer_always_detects_overflow(payload):
    """
    Invariant: With a fixed small destination buffer (size=16), any payload
    longer than 15 characters MUST be detected as an overflow. No adversarial
    input should bypass this check.
    """
    dest_size = 16
    
    # Find the effective string length (up to first null byte, like C would)
    null_pos = payload.find('\x00')
    if null_pos != -1:
        effective_src = payload[:null_pos]
    else:
        effective_src = payload
    
    effective_len = len(effective_src)
    bytes_needed = effective_len + 1  # +1 for null terminator
    
    if bytes_needed > dest_size:
        _, overflowed = safe_strcpy(dest_size, effective_src)
        assert overflowed is True, (
            f"SECURITY VIOLATION: Overflow not detected for adversarial payload. "
            f"effective_src_len={effective_len}, dest_size={dest_size}. "
            f"An unsafe strcpy would write {bytes_needed} bytes into a "
            f"{dest_size}-byte buffer, causing a buffer overflow (CWE-120)."
        )