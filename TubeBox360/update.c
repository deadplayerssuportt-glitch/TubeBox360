/* update.c - Aviso y descarga de version nueva. Ver update.h. C89.
 *   GET {base}/version.json
 *     -> {"build":101,"v":"1.01","notas":"...","xex":"<Drive>","size":123,"url":"..."}
 *   Descarga: pedazos de 1 MB con "Range" (se puede cancelar y se ve el %),
 *   a game:\Actualizacion\default.xex.part; al terminar se revisa que empiece
 *   con "XEX2" y se renombra a default.xex. */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update.h"
#include "version.h"
#include "net.h"
#include "json.h"
#include "lang.h"

#ifndef TB_UPDATE_URL
#define TB_UPDATE_URL TB_PRESENCE_URL   /* misma base de Firebase */
#endif
#define CHUNK     (1024.0 * 1024.0)
#define PART_FILE UPD_FILE ".part"

static HANDLE         g_thread = NULL;
static volatile long  g_quit = 0;
static volatile LONG  g_avail = 0;
static volatile LONG  g_checked = 0;
static char           g_ver[32];
static char           g_notes[400];
static char           g_url[200];
static char           g_xex[400];
static double         g_size = 0.0;

static HANDLE         g_dlThread = NULL;
static volatile LONG  g_dlState = UPD_DL_IDLE;
static volatile long  g_dlCancel = 0;
static volatile double g_dlDone = 0.0;
static char           g_dlErr[200];

static void copy(char *dst, int size, const char *src)
{
    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = 0;
}

/* Enlace de Drive compartido -> descarga directa.
 *   https://drive.google.com/file/d/ID/view?usp=sharing
 *   https://drive.google.com/open?id=ID   /  ...uc?id=ID&export=download
 * Cualquier otro enlace se usa tal cual. */
static void direct_link(const char *src, char *out, int size)
{
    const char *p = NULL;
    char id[128];
    int n = 0;

    if (strstr(src, "drive.google.com") || strstr(src, "drive.usercontent.google.com"))
    {
        if ((p = strstr(src, "/d/")) != NULL)
            p += 3;
        else if ((p = strstr(src, "id=")) != NULL)
            p += 3;
    }
    if (!p)
    {
        copy(out, size, src);
        return;
    }
    while (p[n] && p[n] != '/' && p[n] != '?' && p[n] != '&' && n < (int)sizeof(id) - 1)
    {
        id[n] = p[n];
        ++n;
    }
    id[n] = 0;
    _snprintf(out, size - 1, "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t", id);
    out[size - 1] = 0;
}

/* 1 = respuesta valida (haya o no version nueva) */
static int check_once(void)
{
    char url[256], err[160];
    HttpResponse r;
    JsonDoc *doc;
    int ok = 0;

    _snprintf(url, sizeof(url) - 1, "%s/version.json", TB_UPDATE_URL);
    url[sizeof(url) - 1] = 0;
    if (!http_request("GET", url, NULL, NULL, 0, &r, err, sizeof(err)))
        return 0;
    if (r.status == 200)
    {
        ok = 1;
        if ((doc = json_parse(r.body)) != NULL)
        {
            JsonNode *b = json_get(doc->root, "build");
            if (b && b->type == JSON_NUMBER && (int)b->num > TB_BUILD)
            {
                const char *v = json_str(json_get(doc->root, "v"));
                const char *x = json_str(json_get(doc->root, "xex"));
                JsonNode *sz = json_get(doc->root, "size");
                copy(g_ver, sizeof(g_ver), v ? v : "?");
                copy(g_notes, sizeof(g_notes), json_str(json_get(doc->root, "notas")));
                copy(g_url, sizeof(g_url), json_str(json_get(doc->root, "url")));
                if (x && x[0])
                    direct_link(x, g_xex, sizeof(g_xex));
                g_size = sz && sz->type == JSON_NUMBER ? sz->num : 0.0;
                InterlockedExchange(&g_avail, 1);
                net_log("[update] hay version nueva: %s (build %d, esta es %d)%s", g_ver, (int)b->num, TB_BUILD,
                        g_xex[0] ? ", con descarga" : "");
            }
            else
                net_log("[update] sin version nueva (esta es build %d)", TB_BUILD);
            json_free(doc);
        }
    }
    else
        net_log("[update] HTTP %d", r.status);
    http_free(&r);
    return ok;
}

static DWORD WINAPI update_thread(LPVOID p)
{
    int i, t;

    (void)p;
    for (i = 0; i < 5 && !g_quit; ++i)   /* si aun no hay red: reintenta cada 30 s */
    {
        if (check_once())
        {
            InterlockedExchange(&g_checked, 1);
            return 0;
        }
        for (t = 0; t < 300 && !g_quit; ++t)
            Sleep(100);
    }
    InterlockedExchange(&g_checked, -1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Descarga                                                            */

static void dl_fail(const char *why)
{
    copy(g_dlErr, sizeof(g_dlErr), why);
    net_log("[update] descarga: %s", why);
    InterlockedExchange(&g_dlState, UPD_DL_FAIL);
}

static DWORD WINAPI dl_thread(LPVOID p)
{
    char hdr[96], err[160];
    HttpResponse r;
    FILE *f;
    double pos = 0.0;
    int first = 1;

    (void)p;
    CreateDirectoryA(UPD_DIR, NULL);   /* si ya existe, no pasa nada */
    DeleteFileA(PART_FILE);
    f = fopen(PART_FILE, "wb");
    if (!f)
    {
        dl_fail(T(S_UPD_WRITE));
        return 0;
    }
    net_set_cancel(&g_dlCancel);
    for (;;)
    {
        int got, st;
        if (g_dlCancel)
            break;
        _snprintf(hdr, sizeof(hdr) - 1, "Range: bytes=%.0f-%.0f\r\n", pos, pos + CHUNK - 1.0);
        hdr[sizeof(hdr) - 1] = 0;
        if (!http_request("GET", g_xex, hdr, NULL, 0, &r, err, sizeof(err)))
        {
            if (!g_dlCancel)
                dl_fail(err);
            break;
        }
        if (r.status == 416 && !first)   /* ya no hay mas (el tamano era multiplo de 1 MB) */
        {
            http_free(&r);
            pos = -pos - 1.0;   /* marca: termino bien */
            break;
        }
        if (r.status != 206 && r.status != 200)
        {
            _snprintf(err, sizeof(err) - 1, "HTTP %d", r.status);
            err[sizeof(err) - 1] = 0;
            http_free(&r);
            dl_fail(err);
            break;
        }
        got = r.bodyLen;
        st = r.status;
        if (first && (got < 4 || memcmp(r.body, "XEX2", 4) != 0))
        {
            /* Drive manda una pagina HTML si el archivo no es publico */
            http_free(&r);
            dl_fail(T(S_UPD_BADFILE));
            break;
        }
        first = 0;
        if (got > 0 && fwrite(r.body, 1, got, f) != (size_t)got)
        {
            http_free(&r);
            dl_fail(T(S_UPD_WRITE));
            break;
        }
        http_free(&r);
        pos += got;
        g_dlDone = pos;
        /* 200 = el servidor mando todo de una; menos de 1 MB = era el ultimo pedazo */
        if (st == 200 || got < (int)CHUNK)
        {
            pos = -pos - 1.0;
            break;
        }
    }
    net_set_cancel(NULL);
    fclose(f);

    if (pos < 0.0 && g_dlState == UPD_DL_RUNNING)
    {
        DeleteFileA(UPD_FILE);
        if (!MoveFileA(PART_FILE, UPD_FILE))
        {
            dl_fail(T(S_UPD_WRITE));
            return 0;
        }
        net_log("[update] descarga lista: %.0f bytes en %s", -pos - 1.0, UPD_FILE);
        InterlockedExchange(&g_dlState, UPD_DL_DONE);
        return 0;
    }
    DeleteFileA(PART_FILE);
    if (g_dlCancel && g_dlState == UPD_DL_RUNNING)
        InterlockedExchange(&g_dlState, UPD_DL_IDLE);
    return 0;
}

void update_download_start(void)
{
    if (!g_xex[0] || g_dlState == UPD_DL_RUNNING)
        return;
    if (g_dlThread)
    {
        WaitForSingleObject(g_dlThread, 2000);
        CloseHandle(g_dlThread);
        g_dlThread = NULL;
    }
    g_dlCancel = 0;
    g_dlDone = 0.0;
    g_dlErr[0] = 0;
    InterlockedExchange(&g_dlState, UPD_DL_RUNNING);
    g_dlThread = CreateThread(NULL, 128 * 1024, dl_thread, NULL, 0, NULL);
    if (!g_dlThread)
        dl_fail("CreateThread");
}

void update_download_cancel(void)
{
    if (g_dlState == UPD_DL_RUNNING)
    {
        g_dlCancel = 1;
        InterlockedExchange(&g_dlState, UPD_DL_IDLE);
    }
}

int update_dl_state(void) { return (int)g_dlState; }

void update_dl_progress(double *done, double *total)
{
    if (done)
        *done = g_dlDone;
    if (total)
        *total = g_size;
}

const char *update_dl_error(void) { return g_dlErr; }

/* ------------------------------------------------------------------ */

void update_start(void)
{
    if (g_thread || !TB_UPDATE_URL[0])
        return;
    g_quit = 0;
    g_thread = CreateThread(NULL, 128 * 1024, update_thread, NULL, 0, NULL);
}

void update_stop(void)
{
    g_quit = 1;
    g_dlCancel = 1;
    if (g_thread)
    {
        WaitForSingleObject(g_thread, 1500);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_dlThread)
    {
        WaitForSingleObject(g_dlThread, 3000);
        CloseHandle(g_dlThread);
        g_dlThread = NULL;
    }
}

int         update_enabled(void)      { return TB_UPDATE_URL[0] != 0; }
int         update_checked(void)      { return (int)g_checked; }
int         update_available(void)    { return g_avail != 0; }
const char *update_version(void)      { return g_ver; }
const char *update_notes(void)        { return g_notes; }
const char *update_url(void)          { return g_url; }
int         update_can_download(void) { return g_avail && g_xex[0]; }
