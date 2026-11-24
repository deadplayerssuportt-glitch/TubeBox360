/* subs.c - Subtitulos durante el video (ver subs.h). C89.
 *
 * Formato json3 de YouTube (lo que usamos):
 *   {"events":[ {"tStartMs":140,"dDurationMs":4560,
 *                "segs":[{"utf8":"hola"},{"utf8":" que tal","tOffsetMs":400}]},
 *               {"tStartMs":2050,"aAppend":1,"segs":[{"utf8":"\n"}]}, ... ]}
 * Cada evento con texto es un subtitulo: se juntan sus "segs". Los que solo
 * traen un salto de linea (los subtitulos automaticos los usan para "rodar")
 * se saltan.
 *
 * Hilos: la descarga corre en su propio hilo. Cada subs_load/subs_clear sube
 * g_gen; un hilo cuyo numero ya no coincide tira lo que bajo. La lista
 * publicada (g_data) solo la libera el hilo principal. */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "version.h"
#include "subs.h"
#include "net.h"
#include "json.h"
#include "gfx.h"
#include "text.h"

#define SUB_SCALE      1.25f    /* letra de 15x30 */
#define SUB_MAX_W      1100.0f  /* ancho maximo de una linea (pixeles) */
#define SUB_LINE_BYTES 320
#define SUB_LINE_H     36.0f
#define SUB_MAX_LINES  3
#define SUB_SCREEN_W   1280.0f
#define SUB_BOX        0xC0000000
#define SUB_COLOR      0xFFFFFFFF
#define DEFAULT_DUR    3000     /* ms, si el evento no trae duracion */
#define DEBUG_PATH     "game:\\debug_subtitulos.json"

typedef struct
{
    int start;   /* ms */
    int end;     /* ms */
    int text;    /* posicion del texto en pool */
} Cue;

typedef struct
{
    Cue  *cues;
    int   count;
    char *pool;
} SubsData;

typedef struct
{
    LONG gen;
    char url[720];
} LoadJob;

static CRITICAL_SECTION g_lock;
static int              g_lockReady = 0;
static SubsData        *g_data = NULL;   /* la que se dibuja (con g_lock) */
static LONG             g_gen = 0;       /* con g_lock */
static int              g_state = 0;     /* con g_lock */
static char             g_err[160] = "";

/* ------------------------------------------------------------------ */
/* Lectura del json3                                                   */

static void free_data(SubsData *d)
{
    if (!d)
        return;
    free(d->cues);
    free(d->pool);
    free(d);
}

/* Agrega el texto de un evento al pool: sin espacios ni saltos al inicio
 * y al final, sin \r, y con los espacios repetidos juntados. Devuelve la
 * longitud (0 si no tenia texto). */
static int put_text(char *dst, const JsonNode *segs, int room)
{
    const JsonNode *sg;
    const char *s;
    int n = 0, space = 0;

    for (sg = segs->child; sg; sg = sg->next)
    {
        s = json_str(json_get(sg, "utf8"));
        for (; s && *s; ++s)
        {
            char ch = *s;
            if (ch == '\r')
                continue;
            if (ch == ' ' || ch == '\t')
            {
                space = 1;
                continue;
            }
            if (ch == '\n')
            {
                /* salto de linea: se guarda (sin espacios antes) */
                space = 0;
                if (n > 0 && dst[n - 1] != '\n' && n < room - 1)
                    dst[n++] = '\n';
                continue;
            }
            if (space && n > 0 && dst[n - 1] != '\n' && n < room - 1)
                dst[n++] = ' ';
            space = 0;
            if (n < room - 1)
                dst[n++] = ch;
        }
    }
    while (n > 0 && dst[n - 1] == '\n')
        --n;
    dst[n] = 0;
    return n;
}

static SubsData *parse_json3(char *json, int len)
{
    JsonDoc *doc;
    const JsonNode *events, *e, *segs, *t, *d;
    SubsData *data;
    int count = 0, used = 0, n, i;

    doc = json_parse(json);
    if (!doc)
        return NULL;
    events = json_get(doc->root, "events");
    if (!events || events->type != JSON_ARRAY)
    {
        json_free(doc);
        return NULL;
    }
    for (e = events->child; e; e = e->next)
        ++count;

    data = (SubsData *)calloc(1, sizeof(SubsData));
    if (data)
    {
        data->cues = (Cue *)malloc(sizeof(Cue) * (count > 0 ? count : 1));
        data->pool = (char *)malloc(len + 1);   /* el texto nunca es mas largo que el JSON */
    }
    if (!data || !data->cues || !data->pool)
    {
        free_data(data);
        json_free(doc);
        return NULL;
    }

    for (e = events->child; e; e = e->next)
    {
        segs = json_get(e, "segs");
        if (!segs || segs->type != JSON_ARRAY)
            continue;
        n = put_text(data->pool + used, segs, len + 1 - used);
        if (n <= 0)
            continue;
        t = json_get(e, "tStartMs");
        d = json_get(e, "dDurationMs");
        data->cues[data->count].start = (t && t->type == JSON_NUMBER) ? (int)t->num : 0;
        data->cues[data->count].end = (d && d->type == JSON_NUMBER && d->num > 0)
                                      ? data->cues[data->count].start + (int)d->num : -1;
        data->cues[data->count].text = used;
        used += n + 1;
        data->count++;
    }

    /* sin duracion: hasta que empiece el siguiente (maximo DEFAULT_DUR) */
    for (i = 0; i < data->count; ++i)
    {
        if (data->cues[i].end >= 0)
            continue;
        data->cues[i].end = data->cues[i].start + DEFAULT_DUR;
        if (i + 1 < data->count && data->cues[i + 1].start > data->cues[i].start &&
            data->cues[i + 1].start < data->cues[i].end)
            data->cues[i].end = data->cues[i + 1].start;
    }

    json_free(doc);
    return data;
}

/* ------------------------------------------------------------------ */
/* Descarga                                                            */

static DWORD WINAPI load_thread(LPVOID param)
{
    static const char headers[] =
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36\r\n";
    LoadJob *job = (LoadJob *)param;
    HttpResponse r;
    SubsData *data = NULL;
    char err[160];
    int ok;

    err[0] = 0;
    ok = http_request("GET", job->url, headers, NULL, 0, &r, err, sizeof(err));
    if (ok)
    {
#if !TB_BETA
        /* copia para revisar en la PC si algo sale mal */
        FILE *f = fopen(DEBUG_PATH, "wb");
        if (f)
        {
            fwrite(r.body, 1, r.bodyLen, f);
            fclose(f);
        }
#endif

        if (r.status == 200 && r.bodyLen == 0)
            strcpy(err, "YouTube no mand\xC3\xB3 subt\xC3\xADtulos (respuesta vac\xC3\xAD" "a)");
        else if (r.status == 200)
        {
            data = parse_json3(r.body, r.bodyLen);
            if (!data)
                strcpy(err, "No se pudieron leer los subt\xC3\xADtulos");
        }
        else
        {
            _snprintf(err, sizeof(err) - 1, "YouTube respondi\xC3\xB3 HTTP %d", r.status);
            err[sizeof(err) - 1] = 0;
        }
        http_free(&r);
    }

    EnterCriticalSection(&g_lock);
    if (job->gen == g_gen)
    {
        if (data)
        {
            g_data = data;
            data = NULL;
            g_state = 2;
        }
        else
        {
            strncpy(g_err, err[0] ? err : "Error al bajar los subt\xC3\xADtulos", sizeof(g_err) - 1);
            g_err[sizeof(g_err) - 1] = 0;
            g_state = -1;
        }
    }
    LeaveCriticalSection(&g_lock);

    free_data(data);   /* ya no se necesitaban (se pidieron otros) */
    free(job);
    return 0;
}

static void ensure_lock(void)
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = 1;
    }
}

/* Quita lo publicado y sube el numero de descarga. Devuelve el nuevo. */
static LONG reset(int state)
{
    SubsData *old;
    LONG gen;

    ensure_lock();
    EnterCriticalSection(&g_lock);
    old = g_data;
    g_data = NULL;
    gen = ++g_gen;
    g_state = state;
    g_err[0] = 0;
    LeaveCriticalSection(&g_lock);
    free_data(old);
    return gen;
}

void subs_load(const char *url)
{
    LoadJob *job;
    HANDLE h;

    job = (LoadJob *)malloc(sizeof(LoadJob));
    if (!job || !url || strlen(url) + 12 >= sizeof(job->url))
    {
        free(job);
        reset(-1);
        strcpy(g_err, "URL de subt\xC3\xADtulos no v\xC3\xA1lida");
        return;
    }
    job->gen = reset(1);
    sprintf(job->url, "%s&fmt=json3", url);

    h = CreateThread(NULL, 128 * 1024, load_thread, job, 0, NULL);
    if (!h)
    {
        free(job);
        reset(-1);
        strcpy(g_err, "No se pudo crear el hilo de subt\xC3\xADtulos");
        return;
    }
    CloseHandle(h);
}

void subs_clear(void)
{
    reset(0);
}

int subs_state(void)
{
    int s;

    ensure_lock();
    EnterCriticalSection(&g_lock);
    s = g_state;
    LeaveCriticalSection(&g_lock);
    return s;
}

const char *subs_error(void)
{
    return g_err;
}

/* ------------------------------------------------------------------ */
/* Que se ve en cada momento                                           */

/* Las lineas del segundo sec (ya partidas a lo ancho), hasta
 * SUB_MAX_LINES. Devuelve cuantas. */
static int lines_at(const SubsData *d, double sec, char lines[][SUB_LINE_BYTES])
{
    int idx[2];
    int found = 0, lo, hi, mid, last, i, k, n = 0, total;
    int ms = (int)(sec * 1000.0);
    char tmp[SUB_MAX_LINES * 2][SUB_LINE_BYTES];
    const char *p, *rest;
    char part[1024];
    int len;

    if (!d || d->count == 0 || sec < 0.0)
        return 0;

    /* ultimo que empezo antes de ms (la lista viene en orden) */
    lo = 0;
    hi = d->count - 1;
    last = -1;
    while (lo <= hi)
    {
        mid = (lo + hi) / 2;
        if (d->cues[mid].start <= ms)
        {
            last = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }

    /* hasta 2 vigentes, mirando unos pocos hacia atras */
    for (i = last; i >= 0 && i > last - 12 && found < 2; --i)
        if (d->cues[i].end > ms)
            idx[found++] = i;

    /* en orden de aparicion: el mas viejo arriba */
    for (k = found - 1; k >= 0; --k)
    {
        p = d->pool + d->cues[idx[k]].text;
        while (*p && n < SUB_MAX_LINES * 2)
        {
            /* un renglon del texto original (hasta \n) */
            len = 0;
            while (p[len] && p[len] != '\n' && len < (int)sizeof(part) - 1)
                ++len;
            memcpy(part, p, len);
            part[len] = 0;
            p += len;
            if (*p == '\n')
                ++p;

            /* partido a lo ancho de la pantalla */
            rest = part;
            while (*rest && n < SUB_MAX_LINES * 2)
            {
                rest = gfx_wrap_text(rest, SUB_MAX_W, SUB_SCALE, tmp[n], sizeof(tmp[n]));
                if (tmp[n][0])
                    ++n;
            }
        }
    }

    /* si no caben, se quedan las ultimas */
    total = n > SUB_MAX_LINES ? SUB_MAX_LINES : n;
    for (i = 0; i < total; ++i)
        strcpy(lines[i], tmp[n - total + i]);
    return total;
}

void subs_draw(double sec, float bottom)
{
    char lines[SUB_MAX_LINES][SUB_LINE_BYTES];
    SubsData *d;
    int n, i;
    float w, x, y;

    if (!g_lockReady)
        return;
    EnterCriticalSection(&g_lock);
    d = g_data;   /* solo este hilo la libera: se puede usar sin el candado */
    LeaveCriticalSection(&g_lock);

    n = lines_at(d, sec, lines);
    for (i = 0; i < n; ++i)
    {
        y = bottom - (n - i) * SUB_LINE_H;
        w = gfx_text_width(SUB_SCALE, lines[i]);
        x = (float)(int)((SUB_SCREEN_W - w) * 0.5f);
        gfx_rect(x - 10.0f, y - 2.0f, w + 20.0f, SUB_LINE_H - 2.0f, SUB_BOX);
        gfx_text(x, y + 1.0f, SUB_SCALE, SUB_COLOR, lines[i]);
    }
}

int subs_text_at(double sec, char *out, int size)
{
    char lines[SUB_MAX_LINES][SUB_LINE_BYTES];
    int n, i, used = 0;

    out[0] = 0;
    n = lines_at(g_data, sec, lines);
    for (i = 0; i < n && used < size - 1; ++i)
    {
        int k = _snprintf(out + used, size - used - 1, "%s%s", i ? "\n" : "", lines[i]);
        if (k < 0)
            break;
        used += k;
    }
    out[size - 1] = 0;
    return n;
}

int subs_parse_for_test(char *json)
{
    SubsData *d = parse_json3(json, (int)strlen(json));

    if (!d)
        return -1;
    reset(0);
    EnterCriticalSection(&g_lock);
    g_data = d;
    g_state = 2;
    LeaveCriticalSection(&g_lock);
    return d->count;
}
