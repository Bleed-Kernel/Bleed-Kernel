#pragma once

#include <stdint.h>

/// @brief Out Byte through port of value
/// @param port dest
/// @param value payload
inline void outb(uint16_t port, uint8_t value){
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/// @brief get the value of a port
/// @param port dest
/// @return uint8 value at port
inline uint8_t inb(uint16_t port){
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

inline void outw(uint16_t port, uint16_t value) {
    asm volatile ("outw %0, %1" 
                  : 
                  : "a"(value), "Nd"(port));
}

inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" 
                  : "=a"(ret) 
                  : "Nd"(port));
    return ret;
}

inline void outl(uint16_t port, uint32_t value){
    asm volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline uint32_t inl(uint16_t port){
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}