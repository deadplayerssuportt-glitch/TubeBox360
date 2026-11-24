/* community.c - Recomendados por la comunidad. Ver community.h. C89.
 *   PUT    {base}/recomendados/{id}.json   {...,"t":{".sv":"timestamp"}}
 *   GET    {base}/recomendados.json?orderBy="t"&limitToLast=30
 *   DELETE {base}/recomendados/{id}.json   (los que pasaron del lugar 10) */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "community.h"
#include "version.h"
#include "net.h"
#include "json.h"
#include "lang.h"

#ifndef TB_COMMUNITY_URL
#define TB_COMMUNITY_URL TB_PRESENCE_URL
#endif
#define GAP_MS   30000UL     /* una recomendacion cada 30 s por consola */
#define KEEP_IDS 32

typedef struct { char id[16]; double t; } RecKey;

static CRITICAL_SECTION g_cs;
static int     g_csInit = 0;
static YtVideo g_pending;
static HANDLE  g_thread = NULL;
static volatile LONG g_busy = 0;
static volatile LONG g_result = 0;
static volatile LONG g_version = 0;
static DWORD   g_lastAt = 0;
static int     g_everSent = 0;
static char    g_known[KEEP_IDS][16];   /* recomendados conocidos */
static int     g_knownN = 0;

static void init_once(void)
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = 1;
    }
}

static void add_known(const char *id)
{
    int i;
    for (i = 0; i < g_knownN; ++i)
        if (strcmp(g_known[i], id) == 0)
            return;
    if (g_knownN < KEEP_IDS)
        ++g_knownN;
    else
        memmove(g_known[0], g_known[1], sizeof(g_known[0]) * (KEEP_IDS - 1));
    strncpy(g_known[g_knownN - 1], id, 15);
    g_known[g_knownN - 1][15] = 0;
}

static int valid_id(const char *id)
{
    int n = 0;
    for (; *id; ++id, ++n)
    {
        char c = *id;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return n == 11;
}

/* texto JSON con escapes (UTF-8 tal cual) */
static int json_esc(char *dst, int size, const char *src, int maxChars)
{
    int n = 0, k = 0;
    for (; src && *src && n < size - 8 && k < maxChars; ++src, ++k)
    {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\')
        {
            dst[n++] = '\\';
            dst[n++] = (char)c;
        }
        else if (c < 0x20)
            dst[n++] = ' ';
        else
            dst[n++] = (char)c;
    }
    /* no cortar un caracter UTF-8 a la mitad */
    while (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0x80)
        --n;
    if (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0xC0)
        --n;
    dst[n] = 0;
    return n;
}

static int cmp_newest(const void *a, const void *b)
{
    double ta = ((const RecKey *)a)->t, tb = ((const RecKey *)b)->t;
    return ta < tb ? 1 : ta > tb ? -1 : 0;
}

/* Lee hasta 30 (los mas nuevos) ordenados del mas nuevo al mas viejo */
/* (el JSON apunta dentro de r->body: liberar r con http_free despues de
 * json_free, y solo si devolvio >= 0) */
static int read_keys(RecKey *keys, int max, JsonDoc **docOut, HttpResponse *rp, char *err, int errSize)
{
    char url[300];
    JsonDoc *doc;
    JsonNode *c;
    int n = 0;

    *docOut = NULL;
    _snprintf(url, sizeof(url) - 1, "%s/recomendados.json?orderBy=%%22t%%22&limitToLast=30", TB_COMMUNITY_URL);
    url[sizeof(url) - 1] = 0;
    if (!http_request("GET", url, NULL, NULL, 0, rp, err, errSize))
        return -1;
    if (rp->status != 200)
    {
        _snprintf(err, errSize - 1, "HTTP %d", rp->status);
        err[errSize - 1] = 0;
        net_log("[comunidad] lista: HTTP %d", rp->status);
        http_free(rp);
        return -1;
    }
    doc = json_parse(rp->body);
    if (!doc)
        return 0;   /* "null": lista vacia */
    for (c = doc->root ? doc->root->child : NULL; c && n < max; c = c->next)
    {
        JsonNode *t = json_get(c, "t");
        if (!c->key || strlen(c->key) >= sizeof(keys[0].id))
            continue;
        strcpy(keys[n].id, c->key);
        keys[n].t = t && t->type == JSON_NUMBER ? t->num : 0.0;
        ++n;
    }
    qsort(keys, n, sizeof(RecKey), cmp_newest);
    *docOut = doc;
    return n;
}

static void trim_old(void)
{
    RecKey keys[30];
    JsonDoc *doc;
    char err[120], url[300];
    HttpResponse r, lr;
    int n, i;

    n = read_keys(keys, 30, &doc, &lr, err, sizeof(err));
    if (doc)
        json_free(doc);
    if (n >= 0)
        http_free(&lr);
    for (i = COMMUNITY_MAX; i < n; ++i)
    {
        _snprintf(url, sizeof(url) - 1, "%s/recomendados/%s.json", TB_COMMUNITY_URL, keys[i].id);
        url[sizeof(url) - 1] = 0;
        if (http_request("DELETE", url, NULL, NULL, 0, &r, err, sizeof(err)))
            http_free(&r);
    }
    if (n > COMMUNITY_MAX)
        net_log("[comunidad] se quitaron %d viejos", n - COMMUNITY_MAX);
}

static DWORD WINAPI rec_thread(LPVOID p)
{
    char body[1400], ti[520], ch[260], ci[64], l[32], a[400], url[300], err[160];
    HttpResponse r;
    YtVideo v;
    int ok = 0;

    (void)p;
    EnterCriticalSection(&g_cs);
    v = g_pending;
    LeaveCriticalSection(&g_cs);

    json_esc(ti, sizeof(ti), v.title, 200);
    json_esc(ch, sizeof(ch), v.channel, 100);
    json_esc(ci, sizeof(ci), v.channelId, 40);
    json_esc(l, sizeof(l), v.length, 12);
    json_esc(a, sizeof(a), strncmp(v.avatar, "https://", 8) == 0 ? v.avatar : "", 300);
    _snprintf(body, sizeof(body) - 1,
              "{\"ti\":\"%s\",\"ch\":\"%s\",\"ci\":\"%s\",\"l\":\"%s\",\"a\":\"%s\",\"t\":{\".sv\":\"timestamp\"}}",
              ti, ch, ci, l, a);
    body[sizeof(body) - 1] = 0;
    _snprintf(url, sizeof(url) - 1, "%s/recomendados/%s.json", TB_COMMUNITY_URL, v.id);
    url[sizeof(url) - 1] = 0;
    if (http_request("PUT", url, "Content-Type: application/json\r\n", body, (int)strlen(body), &r, err, sizeof(err)))
    {
        ok = r.status == 200;
        net_log("[comunidad] recomendar %s: HTTP %d", v.id, r.status);
        http_free(&r);
    }
    else
        net_log("[comunidad] recomendar %s: %s", v.id, err);
    if (ok)
    {
        trim_old();
        InterlockedIncrement(&g_version);
    }
    InterlockedExchange(&g_result, ok ? 1 : -1);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

int community_enabled(void)
{
    return TB_COMMUNITY_URL[0] != 0;
}

int community_wait_secs(void)
{
    DWORD d;
    if (!g_everSent)
        return 0;
    d = GetTickCount() - g_lastAt;
    return d >= GAP_MS ? 0 : (int)((GAP_MS - d + 999) / 1000);
}

int community_recommend(const YtVideo *v)
{
    init_once();
    if (!community_enabled() || !v || !valid_id(v->id) || g_busy || community_wait_secs() > 0)
        return 0;
    if (g_thread)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    EnterCriticalSection(&g_cs);
    g_pending = *v;
    add_known(v->id);
    LeaveCriticalSection(&g_cs);
    g_lastAt = GetTickCount();
    g_everSent = 1;
    InterlockedExchange(&g_busy, 1);
    g_thread = CreateThread(NULL, 128 * 1024, rec_thread, NULL, 0, NULL);
    if (!g_thread)
    {
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    return 1;
}

int community_is_recommended(const char *videoId)
{
    int i, yes = 0;
    init_once();
    if (!videoId || !videoId[0])
        return 0;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_knownN && !yes; ++i)
        yes = strcmp(g_known[i], videoId) == 0;
    LeaveCriticalSection(&g_cs);
    return yes;
}

int community_version(void)
{
    return (int)g_version;
}

int community_take_result(void)
{
    return (int)InterlockedExchange(&g_result, 0);
}

/* ms desde 1970 -> "23 nov. 2026" (o 2026年11月23日) */
static void epoch_date(double ms, char *out, int size)
{
    long days = (long)(ms / 86400000.0);
    long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;

    if (m <= 2)
        ++y;
    out[0] = 0;
    if (ms <= 0.0)
        return;
    if (lang_get() == LANG_JA)
        _snprintf(out, size - 1, "%ld\xE5\xB9\xB4%s%ld\xE6\x97\xA5", y, lang_month((int)m - 1), d);
    else if (lang_get() == LANG_EN)
        _snprintf(out, size - 1, "%s %ld, %ld", lang_month((int)m - 1), d, y);
    else
        _snprintf(out, size - 1, "%ld %s %ld", d, lang_month((int)m - 1), y);
    out[size - 1] = 0;
}

int community_fetch(YtVideo *out, int max, char *err, int errSize)
{
    RecKey keys[30];
    JsonDoc *doc;
    HttpResponse lr;
    int n, i, count = 0;

    init_once();
    if (!community_enabled())
    {
        strncpy(err, "sin base", errSize - 1);
        err[errSize - 1] = 0;
        return -1;
    }
    n = read_keys(keys, 30, &doc, &lr, err, errSize);
    if (n < 0)
        return -1;
    if (max > COMMUNITY_MAX)
        max = COMMUNITY_MAX;
    for (i = 0; i < n && count < max; ++i)
    {
        JsonNode *e = json_get(doc->root, keys[i].id);
        YtVideo *v = &out[count];
        const char *s;
        if (!e || !valid_id(keys[i].id))
            continue;
        memset(v, 0, sizeof(*v));
        v->kind = YT_KIND_VIDEO;
        strcpy(v->id, keys[i].id);
#define TAKE(field, key) s = json_str(json_get(e, key)); if (s) { strncpy(v->field, s, sizeof(v->field) - 1); }
        TAKE(title, "ti")
        TAKE(channel, "ch")
        TAKE(channelId, "ci")
        TAKE(length, "l")
        TAKE(avatar, "a")
#undef TAKE
        epoch_date(keys[i].t, v->published, sizeof(v->published));
        EnterCriticalSection(&g_cs);
        add_known(v->id);
        LeaveCriticalSection(&g_cs);
        ++count;
    }
    if (doc)
        json_free(doc);
    http_free(&lr);
    return count;
}
