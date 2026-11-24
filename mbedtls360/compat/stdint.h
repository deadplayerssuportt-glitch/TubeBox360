/* stdint.h minimo para el compilador de la Xbox 360 (VS2010 + XDK),
 * que no lo trae. Escrito para TubeBox360. Punteros de 32 bits. */
#ifndef TB_COMPAT_STDINT_H
#define TB_COMPAT_STDINT_H

#include <limits.h>

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef __int64            int64_t;
typedef unsigned __int64   uint64_t;

typedef int8_t    int_least8_t;
typedef uint8_t   uint_least8_t;
typedef int16_t   int_least16_t;
typedef uint16_t  uint_least16_t;
typedef int32_t   int_least32_t;
typedef uint32_t  uint_least32_t;
typedef int64_t   int_least64_t;
typedef uint64_t  uint_least64_t;
typedef int32_t   int_fast8_t;
typedef uint32_t  uint_fast8_t;
typedef int32_t   int_fast16_t;
typedef uint32_t  uint_fast16_t;
typedef int32_t   int_fast32_t;
typedef uint32_t  uint_fast32_t;
typedef int64_t   int_fast64_t;
typedef uint64_t  uint_fast64_t;
typedef int64_t   intmax_t;
typedef uint64_t  uintmax_t;

#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED
typedef int intptr_t;
#endif
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
typedef unsigned int uintptr_t;
#endif

#define INT8_MIN     (-127 - 1)
#define INT8_MAX     127
#define UINT8_MAX    0xFF
#define INT16_MIN    (-32767 - 1)
#define INT16_MAX    32767
#define UINT16_MAX   0xFFFF
#define INT32_MIN    (-2147483647 - 1)
#define INT32_MAX    2147483647
#define UINT32_MAX   0xFFFFFFFFU
#define INT64_MIN    (-9223372036854775807i64 - 1)
#define INT64_MAX    9223372036854775807i64
#define UINT64_MAX   0xFFFFFFFFFFFFFFFFui64
#define INTMAX_MIN   INT64_MIN
#define INTMAX_MAX   INT64_MAX
#define UINTMAX_MAX  UINT64_MAX
#define INTPTR_MIN   INT32_MIN
#define INTPTR_MAX   INT32_MAX
#define UINTPTR_MAX  UINT32_MAX
#ifndef SIZE_MAX
#define SIZE_MAX     UINT32_MAX
#endif

#define INT8_C(x)    (x)
#define INT16_C(x)   (x)
#define INT32_C(x)   (x)
#define INT64_C(x)   x##i64
#define UINT8_C(x)   (x)
#define UINT16_C(x)  (x)
#define UINT32_C(x)  x##U
#define UINT64_C(x)  x##ui64
#define INTMAX_C(x)  INT64_C(x)
#define UINTMAX_C(x) UINT64_C(x)

#endif /* TB_COMPAT_STDINT_H */
