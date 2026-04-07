#ifndef STDINT_H
#define STDINT_H

# define INT8_MIN		(-128)
# define INT16_MIN		(-32767-1)
# define INT32_MIN		(-2147483647-1)
# define INT64_MIN		(-__INT64_C(9223372036854775807)-1)

# define INT8_MAX		(127)
# define INT16_MAX		(32767)
# define INT32_MAX		(2147483647)
# define INT64_MAX		(__INT64_C(9223372036854775807))

# define UINT8_MAX		(255)
# define UINT16_MAX		(65535)
# define UINT32_MAX		(4294967295U)
# define UINT64_MAX		(__UINT64_C(18446744073709551615))


typedef signed char        int8_t;
typedef unsigned char      uint8_t;

typedef signed short       int16_t;
typedef unsigned short     uint16_t;

typedef signed int         int32_t;
typedef unsigned int       uint32_t;

typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

typedef int8_t          int_least8_t;
typedef uint8_t         uint_least8_t;
typedef int16_t         int_least16_t;
typedef uint16_t        uint_least16_t;
typedef int32_t         int_least32_t;
typedef uint32_t        uint_least32_t;
typedef int64_t         int_least64_t;
typedef uint64_t        uint_least64_t;

typedef int                 int_fast8_t;
typedef unsigned            uint_fast8_t;
typedef int                 int_fast16_t;
typedef unsigned            uint_fast16_t;
typedef int                 int_fast32_t;
typedef unsigned            uint_fast32_t;
typedef long long           int_fast64_t;
typedef unsigned long long  uint_fast64_t;

typedef long             intptr_t;
typedef unsigned long    uintptr_t;

typedef unsigned long   size_t;
typedef long            ssize_t;
typedef long            ptrdiff_t;

typedef int64_t         intmax_t;
typedef uint64_t        uintmax_t;

#endif
