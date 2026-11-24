/* config.h de mbedTLS 2.28.8 para TubeBox360 (Xbox 360, VS2010 + XDK).
 *
 * Reemplaza al config.h original de mbedTLS (la copia original quedo como
 * config_original.h, solo de referencia). Configuracion minima de CLIENTE:
 *   - TLS 1.2 (lo que acepta todo Google)
 *   - Intercambio ECDHE con certificado RSA (y ECDSA de respaldo)
 *   - Cifrados: CHACHA20-POLY1305 (rapido en CPU sin AES por hardware)
 *     y AES-GCM
 *   - Curvas: x25519, P-256 y P-384
 *   - Sin archivos, sin sockets de mbedTLS (los pone net.c), sin reloj,
 *     sin generador de entropia propio: los aleatorios salen de XNetRandom.
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_DEPRECATE)
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

/* ---- Sistema ---- */
/* Sin MBEDTLS_HAVE_ASM: el ensamblador de bn_mul.h no es para el
 * compilador de la Xbox; se usa la version en C. */
/* Sin MBEDTLS_HAVE_TIME: el reloj de la consola puede estar mal; ademas
 * asi no se revisan fechas de certificados. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_SNPRINTF_ALT     /* usa _snprintf con terminacion segura */
#define MBEDTLS_PLATFORM_VSNPRINTF_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_DEPRECATED_REMOVED

/* ---- Cifrado simetrico y resumen ---- */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C                    /* solo para leer certificados viejos */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C                  /* SHA-384 */

/* ---- Llave publica ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_NO_INTERNAL_RNG       /* siempre le pasamos el generador */

/* ---- Certificados ---- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C               /* para las raices de Google (paso 4) */
#define MBEDTLS_BASE64_C

/* ---- TLS ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_ALPN

/* Errores en texto (para la bitacora) */
#define MBEDTLS_ERROR_C

/* ---- Tamanos ---- */
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384  /* lo que manda el servidor (no se puede bajar) */
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096   /* lo que mandamos: peticiones chicas */
#define MBEDTLS_MPI_MAX_SIZE         512    /* hasta RSA 4096 */

/* Orden por omision de los cifrados que ofrecemos (net.c lo vuelve a poner) */
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,   \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,         \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,         \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384

#include "mbedtls/check_config.h"

#endif /* MBEDTLS_CONFIG_H */
