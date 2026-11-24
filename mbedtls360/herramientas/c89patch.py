# Parches para que mbedTLS 2.28.8 compile con el compilador C89 de VS2010.
import sys, os
L = sys.argv[1]
P = []
def p(f, old, new, count=1):
    P.append((f, old, new, count))

# asn1write.c
p("asn1write.c",
  "int mbedtls_asn1_write_len(unsigned char **p, unsigned char *start, size_t len)\n{\n",
  "int mbedtls_asn1_write_len(unsigned char **p, unsigned char *start, size_t len)\n{\n    int len_is_valid;\n")
p("asn1write.c", "    int len_is_valid = 1;\n", "    len_is_valid = 1;\n")
# bignum.c
p("bignum.c",
  "    MPI_VALIDATE_RET(X != NULL);\n    MPI_VALIDATE_RET(A != NULL);\n\n    /* mpi_mul_hlp can't deal with a leading 0. */\n    size_t n = A->n;",
  "    size_t n;\n    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    MPI_VALIDATE_RET(X != NULL);\n    MPI_VALIDATE_RET(A != NULL);\n\n    /* mpi_mul_hlp can't deal with a leading 0. */\n    n = A->n;")
p("bignum.c",
  "    /* Calculate A*b as A + A*(b-1) to take advantage of mpi_mul_hlp */\n    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n",
  "    /* Calculate A*b as A + A*(b-1) to take advantage of mpi_mul_hlp */\n")
p("bignum.c",
  "    mbedtls_mpi_uint x = N[0];\n\n    x += ((N[0] + 2) & 4) << 1;\n\n    for (unsigned int i = biL;",
  "    mbedtls_mpi_uint x = N[0];\n    unsigned int i;\n\n    x += ((N[0] + 2) & 4) << 1;\n\n    for (i = biL;")
p("bignum.c",
  "    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n\n    for (size_t i = 0; i < T_size; i++) {",
  "    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    size_t i;\n\n    for (i = 0; i < T_size; i++) {")
p("bignum.c",
  "    mbedtls_mpi RR, T, W[(size_t) 1 << MBEDTLS_MPI_WINDOW_SIZE], WW, Apos;\n    int neg;\n",
  "    mbedtls_mpi RR, T, W[(size_t) 1 << MBEDTLS_MPI_WINDOW_SIZE], WW, Apos;\n    int neg;\n    size_t w_table_used_size;\n    size_t x_index;\n")
p("bignum.c", "    const size_t w_table_used_size = (size_t) 1 << window_bitsize;\n",
  "    w_table_used_size = (size_t) 1 << window_bitsize;\n")
p("bignum.c", "    const size_t x_index = 0;\n", "    x_index = 0;\n")
# oid.c
p("oid.c", "    for (size_t i = 0; i < oid->len; i++) {", "    for (i = 0; i < oid->len; i++) {")
# sha512.c
p("sha512.c", "    int truncated = 0;\n", "    truncated = 0;\n")
# ssl_cli.c
p("ssl_cli.c", "    int renegotiating = 0;\n", "    renegotiating = 0;\n")
# ssl_tls.c
p("ssl_tls.c", "    int clear_in_buf = 1;\n", "    clear_in_buf = 1;\n")
# x509_crt.c
p("x509_crt.c", "    int extensions_allowed = 1;\n", "    extensions_allowed = 1;\n")
# pk_wrap.c
p("pk_wrap.c",
  "    mbedtls_mpi N;\n    mbedtls_mpi_init(&N);\n    int ret = mbedtls_rsa_export(",
  "    mbedtls_mpi N;\n    int ret;\n    size_t bitlen;\n    mbedtls_mpi_init(&N);\n    ret = mbedtls_rsa_export(")
p("pk_wrap.c", "    size_t bitlen = (ret == 0 ?", "    bitlen = (ret == 0 ?")
# ecp.c
p("ecp.c", "    int should_free_R = (ret != 0);\n", "    should_free_R = (ret != 0);\n")
p("ecp.c", "    int restarting = 0;\n#if defined(MBEDTLS_ECP_RESTARTABLE)\n    restarting = (rs_ctx",
  "    restarting = 0;\n#if defined(MBEDTLS_ECP_RESTARTABLE)\n    restarting = (rs_ctx")
# ecp_curves.c
p("ecp_curves.c", "    mbedtls_mpi_uint msw = (mbedtls_mpi_uint) -c;\n", "    msw = (mbedtls_mpi_uint) -c;\n")
# cipher.c
p("cipher.c", "    int ret = mbedtls_cipher_aead_encrypt(ctx, iv, iv_len, ad, ad_len,",
  "    ret = mbedtls_cipher_aead_encrypt(ctx, iv, iv_len, ad, ad_len,")

bad = 0
for f, old, new, c in P:
    path = os.path.join(L, f)
    s = open(path, encoding="utf-8").read()
    n = s.count(old)
    if n != c:
        print("NO COINCIDE (%d) en %s: %r" % (n, f, old[:70])); bad = 1; continue
    s = s.replace(old, new)
    open(path, "w", encoding="utf-8").write(s)
print("parches aplicados" if not bad else "hubo fallas")

# ---- segunda tanda ----
P2 = []
def q(f, old, new, count=1):
    P2.append((f, old, new, count))
q("ecp.c", "#else\n    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    mbedtls_mpi Zi, ZZi;\n",
  "#else\n    {   /* (VS2010/C89: bloque para declarar) */\n    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    mbedtls_mpi Zi, ZZi;\n")
q("ecp.c", "    return ret;\n#endif /* !defined(MBEDTLS_ECP_NO_FALLBACK) || !defined(MBEDTLS_ECP_NORMALIZE_JAC_ALT) */\n",
  "    return ret;\n    }\n#endif /* !defined(MBEDTLS_ECP_NO_FALLBACK) || !defined(MBEDTLS_ECP_NORMALIZE_JAC_ALT) */\n")
q("ecp.c", "#else\n    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    size_t i;\n    mbedtls_mpi *c, u, Zi, ZZi;\n",
  "#else\n    {   /* (VS2010/C89: bloque para declarar) */\n    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    size_t i;\n    mbedtls_mpi *c, u, Zi, ZZi;\n")
q("ecp.c", "#endif /* !defined(MBEDTLS_ECP_NO_FALLBACK) || !defined(MBEDTLS_ECP_NORMALIZE_JAC_MANY_ALT) */\n}",
  "    }\n#endif /* !defined(MBEDTLS_ECP_NO_FALLBACK) || !defined(MBEDTLS_ECP_NORMALIZE_JAC_MANY_ALT) */\n}")
for f, old, new, c in P2:
    path = os.path.join(L, f)
    s = open(path, encoding="utf-8").read()
    n = s.count(old)
    if n != c:
        print("NO COINCIDE (%d) en %s: %r" % (n, f, old[:70])); continue
    s = s.replace(old, new)
    open(path, "w", encoding="utf-8").write(s)

# ---- tercera tanda: declarar al inicio ----
P3 = []
def r(f, old, new):
    P3.append((f, old, new))
r("oid.c", "    char *p = buf;\n    size_t n = size;\n    unsigned int value = 0;\n",
  "    char *p = buf;\n    size_t n = size;\n    unsigned int value = 0;\n    size_t i;\n")
r("sha512.c", "    unsigned used;\n    uint64_t high, low;\n",
  "    unsigned used;\n    uint64_t high, low;\n    int truncated;\n")
r("ssl_cli.c", "    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    size_t i, n, olen, ext_len = 0;\n",
  "    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;\n    size_t i, n, olen, ext_len = 0;\n    int renegotiating;\n")
r("x509_crt.c", "    size_t len;\n    unsigned char *p, *end, *crt_end;\n    mbedtls_x509_buf sig_params1, sig_params2, sig_oid2;\n",
  "    size_t len;\n    unsigned char *p, *end, *crt_end;\n    mbedtls_x509_buf sig_params1, sig_params2, sig_oid2;\n    int extensions_allowed;\n")
r("ecp.c", "    unsigned char w, p_eq_g, i;\n    size_t d;\n    unsigned char T_size = 0, T_ok = 0;\n",
  "    unsigned char w, p_eq_g, i;\n    size_t d;\n    unsigned char T_size = 0, T_ok = 0;\n    int should_free_R;\n")
r("ecp.c", "    int ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;\n#if defined(MBEDTLS_ECP_INTERNAL_ALT)\n    char is_grp_capable = 0;\n#endif\n    ECP_VALIDATE_RET(grp != NULL);\n    ECP_VALIDATE_RET(R   != NULL);",
  "    int ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;\n    int restarting;\n#if defined(MBEDTLS_ECP_INTERNAL_ALT)\n    char is_grp_capable = 0;\n#endif\n    ECP_VALIDATE_RET(grp != NULL);\n    ECP_VALIDATE_RET(R   != NULL);")
r("ecp_curves.c", "{\n    size_t i;\n\n    /* Set N := 2^bits - 1 - N.",
  "{\n    size_t i;\n    mbedtls_mpi_uint msw;\n\n    /* Set N := 2^bits - 1 - N.")
for f, old, new in P3:
    path = os.path.join(L, f)
    s = open(path, encoding="utf-8").read()
    n = s.count(old)
    if n != 1:
        print("NO COINCIDE (%d) en %s: %r" % (n, f, old[:70])); continue
    s = s.replace(old, new)
    open(path, "w", encoding="utf-8").write(s)
r2 = [
 ("ssl_tls.c", "    size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;\n#endif\n\n#if !defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE) ||     \\\n    !defined(MBEDTLS_SSL_SRV_C)\n    ((void) partial);",
  "    size_t out_buf_len = MBEDTLS_SSL_OUT_BUFFER_LEN;\n#endif\n    int clear_in_buf;\n\n#if !defined(MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE) ||     \\\n    !defined(MBEDTLS_SSL_SRV_C)\n    ((void) partial);"),
 ("cipher.c", "int mbedtls_cipher_auth_encrypt_ext(mbedtls_cipher_context_t *ctx,\n                                    const unsigned char *iv, size_t iv_len,\n                                    const unsigned char *ad, size_t ad_len,\n                                    const unsigned char *input, size_t ilen,\n                                    unsigned char *output, size_t output_len,\n                                    size_t *olen, size_t tag_len)\n{\n",
  "int mbedtls_cipher_auth_encrypt_ext(mbedtls_cipher_context_t *ctx,\n                                    const unsigned char *iv, size_t iv_len,\n                                    const unsigned char *ad, size_t ad_len,\n                                    const unsigned char *input, size_t ilen,\n                                    unsigned char *output, size_t output_len,\n                                    size_t *olen, size_t tag_len)\n{\n#if defined(MBEDTLS_CIPHER_MODE_AEAD)\n    int ret;\n#endif\n"),
]
for f, old, new in r2:
    path = os.path.join(L, f)
    s = open(path, encoding="utf-8").read()
    n = s.count(old)
    if n != 1:
        print("NO COINCIDE (%d) en %s: %r" % (n, f, old[:70])); continue
    s = s.replace(old, new)
    open(path, "w", encoding="utf-8").write(s)
