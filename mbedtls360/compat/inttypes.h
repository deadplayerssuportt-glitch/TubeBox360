/* inttypes.h minimo para el compilador de la Xbox 360 (VS2010 + XDK).
 * mbedTLS solo usa PRIuPTR (en debug.h). Escrito para TubeBox360. */
#ifndef TB_COMPAT_INTTYPES_H
#define TB_COMPAT_INTTYPES_H

#include "stdint.h"

#define PRId32   "d"
#define PRIu32   "u"
#define PRIx32   "x"
#define PRId64   "I64d"
#define PRIu64   "I64u"
#define PRIx64   "I64x"
#define PRIdPTR  "d"
#define PRIuPTR  "u"
#define PRIxPTR  "x"

#endif /* TB_COMPAT_INTTYPES_H */
