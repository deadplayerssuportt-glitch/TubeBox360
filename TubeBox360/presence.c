/* presence.c - Contador de usuarios activos. Ver presence.h. C89.
 *
 * Firebase Realtime Database por REST (HTTPS normal con net.c):
 *   PUT  {base}/presence/{id}.json   {"t":{".sv":"timestamp"}}
 *        -> responde {"t":<hora del servidor en ms>}
 *   GET  {base}/presence.json?orderBy="t"&startAt=<t - 150 s>
 *        -> los que escribieron hace poco; se cuentan
 *   Cada 10 vueltas se borran hasta 20 registros de hace mas de un dia
 *   (consolas que se apagaron sin salir de la app).
 * Reglas que hay que poner en la base: ver LEEME (".indexOn": ["t"]). */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "presence.h"
#include "version.h"
#include "net.h"
#include "json.h"

#ifndef BEAT_MS
#define BEAT_MS        60000
#endif
#define ACTIVE_MS      150000.0      /* activo = escribio en los ultimos 2.5 min */
#define STALE_MS       86400000.0    /* registros de mas de un dia: basura */
#define VALID_MS       200000        /* el conteo vale ~3 min si no se renueva */

static char           g_base[160];
static char           g_id[24];
static HANDLE         g_thread = NULL;
static volatile long  g_quit = 0;
static volatile LONG  g_count = -1;
static volatile LONG  g_okAt = 0;     /* GetTickCount del ultimo conteo bueno */

static double read_t(const char *body)
{
    const char *p = strstr(body ? body : "", "\"t\":");
    return p ? atof(p + 4) : 0.0;
}

static int count_entries(const char *body)
{
    const char *p = body;
    int n = 0;

    while (p && (p = strstr(p, "\"t\":")) != NULL)
    {
        ++n;
        p += 4;
    }
    return n;
}

static void housekeeping(double now)
{
    char url[400], err[160];
    HttpResponse r;
    JsonDoc *doc;
    JsonNode *c;
    int n = 0;

    _snprintf(url, sizeof(url) - 1, "%s/presence.json?orderBy=%%22t%%22&endAt=%.0f&limitToFirst=20", g_base,
              now - STALE_MS);
    url[sizeof(url) - 1] = 0;
    if (!http_request("GET", url, NULL, NULL, 0, &r, err, sizeof(err)))
        return;
    if (r.status == 200 && (doc = json_parse(r.body)) != NULL)
    {
        for (c = doc->root ? doc->root->child : NULL; c && !g_quit; c = c->next)
        {
            HttpResponse d;
            if (!c->key || strlen(c->key) > 32)
                continue;
            _snprintf(url, sizeof(url) - 1, "%s/presence/%s.json", g_base, c->key);
            url[sizeof(url) - 1] = 0;
            if (http_request("DELETE", url, NULL, NULL, 0, &d, err, sizeof(err)))
            {
                http_free(&d);
                ++n;
            }
        }
        json_free(doc);
    }
    http_free(&r);
    if (n)
        net_log("[usuarios] se borraron %d registros viejos", n);
}

static int beat(int round)
{
    static const char body[] = "{\"t\":{\".sv\":\"timestamp\"}}";
    char url[400], err[160];
    HttpResponse r;
    double t;
    int n;

    _snprintf(url, sizeof(url) - 1, "%s/presence/%s.json", g_base, g_id);
    url[sizeof(url) - 1] = 0;
    if (!http_request("PUT", url, "Content-Type: application/json\r\n", body, (int)strlen(body), &r, err,
                      sizeof(err)))
    {
        if (round == 0 && !g_quit)
            net_log("[usuarios] no se pudo escribir: %s", err);
        return 0;
    }
    t = r.status == 200 ? read_t(r.body) : 0.0;
    if (r.status != 200 && round == 0)
        net_log("[usuarios] la base respondio HTTP %d (revisa las reglas): %.120s", r.status, r.body);
    if (r.status == 200 && t <= 0.0)
    {
        /* (si la respuesta no trajo la hora ya resuelta, se lee) */
        http_free(&r);
        _snprintf(url, sizeof(url) - 1, "%s/presence/%s/t.json", g_base, g_id);
        url[sizeof(url) - 1] = 0;
        if (!http_request("GET", url, NULL, NULL, 0, &r, err, sizeof(err)))
            return 0;
        t = r.status == 200 ? atof(r.body) : 0.0;
    }
    http_free(&r);
    if (t <= 0.0)
        return 0;

    _snprintf(url, sizeof(url) - 1, "%s/presence.json?orderBy=%%22t%%22&startAt=%.0f", g_base, t - ACTIVE_MS);
    url[sizeof(url) - 1] = 0;
    if (!http_request("GET", url, NULL, NULL, 0, &r, err, sizeof(err)))
        return 0;
    if (r.status == 200)
    {
        n = count_entries(r.body);
        if (n < 1)
            n = 1;                  /* por lo menos esta consola */
        InterlockedExchange(&g_count, n);
        InterlockedExchange(&g_okAt, (LONG)GetTickCount());
        if (round == 0)
            net_log("[usuarios] conectado: %d activos", n);
    }
    else if (round == 0)
        net_log("[usuarios] el conteo respondio HTTP %d: %.120s", r.status, r.body);
    http_free(&r);

    if (round % 10 == 9)
        housekeeping(t);
    return 1;
}

static DWORD WINAPI presence_thread(LPVOID p)
{
    char url[400], err[160];
    HttpResponse r;
    int round = 0, k;

    (void)p;
    XSetThreadProcessor(GetCurrentThread(), 1);
    net_set_cancel(&g_quit);
    Sleep(3000);                    /* que primero cargue el Inicio */
    while (!g_quit)
    {
        beat(round++);
        for (k = 0; k < BEAT_MS / 250 && !g_quit; ++k)
            Sleep(250);
    }
    /* al salir: borrar el registro propio (sin la bandera de cancelar) */
    net_set_cancel(NULL);
    _snprintf(url, sizeof(url) - 1, "%s/presence/%s.json", g_base, g_id);
    url[sizeof(url) - 1] = 0;
    if (http_request("DELETE", url, NULL, NULL, 0, &r, err, sizeof(err)))
        http_free(&r);
    return 0;
}

void presence_start(void)
{
    BYTE rnd[8];
    int i;

    if (g_thread || !TB_PRESENCE_URL[0])
        return;
    strncpy(g_base, TB_PRESENCE_URL, sizeof(g_base) - 1);
    g_base[sizeof(g_base) - 1] = 0;
    i = (int)strlen(g_base);
    if (i > 0 && g_base[i - 1] == '/')
        g_base[i - 1] = 0;
    XNetRandom(rnd, sizeof(rnd));
    for (i = 0; i < 8; ++i)
        sprintf(g_id + i * 2, "%02x", rnd[i]);
    g_thread = CreateThread(NULL, 64 * 1024, presence_thread, NULL, 0, NULL);
}

void presence_stop(void)
{
    if (!g_thread)
        return;
    g_quit = 1;
    WaitForSingleObject(g_thread, 2500);
    CloseHandle(g_thread);
    g_thread = NULL;
}

int presence_count(void)
{
    if (g_count < 0 || GetTickCount() - (DWORD)g_okAt > VALID_MS)
        return -1;
    return (int)g_count;
}
