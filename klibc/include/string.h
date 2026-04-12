#ifndef STRING_H
#define STRING_H 1

#include <stdint.h>
#include <stddef.h>
#include <mm/smap.h>

/// @brief gets the length of a string
/// @param string string to evaluate
/// @return uint32 length
size_t strlen(const char *string);

/// @brief compare two blocks of memory
/// @param s1 block 1
/// @param s2 block 2
/// @param n size to evaluate
/// @return result
int memcmp(const void *s1, const void *s2, uint64_t n);

/// @brief move memory from source to destination
/// @param dest destination
/// @param src source
/// @param n size to evaluate
/// @return dest
void *memmove(void *dest, const void *src, uint64_t n);

/// @brief set memory to a location
/// @param s memory 
/// @param c address to set
/// @param n size
/// @return void
static inline void *memset(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *)s;

    for (uint64_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }

    return s;
}

/// @brief copy memory from one location to another
/// @param dest destination
/// @param src source
/// @param n size
/// @return dest
static inline void *memcpy(void *dest, const void *src, uint64_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    for (uint64_t i = 0; i < n; i++) {
        *pdest++ = *psrc++;
    }

    return dest;
}

/// @brief copy memory from one location to another for userspace
/// @param dest destination
/// @param src source
/// @param n size
/// @return dest
static inline void *umemcpy(void *dest, const void *src, uint64_t n) {
    SMAP_ALLOW{memcpy(dest, src, n);}
    return dest;
}

/// @brief copy a string
/// @param dest destination buffer
/// @param src source string
/// @return dest
char *strcpy(char *dest, const char *src);

/// @brief copy at most n chars from src to dest
/// @param dest destination
/// @param src source
/// @param n max chars
/// @return dest
char *strncpy(char *, const char *src, uint64_t n);

/// @brief concatenate two strings
/// @param dest destination buffer
/// @param src string to append
/// @return dest
char *strcat(char *dest, const char *src);

/// @brief compare two strings
/// @param s1 string 1
/// @param s2 string 2
/// @return <0, 0, or >0
int strcmp(const char *s1, const char *s2);

/// @brief compare at most n chars of two strings
/// @param s1 string 1
/// @param s2 string 2
/// @param n number of chars to compare
/// @return <0, 0, or >0
int strncmp(const char *s1, const char *s2, uint64_t n);

/// @brief find first occurrence of a char in a string
/// @param s string
/// @param c character
/// @return pointer or NULL
char *strchr(const char *s, int c);

/// @brief tokenize a string (non-thread-safe)
/// @param s string or NULL to continue
/// @param delim delimiter characters
/// @return next token or NULL
char *strtok(char * s, const char *delim);

/// @brief reentrant tokenizer
/// @param s string
/// @param delim delimiters
/// @param save state pointer
/// @return next token
char *strtok_r(char *s, const char *delim, char **save);

char *strdup(const char *s);
size_t strnlen(const char *s, uint64_t n);

#endif
