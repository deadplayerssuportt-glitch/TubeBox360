/* net.c - Red de la Xbox 360 (XNet + Winsock del XDK). C89.
 *
 * Fase 2: la consola habla HTTPS directo con YouTube/googlevideo usando
 * mbedTLS 2.28 (proyecto mbedtls360). Ya no hace falta la PC.
 *   - DNS con XNetDnsLookup y cache de 5 minutos.
 *   - TLS 1.2 con SNI. Por ahora SIN verificar el certificado (paso 4).
 *   - HTTP/1.1 propio: Content-Length, chunked, redirecciones, Range.
 *   - Conexiones reutilizables por host (keep-alive) y reanudacion de
 *     sesion TLS (tickets), para no repetir el handshake con cada pedazo
 *     de video o cada miniatura.
 * El modo proxy (la PC con ProtoHTTPS) sigue disponible con
 * conexion=proxy en game:\config.ini.
 *
 * La pila de red de la Xbox 360 normalmente solo deja hablar con otras
 * consolas y con Xbox Live. Para TCP normal hay que pedir "sin seguridad":
 *   - XNET_STARTUP_BYPASS_SECURITY al arrancar XNet (devkit)
 *   - la opcion de socket 0x5801 (marcar el socket como inseguro), que es
 *     lo que usan los homebrew en consolas retail con RGH. */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "net.h"

#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"
#include "tls_roots.h"

#ifndef XNET_STARTUP_BYPASS_SECURITY
#define XNET_STARTUP_BYPASS_SECURITY 0x01
#endif

#define SO_MARKINSECURE   0x5801
#define LOG_PATH          "game:\\tubebox_red.txt"

#define RECV_TIMEOUT_MS   20000
#define RECV_SLICE_MS     500         /* recv espera de a poco para poder cancelar */
#define CONNECT_TIMEOUT   8000
#define MAX_RESPONSE      (16 * 1024 * 1024)
#define MAX_HEADER        (64 * 1024)
#define MAX_REDIRECTS     5
#define POOL_MAX          10          /* conexiones guardadas sin usar */
#define IDLE_MAX_MS       45000       /* despues de esto se cierran */
#define DNS_SLOTS         32
#define DNS_TTL_MS        300000
#define SESS_SLOTS        12
#define RBUF_SIZE         16384
#define HOST_MAX          128

/* Codigos de error para mbedTLS (los mismos que usa su net_sockets.c) */
#define TB_ERR_RECV       (-0x004C)
#define TB_ERR_SEND       (-0x004E)

static char g_proxyIp[32] = PROXY_IP;
static int  g_proxyPort = PROXY_PORT;
static int  g_direct = 1;               /* 1 = HTTPS directo, 0 = proxy de la PC */
static CRITICAL_SECTION g_logCs;
static int  g_logInit = 0;
static DWORD g_cancelTls = 0xFFFFFFFF;  /* bandera de cancelar de cada hilo */

/* Cada hilo puede dar una bandera (por ejemplo g_stop del reproductor): si
 * se pone en 1, la peticion que este haciendo ese hilo termina en menos de
 * medio segundo con "Cancelado". */
void net_set_cancel(volatile long *flag)
{
    if (g_cancelTls != 0xFFFFFFFF)
        TlsSetValue(g_cancelTls, (LPVOID)flag);
}

static int canceled(void)
{
    volatile long *f;

    if (g_cancelTls == 0xFFFFFFFF)
        return 0;
    f = (volatile long *)TlsGetValue(g_cancelTls);
    return f && *f;
}

static void set_err(char *err, int errSize, const char *msg, int code)
{
    if (!err || errSize <= 0)
        return;
    if (code)
        _snprintf(err, errSize - 1, "%s (%d)", msg, code);
    else
        _snprintf(err, errSize - 1, "%s", msg);
    err[errSize - 1] = 0;
}

void net_set_proxy(const char *ip, int port)
{
    if (ip && ip[0] && strlen(ip) < sizeof(g_proxyIp))
        strcpy(g_proxyIp, ip);
    if (port > 0 && port < 65536)
        g_proxyPort = port;
}

const char *net_get_proxy(int *port)
{
    if (port)
        *port = g_proxyPort;
    return g_proxyIp;
}

/* Verificar certificados HTTPS (config.ini: certificados=1/0). Por omision si. */
static int g_tlsVerify = 1;
static int g_rootsLoaded = 0;     /* raices de Google que se pudieron leer */
static void tls_apply_verify(void);
static void forget_connections(void);

void net_set_verify(int on)
{
    on = on ? 1 : 0;
    if (on == g_tlsVerify)
        return;
    g_tlsVerify = on;
    tls_apply_verify();
    forget_connections();   /* nada de lo que se abrio con la otra regla se reutiliza */
}

int net_verify_on(void)
{
    return g_tlsVerify && g_rootsLoaded > 0;
}

void net_set_direct(int on)
{
    g_direct = on ? 1 : 0;
}

int net_is_direct(void)
{
    return g_direct;
}

void net_log(const char *fmt, ...)
{
    char line[600];
    SYSTEMTIME st;
    va_list ap;
    FILE *f;
    int n;

    if (!g_logInit)
    {
        InitializeCriticalSection(&g_logCs);   /* (la primera vez es desde el hilo principal) */
        g_logInit = 1;
    }
    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line) - 1, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - 2 - n, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 2] = 0;
    strcat(line, "\n");
    OutputDebugStringA(line);
    EnterCriticalSection(&g_logCs);
    f = fopen(LOG_PATH, "ab");
    if (f)
    {
        fputs(line, f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */

/* "a.b.c.d" (los bytes de s_addr ya vienen en el orden de la red) */
static const char *ip_str(IN_ADDR ip, char *buf)
{
    const BYTE *b = (const BYTE *)&ip.s_addr;
    sprintf(buf, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    return buf;
}

static int lower_c(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* strncmp sin distinguir mayusculas */
static int ncase_eq(const char *a, const char *b, int n)
{
    int i;

    for (i = 0; i < n; ++i)
    {
        if (lower_c((unsigned char)a[i]) != lower_c((unsigned char)b[i]))
            return 0;
        if (!a[i])
            return 1;
    }
    return 1;
}

/* strstr sin distinguir mayusculas */
static const char *ncase_find(const char *hay, const char *needle)
{
    int n = (int)strlen(needle);

    if (!hay)
        return NULL;
    for (; *hay; ++hay)
        if (ncase_eq(hay, needle, n))
            return hay;
    return NULL;
}

static void mbed_err(char *err, int errSize, const char *what, int code)
{
    char txt[120];

    txt[0] = 0;
    mbedtls_strerror(code, txt, sizeof(txt));
    if (err && errSize > 0)
    {
        _snprintf(err, errSize - 1, "%s (-0x%04X %s)", what, (unsigned)(-code), txt);
        err[errSize - 1] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Arranque                                                            */

static CRITICAL_SECTION g_poolCs;
static CRITICAL_SECTION g_dnsCs;
static CRITICAL_SECTION g_sessCs;
static int  g_netReady = 0;
static int  g_tlsReady = 0;
static volatile LONG g_linkChecked = 0;
static mbedtls_ssl_config g_tlsConf[2];     /* 0 = solo certificado RSA (rapido), 1 = todos */
static mbedtls_x509_crt   g_caRoots;        /* raices de Google Trust Services (tls_roots.c) */

static const int k_suitesRsa[] =
{
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    0
};
static const int k_suitesAll[] =
{
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    0
};
static const mbedtls_ecp_group_id k_curves[] =
{
    MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_SECP384R1, MBEDTLS_ECP_DP_NONE
};
static const char *k_alpn[] = { "http/1.1", NULL };

/* Numeros aleatorios para TLS: los da el hardware de la consola */
static int tb_rng(void *ctx, unsigned char *out, size_t len)
{
    UINT n;

    (void)ctx;
    while (len > 0)
    {
        n = (UINT)(len > 256 ? 256 : len);
        XNetRandom(out, n);
        out += n;
        len -= n;
    }
    return 0;
}

static void sess_init_all(void);

/* Activa o quita la verificacion en las dos configuraciones (afecta a las
 * conexiones nuevas; las sesiones ya abiertas siguen igual) */
static void tls_apply_verify(void)
{
    int i, on = g_tlsVerify && g_rootsLoaded > 0;

    if (!g_tlsReady)
        return;
    for (i = 0; i < 2; ++i)
    {
        mbedtls_ssl_conf_ca_chain(&g_tlsConf[i], on ? &g_caRoots : NULL, NULL);
        mbedtls_ssl_conf_authmode(&g_tlsConf[i], on ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    }
}

static int tls_init(void)
{
    int i, r;

    /* raices de confianza (DER dentro del .xex) */
    mbedtls_x509_crt_init(&g_caRoots);
    g_rootsLoaded = 0;
    for (i = 0; i < g_tlsRootCount; ++i)
    {
        r = mbedtls_x509_crt_parse_der(&g_caRoots, g_tlsRoots[i].der, g_tlsRoots[i].len);
        if (r == 0)
            ++g_rootsLoaded;
        else
            net_log("[tls] no se pudo leer la raiz %s (-0x%04X)", g_tlsRoots[i].name, (unsigned)(-r));
    }
    net_log("[tls] raices de confianza: %d de %d; verificar certificados: %s", g_rootsLoaded, g_tlsRootCount,
            g_tlsVerify ? "si" : "NO (certificados=0 en config.ini)");

    for (i = 0; i < 2; ++i)
    {
        mbedtls_ssl_config_init(&g_tlsConf[i]);
        r = mbedtls_ssl_config_defaults(&g_tlsConf[i], MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
        if (r != 0)
        {
            net_log("[tls] config_defaults fallo -0x%04X", (unsigned)(-r));
            return 0;
        }
        /* la verificacion (raices de Google) la pone tls_apply_verify */
        mbedtls_ssl_conf_authmode(&g_tlsConf[i], MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&g_tlsConf[i], tb_rng, NULL);
        mbedtls_ssl_conf_min_version(&g_tlsConf[i], MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
        mbedtls_ssl_conf_max_version(&g_tlsConf[i], MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
        mbedtls_ssl_conf_ciphersuites(&g_tlsConf[i], i == 0 ? k_suitesRsa : k_suitesAll);
        mbedtls_ssl_conf_curves(&g_tlsConf[i], k_curves);
        mbedtls_ssl_conf_alpn_protocols(&g_tlsConf[i], k_alpn);
        mbedtls_ssl_conf_session_tickets(&g_tlsConf[i], MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
        mbedtls_ssl_conf_read_timeout(&g_tlsConf[i], 0);
    }
    return 1;
}

int net_init(void)
{
    XNetStartupParams xnsp;
    WSADATA wsa;
    int r;

    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(XNetStartupParams);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;

    /* la bitacora no crece para siempre: si pasa de 512 KB se empieza otra */
    {
        FILE *f = fopen(LOG_PATH, "rb");
        long len = 0;
        if (f)
        {
            fseek(f, 0, SEEK_END);
            len = ftell(f);
            fclose(f);
        }
        if (len > 512 * 1024)
            remove(LOG_PATH);
    }
    r = XNetStartup(&xnsp);
    net_log("---- TubeBox360: arranque de red (%s) ----", g_direct ? "HTTPS directo" : "proxy de la PC");
    if (r != 0)
    {
        net_log("[net] XNetStartup fallo (%d)", r);
        return 0;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        net_log("[net] WSAStartup fallo");
        return 0;
    }
    if (!g_netReady)
    {
        InitializeCriticalSection(&g_poolCs);
        InitializeCriticalSection(&g_dnsCs);
        InitializeCriticalSection(&g_sessCs);
        g_cancelTls = TlsAlloc();
        sess_init_all();
        g_tlsReady = tls_init();
        tls_apply_verify();
        g_netReady = 1;
    }
    OutputDebugStringA("[net] Red lista\n");
    return 1;
}

/* La consola tarda unos segundos en tener IP (DHCP) despues de XNetStartup:
 * antes de la primera conexion directa se espera hasta 10 s. */
static void wait_for_ip(void)
{
    XNADDR xna;
    DWORD t0, flags;

    if (g_linkChecked)
        return;
    t0 = GetTickCount();
    for (;;)
    {
        flags = XNetGetTitleXnAddr(&xna);
        if (flags != XNET_GET_XNADDR_PENDING || GetTickCount() - t0 > 10000)
            break;
        Sleep(100);
    }
    if (GetTickCount() - t0 > 200)
        net_log("[net] se espero la IP de la consola %lu ms", (unsigned long)(GetTickCount() - t0));
    InterlockedExchange(&g_linkChecked, 1);
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */

typedef struct
{
    char    host[HOST_MAX];
    IN_ADDR ip;
    DWORD   when;
} DnsSlot;

static DnsSlot g_dns[DNS_SLOTS];

/* DNS con el resolvedor de la consola. 1 = bien (ip en *addr). */
static int dns_lookup(const char *host, IN_ADDR *addr, int *status, DWORD *ms)
{
    XNDNS *dns = NULL;
    HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    DWORD t0 = GetTickCount();
    int ok = 0, r;

    *status = -1;
    if (!ev)
        return 0;
    r = XNetDnsLookup(host, ev, &dns);
    if (r == 0 && dns)
    {
        /* espera en tramos para poder cancelar (si se cancela, XNetDnsRelease
         * abajo corta la busqueda pendiente) */
        while (WaitForSingleObject(ev, RECV_SLICE_MS) == WAIT_TIMEOUT)
        {
            if (canceled() || GetTickCount() - t0 >= 8000)
                break;
        }
        *status = dns->iStatus;
        if (dns->iStatus == 0 && dns->cina > 0)
        {
            *addr = dns->aina[0];
            ok = 1;
        }
        XNetDnsRelease(dns);
    }
    else
        *status = r;
    CloseHandle(ev);
    *ms = GetTickCount() - t0;
    return ok;
}

static int dns_cached(const char *host, IN_ADDR *ip, char *err, int errSize)
{
    DWORD now = GetTickCount(), ms;
    unsigned long lit = inet_addr(host);
    int i, st, oldest = 0;
    char ipb[20];

    if (lit != INADDR_NONE)
    {
        ip->s_addr = lit;
        return 1;
    }
    EnterCriticalSection(&g_dnsCs);
    for (i = 0; i < DNS_SLOTS; ++i)
    {
        if (g_dns[i].host[0] && strcmp(g_dns[i].host, host) == 0 && now - g_dns[i].when < DNS_TTL_MS)
        {
            *ip = g_dns[i].ip;
            LeaveCriticalSection(&g_dnsCs);
            return 1;
        }
    }
    LeaveCriticalSection(&g_dnsCs);

    if (!dns_lookup(host, ip, &st, &ms))
    {
        net_log("[dns] %s FALLO (estado %d, %lu ms)", host, st, (unsigned long)ms);
        _snprintf(err, errSize - 1, "No se encontro el servidor %s (DNS %d)", host, st);
        err[errSize - 1] = 0;
        return 0;
    }
    net_log("[dns] %s -> %s (%lu ms)", host, ip_str(*ip, ipb), (unsigned long)ms);

    EnterCriticalSection(&g_dnsCs);
    for (i = 0; i < DNS_SLOTS; ++i)
    {
        if (!g_dns[i].host[0] || strcmp(g_dns[i].host, host) == 0)
        {
            oldest = i;
            break;
        }
        if (g_dns[i].when < g_dns[oldest].when)
            oldest = i;
    }
    strncpy(g_dns[oldest].host, host, HOST_MAX - 1);
    g_dns[oldest].host[HOST_MAX - 1] = 0;
    g_dns[oldest].ip = *ip;
    g_dns[oldest].when = now;
    LeaveCriticalSection(&g_dnsCs);
    return 1;
}

static void dns_forget(const char *host)
{
    int i;

    EnterCriticalSection(&g_dnsCs);
    for (i = 0; i < DNS_SLOTS; ++i)
        if (strcmp(g_dns[i].host, host) == 0)
            g_dns[i].host[0] = 0;
    LeaveCriticalSection(&g_dnsCs);
}

/* ------------------------------------------------------------------ */
/* TCP                                                                 */

static SOCKET tcp_connect(IN_ADDR ip, int port, DWORD timeoutMs, int *werr, DWORD *ms)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    BOOL yes = TRUE;
    DWORD timeout = timeoutMs, t0 = GetTickCount();

    *werr = 0;
    if (s == INVALID_SOCKET)
    {
        *werr = WSAGetLastError();
        return INVALID_SOCKET;
    }
    setsockopt(s, SOL_SOCKET, SO_MARKINSECURE, (const char *)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr = ip;

    /* connect sin bloquear: se espera en tramos de medio segundo (hasta
     * CONNECT_TIMEOUT) para poder cancelar, y luego el socket vuelve a ser
     * normal (bloqueante, con los tiempos de arriba) */
    {
        u_long nb = 1;
        fd_set wr, ex;
        struct timeval tv;
        int r;

        ioctlsocket(s, FIONBIO, &nb);
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        {
            r = WSAGetLastError();
            if (r != WSAEWOULDBLOCK && r != WSAEINPROGRESS)
                *werr = r;
            else
            {
                for (;;)
                {
                    FD_ZERO(&wr);
                    FD_ZERO(&ex);
                    FD_SET(s, &wr);
                    FD_SET(s, &ex);
                    tv.tv_sec = 0;
                    tv.tv_usec = RECV_SLICE_MS * 1000;
                    r = select(0, NULL, &wr, &ex, &tv);
                    if (r > 0)
                    {
                        /* listo: ver si fue conexion o error */
#ifdef SO_ERROR
                        int soErr = 0, len = sizeof(soErr);
                        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soErr, &len) == 0 && soErr)
                            *werr = soErr;
                        else
#endif
                        /* (Winsock, tambien el de la Xbox: si fallo, sale en "ex") */
                        if (FD_ISSET(s, &ex) || !FD_ISSET(s, &wr))
                            *werr = WSAECONNREFUSED;
                        break;
                    }
                    if (r < 0)
                    {
                        *werr = WSAGetLastError();
                        break;
                    }
                    if (canceled())
                    {
                        *werr = WSAEINTR;
                        break;
                    }
                    if (GetTickCount() - t0 >= CONNECT_TIMEOUT)
                    {
                        *werr = WSAETIMEDOUT;
                        break;
                    }
                }
            }
        }
        nb = 0;
        if (!*werr)
            ioctlsocket(s, FIONBIO, &nb);
    }
    if (*werr)
    {
        closesocket(s);
        s = INVALID_SOCKET;
    }
    *ms = GetTickCount() - t0;
    return s;
}

static int send_all(SOCKET s, const char *data, int len)
{
    int n;

    while (len > 0)
    {
        n = send(s, data, len, 0);
        if (n <= 0)
            return 0;
        data += n;
        len -= n;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Conexiones (TCP o TLS) con lectura en bufer                         */

typedef struct NetConn
{
    SOCKET s;
    int    tls;
    int    sslInit;
    mbedtls_ssl_context ssl;
    char   host[HOST_MAX];
    int    port;
    DWORD  idleSince;
    int    lastErr;
    int    waited;          /* ms esperando datos sin que llegue nada */
    int    cancel;          /* se cancelo (bandera del hilo) */
    int    gotAny;          /* llego al menos un byte de la respuesta actual */
    int    rpos, rlen;
    unsigned char rbuf[RBUF_SIZE];
} NetConn;

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    NetConn *c = (NetConn *)ctx;
    int n = send(c->s, (const char *)buf, (int)len, 0);

    if (n < 0)
    {
        c->lastErr = WSAGetLastError();
        if (c->lastErr == WSAEWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return TB_ERR_SEND;
    }
    return n;
}

/* recv con espera en tramos de RECV_SLICE_MS: entre tramo y tramo se mira
 * la bandera de cancelar. Devuelve >=0 bytes, -1 error, -2 sin datos aun
 * (volver a llamar), -3 tiempo agotado o cancelado. */
static int slice_recv(NetConn *c, char *buf, int len)
{
    int n = recv(c->s, buf, len, 0);

    if (n >= 0)
    {
        c->waited = 0;
        return n;
    }
    c->lastErr = WSAGetLastError();
    if (c->lastErr != WSAETIMEDOUT && c->lastErr != WSAEWOULDBLOCK)
        return -1;
    if (canceled())
    {
        c->cancel = 1;
        return -3;
    }
    c->waited += RECV_SLICE_MS;
    if (c->waited >= RECV_TIMEOUT_MS)
    {
        c->lastErr = WSAETIMEDOUT;
        return -3;
    }
    return -2;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    NetConn *c = (NetConn *)ctx;
    int n = slice_recv(c, (char *)buf, (int)len);

    if (n >= 0)
        return n;   /* 0 = el servidor cerro */
    if (n == -2)
        return MBEDTLS_ERR_SSL_WANT_READ;   /* mbedTLS deja seguir despues */
    if (n == -3)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    return TB_ERR_RECV;
}

static void conn_close(NetConn *c)
{
    if (!c)
        return;
    if (c->sslInit)
        mbedtls_ssl_free(&c->ssl);
    if (c->s != INVALID_SOCKET)
        closesocket(c->s);
    free(c);
}

/* --- Reanudacion de sesion TLS (tickets) por host --- */

typedef struct
{
    char  host[HOST_MAX];
    int   valid;
    DWORD when;
    mbedtls_ssl_session s;
} SessSlot;

static SessSlot g_sess[SESS_SLOTS];

static void sess_init_all(void)
{
    int i;

    for (i = 0; i < SESS_SLOTS; ++i)
    {
        g_sess[i].host[0] = 0;
        g_sess[i].valid = 0;
        mbedtls_ssl_session_init(&g_sess[i].s);
    }
}

/* Pone la sesion guardada de este host. Devuelve 1 si habia una. */
static int sess_load(const char *host, mbedtls_ssl_context *ssl, unsigned char *master)
{
    int i, ok = 0;

    EnterCriticalSection(&g_sessCs);
    for (i = 0; i < SESS_SLOTS; ++i)
    {
        if (g_sess[i].valid && strcmp(g_sess[i].host, host) == 0)
        {
            if (mbedtls_ssl_set_session(ssl, &g_sess[i].s) == 0)
            {
                ok = 1;
                memcpy(master, g_sess[i].s.master, 48);
            }
            break;
        }
    }
    LeaveCriticalSection(&g_sessCs);
    return ok;
}

static void sess_save(const char *host, const mbedtls_ssl_context *ssl)
{
    mbedtls_ssl_session tmp;
    int i, slot = -1, oldest = 0;

    mbedtls_ssl_session_init(&tmp);
    if (mbedtls_ssl_get_session(ssl, &tmp) != 0)
    {
        mbedtls_ssl_session_free(&tmp);
        return;
    }
    EnterCriticalSection(&g_sessCs);
    for (i = 0; i < SESS_SLOTS; ++i)
    {
        if (g_sess[i].valid && strcmp(g_sess[i].host, host) == 0)
        {
            slot = i;
            break;
        }
        if (!g_sess[i].valid && slot < 0)
            slot = i;
        if (g_sess[i].when < g_sess[oldest].when)
            oldest = i;
    }
    if (slot < 0)
        slot = oldest;
    mbedtls_ssl_session_free(&g_sess[slot].s);
    g_sess[slot].s = tmp;                 /* (la copia queda a cargo del cache) */
    strncpy(g_sess[slot].host, host, HOST_MAX - 1);
    g_sess[slot].host[HOST_MAX - 1] = 0;
    g_sess[slot].valid = 1;
    g_sess[slot].when = GetTickCount();
    LeaveCriticalSection(&g_sessCs);
}

static void sess_forget(const char *host)
{
    int i;

    EnterCriticalSection(&g_sessCs);
    for (i = 0; i < SESS_SLOTS; ++i)
    {
        if (g_sess[i].valid && strcmp(g_sess[i].host, host) == 0)
        {
            mbedtls_ssl_session_free(&g_sess[i].s);
            mbedtls_ssl_session_init(&g_sess[i].s);
            g_sess[i].valid = 0;
        }
    }
    LeaveCriticalSection(&g_sessCs);
}

/* Hosts que no aceptaron los cifrados con certificado RSA: a esos se les
 * ofrece todo desde el principio. */
static char g_allSuites[16][HOST_MAX];

static int host_wants_all(const char *host)
{
    int i, yes = 0;

    EnterCriticalSection(&g_sessCs);
    for (i = 0; i < 16 && !yes; ++i)
        yes = strcmp(g_allSuites[i], host) == 0;
    LeaveCriticalSection(&g_sessCs);
    return yes;
}

static void host_set_all(const char *host)
{
    static int next = 0;

    EnterCriticalSection(&g_sessCs);
    strncpy(g_allSuites[next], host, HOST_MAX - 1);
    g_allSuites[next][HOST_MAX - 1] = 0;
    next = (next + 1) % 16;
    LeaveCriticalSection(&g_sessCs);
}

/* Handshake TLS sobre c->s. 1 = bien. */
static int tls_handshake(NetConn *c, int confIndex, int *code, char *err, int errSize)
{
    unsigned char oldMaster[48];
    const mbedtls_ssl_session *now;
    DWORD t0 = GetTickCount();
    int r, offered;

    mbedtls_ssl_init(&c->ssl);
    c->sslInit = 1;
    *code = 0;
    r = mbedtls_ssl_setup(&c->ssl, &g_tlsConf[confIndex]);
    if (r == 0)
        r = mbedtls_ssl_set_hostname(&c->ssl, c->host);
    if (r != 0)
    {
        *code = r;
        mbed_err(err, errSize, "No se pudo preparar TLS", r);
        return 0;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    offered = sess_load(c->host, &c->ssl, oldMaster);

    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
    {
        if ((r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) && canceled())
        {
            c->cancel = 1;
            set_err(err, errSize, "Cancelado", 0);
            *code = r;
            return 0;
        }
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
        {
            /* el servidor no demostro ser quien dice: no se manda nada */
            char why[256];
            unsigned int flags = mbedtls_ssl_get_verify_result(&c->ssl);
            why[0] = 0;
            mbedtls_x509_crt_verify_info(why, sizeof(why), "", flags);
            {
                char *p;
                for (p = why; *p; ++p)
                    if (*p == '\n')
                        *p = ' ';
            }
            *code = r;
            net_log("[tls] %s: CERTIFICADO NO VALIDO (0x%X): %s", c->host, flags, why);
            set_err(err, errSize, "Certificado del servidor no valido (red insegura o reloj/proxy de por medio)", 0);
            if (offered)
                sess_forget(c->host);
            return 0;
        }
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            *code = r;
            net_log("[tls] %s: handshake FALLO -0x%04X (red %d)", c->host, (unsigned)(-r), c->lastErr);
            mbed_err(err, errSize, "Fallo el cifrado con el servidor", r);
            if (offered)
                sess_forget(c->host);
            return 0;
        }
    }
    now = mbedtls_ssl_get_session_pointer(&c->ssl);
    net_log("[tls] %s: %s en %lu ms%s", c->host, mbedtls_ssl_get_ciphersuite(&c->ssl),
            (unsigned long)(GetTickCount() - t0),
            (offered && now && memcmp(now->master, oldMaster, 48) == 0) ? " (sesion reanudada)" : "");
    sess_save(c->host, &c->ssl);
    return 1;
}

static NetConn *conn_open(const char *host, int port, int tls, char *err, int errSize)
{
    NetConn *c;
    IN_ADDR ip;
    DWORD ms;
    int werr, attempt, code, first;
    char ipb[20];

    if (tls && !g_tlsReady)
    {
        set_err(err, errSize, "TLS no se pudo iniciar", 0);
        return NULL;
    }
    wait_for_ip();
    first = tls && host_wants_all(host) ? 1 : 0;
    for (attempt = first; attempt < 2; ++attempt)
    {
        if (!dns_cached(host, &ip, err, errSize))
            return NULL;
        c = (NetConn *)malloc(sizeof(NetConn));
        if (!c)
        {
            set_err(err, errSize, "Sin memoria", 0);
            return NULL;
        }
        ZeroMemory(c, sizeof(NetConn) - RBUF_SIZE);
        c->s = INVALID_SOCKET;
        c->tls = tls;
        c->port = port;
        strncpy(c->host, host, HOST_MAX - 1);
        c->host[HOST_MAX - 1] = 0;

        c->s = tcp_connect(ip, port, RECV_TIMEOUT_MS, &werr, &ms);
        if (c->s == INVALID_SOCKET && werr == WSAEINTR)
        {
            set_err(err, errSize, "Cancelado", 0);     /* (se pidio detener) */
            conn_close(c);
            return NULL;
        }
        if (c->s == INVALID_SOCKET)
        {
            net_log("[tcp] %s (%s:%d) FALLO error %d (%lu ms)", host, ip_str(ip, ipb), port, werr,
                    (unsigned long)ms);
            _snprintf(err, errSize - 1, "No se pudo conectar con %s (%d)", host, werr);
            err[errSize - 1] = 0;
            conn_close(c);
            dns_forget(host);       /* la proxima vez se vuelve a preguntar la IP */
            return NULL;
        }
        {
            /* recv espera de a medio segundo (para poder cancelar) */
            DWORD slice = RECV_SLICE_MS;
            setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&slice, sizeof(slice));
        }
        if (!tls)
            return c;
        /* Primero solo cifrados con certificado RSA (verificar RSA es lo mas
         * barato para el CPU); si el servidor no acepta, se ofrece todo. */
        if (tls_handshake(c, attempt, &code, err, errSize))
        {
            if (attempt == 1 && first == 0)
                host_set_all(host);
            return c;
        }
        conn_close(c);
        if (code != MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE || canceled())
            return NULL;
    }
    return NULL;
}

/* Lee mas datos al bufer. >0 bytes, 0 = el servidor cerro, <0 = error. */
static int conn_fill(NetConn *c)
{
    int n;

    c->rpos = 0;
    c->rlen = 0;
    if (c->tls)
    {
        do
            n = mbedtls_ssl_read(&c->ssl, c->rbuf, RBUF_SIZE);
        while ((n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) && !c->cancel);
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF)
            return 0;
        if (n < 0)
        {
            c->lastErr = n;
            return -1;
        }
    }
    else
    {
        do
            n = slice_recv(c, (char *)c->rbuf, RBUF_SIZE);
        while (n == -2);
        if (n == 0)
            return 0;
        if (n < 0)
            return -1;
    }
    c->rlen = n;
    c->gotAny = 1;
    return n;
}

static int conn_write(NetConn *c, const char *data, int len)
{
    int n;

    if (!c->tls)
        return send_all(c->s, data, len);
    while (len > 0)
    {
        n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data, (size_t)len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n <= 0)
        {
            c->lastErr = n;
            return 0;
        }
        data += n;
        len -= n;
    }
    return 1;
}

/* Una linea terminada en \n (sin \r\n). 1 = bien, 0 = cerro, -1 = error.
 * Si la linea no cabe se guarda solo el principio (YouTube manda
 * encabezados de varios KB, como Content-Security-Policy, que no usamos). */
static int conn_line(NetConn *c, char *out, int size)
{
    int len = 0, r, cut = 0;
    unsigned char ch;

    for (;;)
    {
        if (c->rpos >= c->rlen)
        {
            r = conn_fill(c);
            if (r <= 0)
                return r;
        }
        ch = c->rbuf[c->rpos++];
        if (ch == '\n')
            break;
        if (len < size - 1)
            out[len++] = (char)ch;
        else
            cut = 1;
    }
    if (len > 0 && out[len - 1] == '\r' && !cut)
        --len;
    out[len] = 0;
    return 1;
}

/* Exactamente n bytes. 1 = bien. */
static int conn_read(NetConn *c, char *dst, int n)
{
    int take;

    while (n > 0)
    {
        if (c->rpos >= c->rlen && conn_fill(c) <= 0)
            return 0;
        take = c->rlen - c->rpos;
        if (take > n)
            take = n;
        memcpy(dst, c->rbuf + c->rpos, take);
        c->rpos += take;
        dst += take;
        n -= take;
    }
    return 1;
}

/* --- Conexiones guardadas (keep-alive) --- */

static NetConn *g_pool[POOL_MAX];

/* Cierra las conexiones guardadas y olvida las sesiones TLS (al cambiar la
 * regla de verificacion) */
static void forget_connections(void)
{
    int i;

    if (!g_netReady)
        return;
    EnterCriticalSection(&g_poolCs);
    for (i = 0; i < POOL_MAX; ++i)
        if (g_pool[i])
        {
            conn_close(g_pool[i]);
            g_pool[i] = NULL;
        }
    LeaveCriticalSection(&g_poolCs);
    EnterCriticalSection(&g_sessCs);
    for (i = 0; i < SESS_SLOTS; ++i)
        if (g_sess[i].valid)
        {
            mbedtls_ssl_session_free(&g_sess[i].s);
            mbedtls_ssl_session_init(&g_sess[i].s);
            g_sess[i].valid = 0;
        }
    LeaveCriticalSection(&g_sessCs);
}

/* El servidor ya cerro (o mando algo inesperado) mientras estaba guardada? */
static int conn_dead(NetConn *c)
{
    fd_set rd;
    struct timeval tv;

    if (c->rpos < c->rlen)
        return 1;
    if (c->tls && mbedtls_ssl_get_bytes_avail(&c->ssl) > 0)
        return 1;
    FD_ZERO(&rd);
    FD_SET(c->s, &rd);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return select(0, &rd, NULL, NULL, &tv) != 0;   /* legible = cerro o error */
}

static NetConn *pool_take(const char *host, int port, int tls)
{
    NetConn *c, *found = NULL, *drop[POOL_MAX];
    DWORD now = GetTickCount();
    int i, nDrop = 0;

    EnterCriticalSection(&g_poolCs);
    for (i = 0; i < POOL_MAX; ++i)
    {
        c = g_pool[i];
        if (!c)
            continue;
        if (now - c->idleSince > IDLE_MAX_MS)
        {
            drop[nDrop++] = c;
            g_pool[i] = NULL;
        }
        else if (!found && c->port == port && c->tls == tls && strcmp(c->host, host) == 0)
        {
            found = c;
            g_pool[i] = NULL;
        }
    }
    LeaveCriticalSection(&g_poolCs);
    for (i = 0; i < nDrop; ++i)
        conn_close(drop[i]);
    if (found && conn_dead(found))
    {
        conn_close(found);
        found = NULL;
    }
    return found;
}

static void pool_put(NetConn *c)
{
    NetConn *old = NULL;
    int i, slot = -1;

    c->idleSince = GetTickCount();
    c->gotAny = 0;
    EnterCriticalSection(&g_poolCs);
    for (i = 0; i < POOL_MAX; ++i)
    {
        if (!g_pool[i])
        {
            slot = i;
            break;
        }
        if (slot < 0 || g_pool[i]->idleSince < g_pool[slot]->idleSince)
            slot = i;
    }
    old = g_pool[slot];
    g_pool[slot] = c;
    LeaveCriticalSection(&g_poolCs);
    conn_close(old);    /* (NULL si habia lugar) */
}

/* ------------------------------------------------------------------ */
/* HTTP/1.1                                                            */

typedef struct
{
    char scheme[8];
    char host[HOST_MAX];
    int  port;
    int  tls;
    const char *path;       /* dentro de la URL original, empieza con "/" o "?" o vacio */
} UrlParts;

static int url_parse(const char *url, UrlParts *u)
{
    const char *p = url, *h, *end;
    int len;

    ZeroMemory(u, sizeof(*u));
    if (ncase_eq(p, "https://", 8))
    {
        u->tls = 1;
        u->port = 443;
        p += 8;
    }
    else if (ncase_eq(p, "http://", 7))
    {
        u->port = 80;
        p += 7;
    }
    else
        return 0;
    h = p;
    while (*p && *p != '/' && *p != '?' && *p != ':' && *p != '#')
        ++p;
    len = (int)(p - h);
    if (len <= 0 || len >= HOST_MAX)
        return 0;
    memcpy(u->host, h, len);
    u->host[len] = 0;
    if (*p == ':')
    {
        u->port = atoi(p + 1);
        while (*p && *p != '/' && *p != '?')
            ++p;
        if (u->port <= 0 || u->port > 65535)
            return 0;
    }
    end = p;
    u->path = end;
    return 1;
}

typedef struct
{
    int  status;
    int  http10;
    int  chunked;
    int  hasLength;
    double length;
    int  closeHdr;
    int  keepAliveHdr;
    char location[2048];
} RespHead;

static void parse_header_line(const char *line, RespHead *h)
{
    const char *v = strchr(line, ':');
    int n;

    if (!v)
        return;
    n = (int)(v - line);
    ++v;
    while (*v == ' ' || *v == '\t')
        ++v;
    if (n == 14 && ncase_eq(line, "content-length", 14))
    {
        h->hasLength = 1;
        h->length = atof(v);
    }
    else if (n == 17 && ncase_eq(line, "transfer-encoding", 17))
    {
        if (ncase_find(v, "chunked"))
            h->chunked = 1;
    }
    else if (n == 10 && ncase_eq(line, "connection", 10))
    {
        if (ncase_find(v, "close"))
            h->closeHdr = 1;
        if (ncase_find(v, "keep-alive"))
            h->keepAliveHdr = 1;
    }
    else if (n == 8 && ncase_eq(line, "location", 8))
    {
        strncpy(h->location, v, sizeof(h->location) - 1);
        h->location[sizeof(h->location) - 1] = 0;
    }
}

/* Cuerpo que crece segun llega */
typedef struct
{
    char *buf;
    int   len, cap;
} Body;

static int body_reserve(Body *b, int extra)
{
    int need = b->len + extra, cap;
    char *tmp;

    if (need <= b->cap)
        return 1;
    if (need > MAX_RESPONSE)
        return 0;
    cap = b->cap ? b->cap : 64 * 1024;
    while (cap < need)
        cap *= 2;
    if (cap > MAX_RESPONSE)
        cap = MAX_RESPONSE;
    tmp = (char *)realloc(b->buf, cap + 1);
    if (!tmp)
        return 0;
    b->buf = tmp;
    b->cap = cap;
    return 1;
}

static int body_read(NetConn *c, Body *b, int n)
{
    if (!body_reserve(b, n))
        return 0;
    if (!conn_read(c, b->buf + b->len, n))
        return 0;
    b->len += n;
    return 1;
}

#define RR_OK      1
#define RR_NET     0       /* error de red (se puede reintentar si no llego nada) */
#define RR_BAD    -1       /* respuesta rara o demasiado grande */

/* Lee una respuesta completa. *keep = la conexion se puede reutilizar. */
static int read_response(NetConn *c, int noBody, RespHead *h, Body *b, int *keep, char *err, int errSize)
{
    char line[4096];
    int r, total, n;
    double left;
    unsigned long chunk;

    *keep = 0;
    for (;;)
    {
        ZeroMemory(h, sizeof(*h));
        r = conn_line(c, line, sizeof(line));
        if (r <= 0)
        {
            if (c->cancel)
                set_err(err, errSize, "Cancelado", 0);
            else
                set_err(err, errSize, r < 0 && c->lastErr ? "Error o tiempo agotado al recibir" :
                        "El servidor cerro la conexion", c->lastErr);
            return c->cancel ? RR_BAD : RR_NET;
        }
        if (strncmp(line, "HTTP/", 5) != 0 || !strchr(line, ' '))
        {
            set_err(err, errSize, "Respuesta invalida del servidor", 0);
            return RR_BAD;
        }
        h->http10 = strncmp(line, "HTTP/1.0", 8) == 0;
        h->status = atoi(strchr(line, ' ') + 1);
        total = 0;
        for (;;)
        {
            r = conn_line(c, line, sizeof(line));
            if (r <= 0)
            {
                set_err(err, errSize, "Se corto la respuesta (encabezados)", c->lastErr);
                return r == 0 ? RR_NET : RR_BAD;
            }
            if (!line[0])
                break;
            total += (int)strlen(line);
            if (total > MAX_HEADER * 4)
            {
                set_err(err, errSize, "Encabezados demasiado grandes", 0);
                return RR_BAD;
            }
            parse_header_line(line, h);
        }
        if (h->status != 100)
            break;          /* "100 Continue": viene otra respuesta detras */
    }

    if (noBody || (h->status >= 100 && h->status < 200) || h->status == 204 || h->status == 304)
    {
        /* sin cuerpo */
    }
    else if (h->chunked)
    {
        for (;;)
        {
            if (conn_line(c, line, sizeof(line)) <= 0)
            {
                set_err(err, errSize, "Se corto la respuesta (chunked)", c->lastErr);
                return RR_BAD;
            }
            chunk = strtoul(line, NULL, 16);
            if (chunk == 0)
            {
                /* encabezados finales opcionales, hasta la linea vacia */
                do
                    r = conn_line(c, line, sizeof(line));
                while (r > 0 && line[0]);
                if (r <= 0)
                {
                    set_err(err, errSize, "Se corto la respuesta (fin chunked)", c->lastErr);
                    return RR_BAD;
                }
                break;
            }
            if (chunk > (unsigned long)MAX_RESPONSE || !body_read(c, b, (int)chunk))
            {
                set_err(err, errSize, b->len + (double)chunk > MAX_RESPONSE ? "Respuesta demasiado grande" :
                        "Se corto la respuesta", c->lastErr);
                return RR_BAD;
            }
            if (conn_line(c, line, sizeof(line)) <= 0 || line[0])
            {
                set_err(err, errSize, "Respuesta chunked invalida", 0);
                return RR_BAD;
            }
        }
    }
    else if (h->hasLength)
    {
        left = h->length;
        if (left > MAX_RESPONSE)
        {
            set_err(err, errSize, "Respuesta demasiado grande", 0);
            return RR_BAD;
        }
        if (!body_reserve(b, (int)left))
        {
            set_err(err, errSize, "Sin memoria", 0);
            return RR_BAD;
        }
        if (left > 0 && !body_read(c, b, (int)left))
        {
            set_err(err, errSize, "Se corto la respuesta", c->lastErr);
            return RR_BAD;
        }
    }
    else
    {
        /* sin longitud: hasta que el servidor cierre */
        for (;;)
        {
            if (c->rpos >= c->rlen)
            {
                r = conn_fill(c);
                if (r == 0)
                    break;
                if (r < 0)
                {
                    set_err(err, errSize, "Error o tiempo agotado al recibir", c->lastErr);
                    return RR_BAD;
                }
            }
            n = c->rlen - c->rpos;
            if (!body_reserve(b, n))
            {
                set_err(err, errSize, "Respuesta demasiado grande", 0);
                return RR_BAD;
            }
            memcpy(b->buf + b->len, c->rbuf + c->rpos, n);
            b->len += n;
            c->rpos = c->rlen;
        }
        return RR_OK;       /* (keep = 0) */
    }

    *keep = !h->closeHdr && (!h->http10 || h->keepAliveHdr);
    return RR_OK;
}

/* Arma la peticion completa (encabezados + cuerpo) en un solo bufer */
static char *build_request(const char *method, const UrlParts *u, const char *extra, const char *body,
                           int bodyLen, int *outLen)
{
    int cap = (int)strlen(method) + (int)strlen(u->path) + (extra ? (int)strlen(extra) : 0) + bodyLen + 512;
    char *req = (char *)malloc(cap);
    int n;

    if (!req)
        return NULL;
    n = _snprintf(req, cap - 1, "%s %s%s HTTP/1.1\r\nHost: %s", method, u->path[0] == '/' ? "" : "/", u->path,
                  u->host);
    if ((u->tls && u->port != 443) || (!u->tls && u->port != 80))
        n += _snprintf(req + n, cap - 1 - n, ":%d", u->port);
    n += _snprintf(req + n, cap - 1 - n, "\r\n");
    if (!ncase_find(extra, "User-Agent:"))
        n += _snprintf(req + n, cap - 1 - n, "User-Agent: Mozilla/5.0 (TubeBox360)\r\n");
    if (!ncase_find(extra, "Accept:"))
        n += _snprintf(req + n, cap - 1 - n, "Accept: */*\r\n");
    if (extra)
        n += _snprintf(req + n, cap - 1 - n, "%s", extra);
    if (bodyLen > 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0)
        n += _snprintf(req + n, cap - 1 - n, "Content-Length: %d\r\n", bodyLen > 0 ? bodyLen : 0);
    n += _snprintf(req + n, cap - 1 - n, "\r\n");
    if (body && bodyLen > 0)
    {
        memcpy(req + n, body, bodyLen);
        n += bodyLen;
    }
    *outLen = n;
    return req;
}

/* Nueva URL de una redireccion (absoluta, "//host/...", o "/ruta") */
static char *redirect_url(const UrlParts *u, const char *loc)
{
    char *out = (char *)malloc(strlen(loc) + HOST_MAX + 32);
    const char *q;

    if (!out)
        return NULL;
    if (ncase_eq(loc, "http://", 7) || ncase_eq(loc, "https://", 8))
        strcpy(out, loc);
    else if (loc[0] == '/' && loc[1] == '/')
        sprintf(out, "%s:%s", u->tls ? "https" : "http", loc);
    else if (loc[0] == '/')
    {
        if ((u->tls && u->port != 443) || (!u->tls && u->port != 80))
            sprintf(out, "%s://%s:%d%s", u->tls ? "https" : "http", u->host, u->port, loc);
        else
            sprintf(out, "%s://%s%s", u->tls ? "https" : "http", u->host, loc);
    }
    else
    {
        /* relativa a la carpeta actual (casi nunca pasa) */
        q = strrchr(u->path, '/');
        if (q)
            sprintf(out, "%s://%s%.*s%s", u->tls ? "https" : "http", u->host, (int)(q - u->path) + 1, u->path, loc);
        else
            sprintf(out, "%s://%s/%s", u->tls ? "https" : "http", u->host, loc);
    }
    return out;
}

/* direct_request: hace la peticion siguiendo redirecciones. Si hubo
 * redireccion, *finalUrl (malloc) = la URL donde termino; NULL si no. */
static int direct_request(const char *method, const char *url, const char *extraHeaders, const char *body,
                          int bodyLen, HttpResponse *resp, char *err, int errSize, char **finalUrl)
{
    char *curUrl = NULL, *req = NULL, *next;
    char curMethod[16];
    const char *curBody = body;
    int curBodyLen = bodyLen;
    UrlParts u;
    RespHead *h = NULL;
    Body b;
    NetConn *c;
    int redir, attempt, reqLen, keep, rr, reused, ok = 0, noBody;

    b.buf = NULL;
    b.len = b.cap = 0;
    strncpy(curMethod, method, sizeof(curMethod) - 1);
    curMethod[sizeof(curMethod) - 1] = 0;
    curUrl = (char *)malloc(strlen(url) + 1);
    h = (RespHead *)malloc(sizeof(RespHead));
    if (!curUrl || !h)
    {
        set_err(err, errSize, "Sin memoria", 0);
        goto done;
    }
    strcpy(curUrl, url);

    for (redir = 0; ; ++redir)
    {
        if (!url_parse(curUrl, &u))
        {
            set_err(err, errSize, "URL invalida", 0);
            goto done;
        }
        free(req);
        req = build_request(curMethod, &u, extraHeaders, curBody, curBodyLen, &reqLen);
        if (!req)
        {
            set_err(err, errSize, "Sin memoria", 0);
            goto done;
        }
        noBody = strcmp(curMethod, "HEAD") == 0;

        for (attempt = 0; ; ++attempt)
        {
            if (canceled())
            {
                set_err(err, errSize, "Cancelado", 0);
                goto done;
            }
            c = attempt == 0 ? pool_take(u.host, u.port, u.tls) : NULL;
            reused = c != NULL;
            if (!c)
            {
                c = conn_open(u.host, u.port, u.tls, err, errSize);
                if (!c)
                    goto done;
            }
            c->gotAny = 0;
            b.len = 0;
            if (conn_write(c, req, reqLen))
            {
                rr = read_response(c, noBody, h, &b, &keep, err, errSize);
                if (rr == RR_OK)
                    break;
            }
            else
            {
                rr = RR_NET;
                set_err(err, errSize, "Error enviando la peticion", c->lastErr);
            }
            /* Una conexion guardada que el servidor ya habia cerrado: se
             * repite UNA vez con una nueva (solo si no llego nada). */
            reused = reused && !c->gotAny && rr == RR_NET && !c->cancel;
            if (c->cancel)
                set_err(err, errSize, "Cancelado", 0);
            conn_close(c);
            if (!reused || attempt > 0)
            {
                if (!canceled())
                    net_log("[http] %s %.90s -> %s", curMethod, curUrl, err);
                goto done;
            }
        }
        if (keep)
            pool_put(c);
        else
            conn_close(c);

        if ((h->status == 301 || h->status == 302 || h->status == 303 || h->status == 307 || h->status == 308) &&
            h->location[0])
        {
            if (redir >= MAX_REDIRECTS)
            {
                set_err(err, errSize, "Demasiadas redirecciones", 0);
                goto done;
            }
            next = redirect_url(&u, h->location);
            if (!next)
            {
                set_err(err, errSize, "Sin memoria", 0);
                goto done;
            }
            net_log("[http] %d -> %.100s", h->status, next);
            free(curUrl);
            curUrl = next;
            if (h->status == 303 || ((h->status == 301 || h->status == 302) && strcmp(curMethod, "POST") == 0))
            {
                strcpy(curMethod, "GET");
                curBody = NULL;
                curBodyLen = 0;
            }
            continue;
        }
        break;
    }

    if (!b.buf && !body_reserve(&b, 1))
    {
        set_err(err, errSize, "Sin memoria", 0);
        goto done;
    }
    b.buf[b.len] = 0;
    resp->status = h->status;
    resp->data = b.buf;
    resp->body = b.buf;
    resp->bodyLen = b.len;
    b.buf = NULL;           /* ahora es de resp */
    ok = 1;
    if (finalUrl && redir > 0)
    {
        *finalUrl = curUrl;     /* (es de quien llamo) */
        curUrl = NULL;
    }

done:
    free(b.buf);
    free(req);
    free(curUrl);
    free(h);
    return ok;
}

/* ---- Cache de redirecciones ----
 * googlevideo contesta casi cada pedazo de video con un 302 hacia otro
 * servidor (rr4 -> rr6 -> rr3...), y ese 302 viene con "Connection: close":
 * cada pedazo costaba una conexion nueva y 1-2 viajes de mas. Como la URL
 * es la misma para todos los pedazos (solo cambia el Range), se recuerda a
 * donde termino y se va directo. Si el destino falla o contesta 4xx/5xx, se
 * olvida y se repite con la URL original. */
#define REDIR_SLOTS   16
#define REDIR_TTL_MS  (10 * 60 * 1000)

typedef struct
{
    char *from;
    char *to;
    DWORD when;
} RedirSlot;

static RedirSlot g_redir[REDIR_SLOTS];

static char *redir_lookup(const char *url)
{
    char *out = NULL;
    DWORD now = GetTickCount();
    int i;

    EnterCriticalSection(&g_poolCs);
    for (i = 0; i < REDIR_SLOTS; ++i)
    {
        if (g_redir[i].from && now - g_redir[i].when < REDIR_TTL_MS && strcmp(g_redir[i].from, url) == 0)
        {
            out = (char *)malloc(strlen(g_redir[i].to) + 1);
            if (out)
                strcpy(out, g_redir[i].to);
            break;
        }
    }
    LeaveCriticalSection(&g_poolCs);
    return out;
}

static void redir_forget(const char *url)
{
    int i;

    EnterCriticalSection(&g_poolCs);
    for (i = 0; i < REDIR_SLOTS; ++i)
    {
        if (g_redir[i].from && strcmp(g_redir[i].from, url) == 0)
        {
            free(g_redir[i].from);
            free(g_redir[i].to);
            g_redir[i].from = g_redir[i].to = NULL;
        }
    }
    LeaveCriticalSection(&g_poolCs);
}

/* Guarda from -> to (to pasa a ser del cache) */
static void redir_store(const char *url, char *to)
{
    int i, slot = -1, oldest = 0;
    char *from = (char *)malloc(strlen(url) + 1);

    if (!from)
    {
        free(to);
        return;
    }
    strcpy(from, url);
    EnterCriticalSection(&g_poolCs);
    for (i = 0; i < REDIR_SLOTS; ++i)
    {
        if (g_redir[i].from && strcmp(g_redir[i].from, url) == 0)
        {
            slot = i;
            break;
        }
        if (!g_redir[i].from && slot < 0)
            slot = i;
        if (g_redir[i].when < g_redir[oldest].when)
            oldest = i;
    }
    if (slot < 0)
        slot = oldest;
    free(g_redir[slot].from);
    free(g_redir[slot].to);
    g_redir[slot].from = from;
    g_redir[slot].to = to;
    g_redir[slot].when = GetTickCount();
    LeaveCriticalSection(&g_poolCs);
}

static int cached_request(const char *method, const char *url, const char *extraHeaders, const char *body,
                          int bodyLen, HttpResponse *resp, char *err, int errSize)
{
    char *to = NULL, *final = NULL;
    int ok, isGet = strcmp(method, "GET") == 0;

    if (isGet)
        to = redir_lookup(url);
    if (to)
    {
        ok = direct_request(method, to, extraHeaders, body, bodyLen, resp, err, errSize, &final);
        free(to);
        free(final);        /* (si el destino volvio a redirigir, la proxima se sigue de nuevo) */
        if (ok && resp->status < 400)
            return 1;
        if (canceled())
            return ok;
        if (ok)
            http_free(resp);
        redir_forget(url);  /* el destino ya no sirve: desde la original */
        net_log("[http] la redireccion guardada ya no sirve; se repite desde la original");
    }
    ok = direct_request(method, url, extraHeaders, body, bodyLen, resp, err, errSize, isGet ? &final : NULL);
    if (ok && final && resp->status >= 200 && resp->status < 300)
        redir_store(url, final);
    else
        free(final);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Modo proxy (fase 1, la PC con ProtoHTTPS)                           */

static int proxy_request(const char *method, const char *url, const char *extraHeaders,
                         const char *body, int bodyLen, HttpResponse *resp,
                         char *err, int errSize)
{
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in addr;
    BOOL yes = TRUE;
    DWORD timeout = RECV_TIMEOUT_MS;
    char *head = NULL;
    char *buf = NULL;
    char *tmp;
    char *sep;
    int headLen, cap, size, n;
    int ok = 0;

    /* 1. Armar los encabezados de la peticion */
    headLen = (int)strlen(method) + (int)strlen(url) +
              (extraHeaders ? (int)strlen(extraHeaders) : 0) + 256;
    head = (char *)malloc(headLen);
    if (!head)
    {
        set_err(err, errSize, "Sin memoria", 0);
        goto done;
    }
    headLen = _snprintf(head, headLen - 1,
                        "%s /%s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Connection: close\r\n"
                        "%s",
                        method, url, g_proxyIp, g_proxyPort,
                        extraHeaders ? extraHeaders : "");
    if (body && bodyLen > 0)
        headLen += _snprintf(head + headLen, 64, "Content-Length: %d\r\n", bodyLen);
    strcpy(head + headLen, "\r\n");
    headLen += 2;

    /* 2. Conectar con el proxy */
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        set_err(err, errSize, "No se pudo crear el socket", WSAGetLastError());
        goto done;
    }
    setsockopt(s, SOL_SOCKET, SO_MARKINSECURE, (const char *)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)g_proxyPort);
    addr.sin_addr.s_addr = inet_addr(g_proxyIp);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        set_err(err, errSize, "No se pudo conectar al proxy", WSAGetLastError());
        goto done;
    }

    /* 3. Enviar */
    if (!send_all(s, head, headLen) ||
        (body && bodyLen > 0 && !send_all(s, body, bodyLen)))
    {
        set_err(err, errSize, "Error enviando la peticion", WSAGetLastError());
        goto done;
    }

    /* 4. Recibir todo hasta que el proxy cierre la conexion */
    cap = 64 * 1024;
    size = 0;
    buf = (char *)malloc(cap + 1);
    if (!buf)
    {
        set_err(err, errSize, "Sin memoria", 0);
        goto done;
    }
    for (;;)
    {
        if (size == cap)
        {
            if (cap >= MAX_RESPONSE)
            {
                set_err(err, errSize, "Respuesta demasiado grande", 0);
                goto done;
            }
            cap *= 2;
            tmp = (char *)realloc(buf, cap + 1);
            if (!tmp)
            {
                set_err(err, errSize, "Sin memoria", 0);
                goto done;
            }
            buf = tmp;
        }
        n = recv(s, buf + size, cap - size, 0);
        if (n == 0)
            break;          /* el proxy cerro: respuesta completa */
        if (n < 0)
        {
            set_err(err, errSize, "Error o tiempo agotado al recibir", WSAGetLastError());
            goto done;
        }
        size += n;
    }
    buf[size] = 0;

    /* 5. Separar estado, encabezados y cuerpo */
    sep = strstr(buf, "\r\n\r\n");
    if (strncmp(buf, "HTTP/", 5) != 0 || !sep || !strchr(buf, ' '))
    {
        set_err(err, errSize, "Respuesta invalida del proxy", 0);
        goto done;
    }
    resp->status  = atoi(strchr(buf, ' ') + 1);
    resp->data    = buf;
    resp->body    = sep + 4;
    resp->bodyLen = size - (int)(resp->body - buf);
    buf = NULL;     /* ahora es de resp */
    ok = 1;

done:
    if (s != INVALID_SOCKET)
        closesocket(s);
    free(head);
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */

int http_request(const char *method, const char *url, const char *extraHeaders,
                 const char *body, int bodyLen, HttpResponse *resp,
                 char *err, int errSize)
{
    ZeroMemory(resp, sizeof(*resp));
    if (err && errSize > 0)
        err[0] = 0;
    if (!g_netReady)
    {
        set_err(err, errSize, "La red no esta lista", 0);
        return 0;
    }
    if (g_direct)
        return cached_request(method, url, extraHeaders, body, bodyLen, resp, err, errSize);
    return proxy_request(method, url, extraHeaders, body, bodyLen, resp, err, errSize);
}

void http_free(HttpResponse *resp)
{
    if (!resp)
        return;
    free(resp->data);
    ZeroMemory(resp, sizeof(*resp));
}

/* ------------------------------------------------------------------ */
/* Prueba de red sin la PC                                             */

static void rep_add(char *out, int size, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int len = (int)strlen(out);

    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    net_log("[prueba] %s", line);
    if (len + (int)strlen(line) + 2 < size)
    {
        strcat(out, line);
        strcat(out, "\n");
    }
}

int net_selftest(char *out, int size)
{
    XNADDR xna;
    DWORD link, flags, ms, t0;
    IN_ADDR ip;
    SOCKET s;
    HttpResponse r;
    int st, werr, fails = 0, n, i, status, saved;
    char buf[512], ipb[20], err[200];
    BYTE rnd[16];
    const char *req = "GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\n"
                      "Connection: close\r\n\r\n";

    out[0] = 0;
    net_log("---- prueba de red sin la PC ----");

    /* 1. cable/wifi e IP de la consola */
    link = XNetGetEthernetLinkStatus();
    ZeroMemory(&xna, sizeof(xna));
    flags = XNetGetTitleXnAddr(&xna);
    if ((link & XNET_ETHERNET_LINK_ACTIVE) && flags != XNET_GET_XNADDR_PENDING && !(flags & XNET_GET_XNADDR_NONE))
        rep_add(out, size, "1. Red: OK  (IP de la consola %s)", ip_str(xna.ina, ipb));
    else
    {
        rep_add(out, size, "1. Red: FALLO  (enlace %08lX, direccion %08lX)", (unsigned long)link,
                (unsigned long)flags);
        ++fails;
    }

    /* 2. DNS */
    if (dns_lookup("www.youtube.com", &ip, &st, &ms))
        rep_add(out, size, "2. DNS www.youtube.com: OK  %s  (%lu ms)", ip_str(ip, ipb), (unsigned long)ms);
    else
    {
        rep_add(out, size, "2. DNS www.youtube.com: FALLO  (estado %d, %lu ms)", st, (unsigned long)ms);
        ++fails;
    }

    /* 3. TCP al puerto 443 */
    if (st == 0)
    {
        s = tcp_connect(ip, 443, CONNECT_TIMEOUT, &werr, &ms);
        if (s != INVALID_SOCKET)
        {
            rep_add(out, size, "3. TCP youtube:443: OK  (%lu ms)", (unsigned long)ms);
            closesocket(s);
        }
        else
        {
            rep_add(out, size, "3. TCP youtube:443: FALLO  (error %d, %lu ms)", werr, (unsigned long)ms);
            ++fails;
        }
    }
    else
    {
        rep_add(out, size, "3. TCP youtube:443: no se probo (sin DNS)");
        ++fails;
    }

    /* 4. HTTP simple (sin cifrado) a internet */
    if (dns_lookup("connectivitycheck.gstatic.com", &ip, &st, &ms))
    {
        s = tcp_connect(ip, 80, CONNECT_TIMEOUT, &werr, &ms);
        if (s != INVALID_SOCKET)
        {
            status = 0;
            if (send(s, req, (int)strlen(req), 0) > 0)
            {
                n = recv(s, buf, sizeof(buf) - 1, 0);
                if (n > 12)
                {
                    buf[n] = 0;
                    status = atoi(buf + 9);
                }
            }
            closesocket(s);
            if (status == 204)
                rep_add(out, size, "4. HTTP a internet: OK  (204)");
            else
            {
                rep_add(out, size, "4. HTTP a internet: FALLO  (respuesta %d)", status);
                ++fails;
            }
        }
        else
        {
            rep_add(out, size, "4. HTTP a internet: FALLO  (TCP error %d)", werr);
            ++fails;
        }
    }
    else
    {
        rep_add(out, size, "4. HTTP a internet: FALLO  (DNS estado %d)", st);
        ++fails;
    }

    /* 5. numeros aleatorios del hardware (para las llaves del cifrado) */
    ZeroMemory(rnd, sizeof(rnd));
    XNetRandom(rnd, sizeof(rnd));
    for (i = 0, n = 0; i < (int)sizeof(rnd); ++i)
        n |= rnd[i];
    if (n)
        rep_add(out, size, "5. Aleatorios: OK  (%02X%02X%02X%02X...)", rnd[0], rnd[1], rnd[2], rnd[3]);
    else
    {
        rep_add(out, size, "5. Aleatorios: FALLO");
        ++fails;
    }

    /* 6 y 7 usan el HTTPS directo aunque config.ini diga proxy */
    saved = g_direct;
    g_direct = 1;

    /* 6. HTTPS (TLS con mbedTLS) a YouTube, conexion nueva */
    sess_forget("www.youtube.com");
    t0 = GetTickCount();
    if (http_request("GET", "https://www.youtube.com/generate_204", NULL, NULL, 0, &r, err, sizeof(err)))
    {
        if (r.status == 204)
            rep_add(out, size, "6. HTTPS YouTube: OK  (%lu ms con el cifrado; certificado %s)",
                    (unsigned long)(GetTickCount() - t0),
                    net_verify_on() ? "verificado" : "SIN verificar");
        else
        {
            rep_add(out, size, "6. HTTPS YouTube: respuesta %d  (%lu ms)", r.status,
                    (unsigned long)(GetTickCount() - t0));
            ++fails;
        }
        http_free(&r);
    }
    else
    {
        rep_add(out, size, "6. HTTPS YouTube: FALLO  %s", err);
        ++fails;
    }
    rep_add(out, size, "   Raices de confianza: %d de %d (Google Trust Services); verificar: %s",
            g_rootsLoaded, g_tlsRootCount, g_tlsVerify ? "si" : "no (certificados=0)");

    /* 7. Descarga de prueba (pagina de inicio de YouTube, ~0.5-1 MB, chunked) */
    t0 = GetTickCount();
    if (http_request("GET", "https://www.youtube.com/", NULL, NULL, 0, &r, err, sizeof(err)))
    {
        ms = GetTickCount() - t0;
        if (ms == 0)
            ms = 1;
        if (r.status == 200 && r.bodyLen > 0)
            rep_add(out, size, "7. Descarga: OK  %d KB en %lu ms = %lu KB/s", r.bodyLen / 1024, (unsigned long)ms,
                    (unsigned long)((double)r.bodyLen / 1.024 / ms));
        else
        {
            rep_add(out, size, "7. Descarga: respuesta %d (%d bytes)", r.status, r.bodyLen);
            ++fails;
        }
        http_free(&r);
    }
    else
    {
        rep_add(out, size, "7. Descarga: FALLO  %s", err);
        ++fails;
    }
    g_direct = saved;

    rep_add(out, size, "Modo actual: %s", g_direct ? "HTTPS directo (sin la PC)" : "proxy de la PC (config.ini)");
    rep_add(out, size, fails ? "Resultado: %d paso(s) fallaron" : "Resultado: todo bien", fails);
    return fails;
}
