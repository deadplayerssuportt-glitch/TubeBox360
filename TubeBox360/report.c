/* report.c - Registro de errores y reporte para los probadores. C89.
 * Ver report.h. */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "report.h"
#include "version.h"
#include "net.h"
#include "cuenta.h"
#include "lang.h"

#define ERRORS_PATH   "game:\\errores.txt"
#define NETLOG_PATH   "game:\\tubebox_red.txt"
#define DIAG_PATH     "game:\\diagnostico.txt"
#define CONFIG_PATH   "game:\\config.ini"

static CRITICAL_SECTION g_cs;
static int g_init = 0;

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

void report_init(void)
{
    SYSTEMTIME st;
    FILE *f;

    if (!g_init)
    {
        InitializeCriticalSection(&g_cs);
        g_init = 1;
    }
    /* las bitacoras no crecen para siempre */
    if (file_size(ERRORS_PATH) > 256 * 1024)
        remove(ERRORS_PATH);
    if (file_size(DIAG_PATH) > 1024 * 1024)
        remove(DIAG_PATH);

    GetLocalTime(&st);
    f = fopen(ERRORS_PATH, "ab");
    if (f)
    {
        fprintf(f, "---- %04d-%02d-%02d %02d:%02d:%02d  TubeBox360 %s: inicio ----\r\n", st.wYear, st.wMonth,
                st.wDay, st.wHour, st.wMinute, st.wSecond, TB_VERSION);
        fclose(f);
    }
}

void report_error(const char *where, const char *fmt, ...)
{
    char msg[400];
    SYSTEMTIME st;
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = 0;

    net_log("[error] %s: %s", where, msg);
    if (!g_init)
        return;
    GetLocalTime(&st);
    EnterCriticalSection(&g_cs);
    f = fopen(ERRORS_PATH, "ab");
    if (f)
    {
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond, where, msg);
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

/* Copia las ultimas 'lines' lineas de un archivo */
static void copy_tail(FILE *out, const char *title, const char *path, int lines)
{
    FILE *f = fopen(path, "rb");
    long size, start, pos;
    int n = 0, c;
    char *buf;
    long len, i;

    fprintf(out, "\r\n===== %s (%s, ultimas %d lineas) =====\r\n", title, path, lines);
    if (!f)
    {
        fputs("(no existe)\r\n", out);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    /* buscar hacia atras desde el final, leyendo hasta 64 KB */
    start = size > 64 * 1024 ? size - 64 * 1024 : 0;
    len = size - start;
    buf = (char *)malloc(len + 1);
    if (!buf)
    {
        fclose(f);
        fputs("(sin memoria)\r\n", out);
        return;
    }
    fseek(f, start, SEEK_SET);
    len = (long)fread(buf, 1, len, f);
    fclose(f);
    buf[len] = 0;
    pos = 0;
    for (i = len - 1; i >= 0; --i)
    {
        c = buf[i];
        if (c == '\n' && i != len - 1 && ++n >= lines)
        {
            pos = i + 1;
            break;
        }
    }
    fwrite(buf + pos, 1, len - pos, out);
    if (len > 0 && buf[len - 1] != '\n')
        fputs("\r\n", out);
    free(buf);
}

int report_write(const char *selftest)
{
    FILE *out = fopen(REPORT_PATH, "wb");
    SYSTEMTIME st;
    MEMORYSTATUS ms;
    int port;

    if (!out)
        return 0;
    GetLocalTime(&st);
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);

    fprintf(out, "TubeBox360 - reporte de errores\r\n");
    fprintf(out, "Mandalo a: deadplayers.suportt@gmail.com (junto con game:\\errores.txt)\r\n");
    fprintf(out, "Version: %s (compilada %s %s)\r\n", TB_VERSION, __DATE__, __TIME__);
    fprintf(out, "Fecha de la consola: %04d-%02d-%02d %02d:%02d:%02d\r\n", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond);
    fprintf(out, "Memoria libre: %lu KB de %lu KB\r\n", (unsigned long)(ms.dwAvailPhys / 1024),
            (unsigned long)(ms.dwTotalPhys / 1024));
    fprintf(out, "Region: %s (YouTube gl=%s, hl=%s)\r\n", lang_region_code(lang_region()), lang_gl(), lang_hl());
    fprintf(out, "Certificados HTTPS: %s\r\n", net_verify_on() ? "verificados" : "SIN verificar");
    /* (sin nombre ni datos de la cuenta: solo si hay sesion) */
    fprintf(out, "Cuenta de Google: %s\r\n", cuenta_signed_in() ? "con sesion" :
            cuenta_state() == CUENTA_NOCLIENT ? "no configurada" : "sin sesion");
    fprintf(out, "Conexion: %s", net_is_direct() ? "HTTPS directo" : "proxy de la PC");
    if (!net_is_direct())
    {
        const char *ip = net_get_proxy(&port);   /* (primero, para que port ya tenga valor) */
        fprintf(out, " (%s:%d)", ip, port);
    }
    fputs("\r\n", out);
    fputs("\r\nDescribe aqui que estabas haciendo cuando fallo:\r\n\r\n\r\n", out);

    if (selftest && selftest[0])
    {
        const char *p;
        fputs("\r\n===== Prueba de red =====\r\n", out);
        for (p = selftest; *p; ++p)
        {
            if (*p == '\n')
                fputc('\r', out);
            fputc(*p, out);
        }
    }
    copy_tail(out, "config.ini", CONFIG_PATH, 40);
    copy_tail(out, "Errores", ERRORS_PATH, 80);
    copy_tail(out, "Red", NETLOG_PATH, 200);
    copy_tail(out, "Reproductor", DIAG_PATH, 150);
    fclose(out);
    net_log("[reporte] guardado en %s", REPORT_PATH);
    return 1;
}
