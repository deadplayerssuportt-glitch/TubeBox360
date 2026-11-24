/* tls_roots.h - Certificados raiz para verificar HTTPS (ver tls_roots.c). C89. */
#ifndef TB_TLS_ROOTS_H
#define TB_TLS_ROOTS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char          *name;
    const unsigned char *der;
    unsigned int         len;
} TlsRoot;

extern const TlsRoot g_tlsRoots[];
extern const int     g_tlsRootCount;

#ifdef __cplusplus
}
#endif

#endif
