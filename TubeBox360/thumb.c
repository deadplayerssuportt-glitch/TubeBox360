/* thumb.c - Miniaturas de los videos (ver thumb.h). C89.
 *
 * Cada miniatura vive en un "lugar" (slot) que pasa por estos estados:
 *
 *   LIBRE -> PEDIDA -> DESCARGANDO -> DESCARGADA -> LISTA
 *                            \                \
 *                             +-> FALLO        +-> FALLO
 *
 * Quien cambia cada estado:
 *   - Hilo principal: LIBRE -> PEDIDA, DESCARGADA -> LISTA/FALLO, y
 *     recicla lugares (nunca uno DESCARGANDO).
 *   - Hilos de descarga: PEDIDA -> DESCARGANDO -> DESCARGADA/FALLO, y
 *     PEDIDA -> LIBRE si ya nadie la necesita.
 * Los cambios se hacen dentro de g_lock. Una miniatura DESCARGADA o LISTA
 * solo la toca el hilo principal, por eso la puede decodificar y dibujar
 * sin tener el candado.
 *
 * Liberar una textura: la GPU puede ir hasta 2 cuadros atrasada, asi que
 * solo se recicla un lugar que no se dibujo en los ultimos 4 cuadros. */

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include "thumb.h"
#include "net.h"
#include "gfx.h"

#define THUMB_SLOTS      96     /* miniaturas, avatares y banners */
#define THUMB_WORKERS    3
#define KEY_SIZE         400    /* id del video, o la URL del avatar/banner */
#define GPU_SAFE_FRAMES  4      /* cuadros antes de poder liberar una textura */
#define STALE_FRAMES     180    /* pedida hace mas de ~3 s y ya no se ve: se cancela */
#define RETRY_MS         3000   /* fallo de red: se reintenta despues de 3 s */
#define FADE_MS          180    /* la imagen aparece poco a poco */
/* colores de los recuadros mientras llegan (cambian con el tema) */
static unsigned int g_colHolder = 0xFF2A2A2A;
static unsigned int g_colAvatar = 0xFF3A3A3A;
static unsigned int g_colLetter = 0xFFD0D0D0;
#define COL_HOLDER g_colHolder

enum { TS_FREE, TS_WANTED, TS_LOADING, TS_DOWNLOADED, TS_READY, TS_FAILED };

typedef struct
{
    char         key[KEY_SIZE];
    int          kind;        /* THUMB_VIDEO, THUMB_AVATAR, THUMB_BANNER */
    int          state;
    DWORD        lastUsed;    /* cuadro en que se pidio o dibujo por ultima vez */
    int          order;       /* en que orden se pidio dentro de ese cuadro */
    HttpResponse resp;        /* el JPEG (estado DESCARGADA) */
    void        *tex;         /* la textura (estado LISTA) */
    DWORD        readyTick;
    DWORD        retryTick;   /* estado FALLO: cuando reintentar; 0 = nunca */
} Thumb;

static Thumb            g_slots[THUMB_SLOTS];
static CRITICAL_SECTION g_lock;
static HANDLE           g_wake = NULL;
static int              g_active = 1;    /* con g_lock */
static int              g_ok = 0;

static DWORD g_frame = 0;        /* lo escribe el hilo principal */
static int   g_touches = 0;      /* pedidas en este cuadro */
static int   g_decoded = 0;      /* texturas creadas en este cuadro */

/* Para el reporte de pruebas (con g_lock):
 *   - JPEG que D3DX no pudo leer: se quedan a la vista siempre.
 *   - Fallo de descarga: se ve solo hasta que otra descarga salga bien. */
static int  g_jpegFails = 0;
static char g_jpegErr[64] = "";
static char g_netErr[96] = "";

/* ------------------------------------------------------------------ */
/* Hilos de descarga                                                   */

/* Toma la miniatura pedida mas reciente (y, entre las de un mismo cuadro,
 * la que se pidio primero). Cancela las que ya nadie mira. Con g_lock. */
static Thumb *take_next(int *more)
{
    Thumb *t = NULL;
    Thumb *s;
    int i;

    *more = 0;

    /* durante el video solo se bajan avatares (el del boton del canal) */
    for (i = 0; i < THUMB_SLOTS; ++i)
    {
        s = &g_slots[i];
        if (s->state != TS_WANTED || (!g_active && s->kind != THUMB_AVATAR))
            continue;
        if ((LONG)(g_frame - s->lastUsed) > STALE_FRAMES)
        {
            s->state = TS_FREE;
            s->key[0] = 0;
            continue;
        }
        if (!t)
        {
            t = s;
            continue;
        }
        *more = 1;
        if ((LONG)(s->lastUsed - t->lastUsed) > 0 ||
            (s->lastUsed == t->lastUsed && s->order < t->order))
            t = s;
    }
    if (t)
        t->state = TS_LOADING;
    return t;
}

/* Las que fallaron por la red se reintentan en cuanto alguien las vuelva
 * a dibujar. Con g_lock. */
static void retry_now(void)
{
    int i;

    for (i = 0; i < THUMB_SLOTS; ++i)
        if (g_slots[i].state == TS_FAILED && g_slots[i].retryTick)
            g_slots[i].retryTick = GetTickCount() | 1;
}

static DWORD WINAPI worker_thread(LPVOID param)
{
    char key[KEY_SIZE];
    char url[KEY_SIZE + 64];
    char err[128];
    int kind = 0;
    HttpResponse resp;
    Thumb *t;
    int ok, more;

    /* nucleos 2, 3 y 4: fuera de la reproduccion casi no tienen trabajo */
    XSetThreadProcessor(GetCurrentThread(), 2 + (int)(DWORD)param);

    for (;;)
    {
        WaitForSingleObject(g_wake, 1000);

        for (;;)
        {
            EnterCriticalSection(&g_lock);
            t = take_next(&more);
            if (t)
            {
                strcpy(key, t->key);
                kind = t->kind;
            }
            LeaveCriticalSection(&g_lock);

            if (!t)
                break;
            if (more)
                SetEvent(g_wake);   /* que otro hilo tome la siguiente */

            if (kind == THUMB_VIDEO)
                _snprintf(url, sizeof(url) - 1, "https://i.ytimg.com/vi/%s/mqdefault.jpg", key);
            else
                _snprintf(url, sizeof(url) - 1, "%s", key);   /* ya es la URL */
            url[sizeof(url) - 1] = 0;
            err[0] = 0;
            ok = http_request("GET", url, NULL, NULL, 0, &resp, err, sizeof(err));
            ok = ok ? (resp.status == 200 && resp.bodyLen > 0 ? 1 : -1) : 0;

            EnterCriticalSection(&g_lock);
            if (ok > 0)
            {
                t->resp = resp;          /* ahora el JPEG es del lugar */
                t->state = TS_DOWNLOADED;
                g_netErr[0] = 0;
                retry_now();             /* la red volvio: no esperar los 3 s */
            }
            else
            {
                /* 404 y parecidos no se reintentan; un error de red si */
                t->state = TS_FAILED;
                t->retryTick = 0;
                if (ok == 0)
                    t->retryTick = (GetTickCount() + RETRY_MS) | 1;
                if (ok < 0)
                    _snprintf(g_netErr, sizeof(g_netErr) - 1, "HTTP %d (%.40s)", resp.status, key);
                else
                    _snprintf(g_netErr, sizeof(g_netErr) - 1, "%s", err);
                g_netErr[sizeof(g_netErr) - 1] = 0;
            }
            LeaveCriticalSection(&g_lock);

            if (ok < 0)
                http_free(&resp);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hilo principal                                                      */

int thumb_init(void)
{
    HANDLE h;
    int i, started = 0;

    InitializeCriticalSection(&g_lock);
    ZeroMemory(g_slots, sizeof(g_slots));
    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_wake)
        return 0;

    for (i = 0; i < THUMB_WORKERS; ++i)
    {
        h = CreateThread(NULL, 64 * 1024, worker_thread, (LPVOID)(DWORD)i, 0, NULL);
        if (h)
        {
            CloseHandle(h);
            ++started;
        }
    }
    g_ok = started > 0;
    OutputDebugStringA(g_ok ? "[thumb] Listo\n" : "[thumb] No se pudieron crear los hilos\n");
    return g_ok;
}

void thumb_frame(void)
{
    if (g_ok)
        EnterCriticalSection(&g_lock);   /* los hilos de descarga leen g_frame */
    ++g_frame;
    if (g_ok)
        LeaveCriticalSection(&g_lock);
    g_touches = 0;
    g_decoded = 0;
}

void thumb_set_active(int on)
{
    if (!g_ok)
        return;
    EnterCriticalSection(&g_lock);
    g_active = on ? 1 : 0;
    LeaveCriticalSection(&g_lock);
    if (on)
        SetEvent(g_wake);
}

/* Deja el lugar libre. Solo el hilo principal, con g_lock, y nunca con
 * un lugar DESCARGANDO. */
static void clear_slot(Thumb *t)
{
    if (t->state == TS_READY)
        gfx_texture_free(t->tex);
    if (t->state == TS_DOWNLOADED)
        http_free(&t->resp);
    t->tex = NULL;
    t->state = TS_FREE;
    t->key[0] = 0;
}

/* Un lugar para una miniatura nueva: uno libre o, si no hay, el que lleva
 * mas tiempo sin usarse (y que la GPU ya no esta leyendo). Con g_lock. */
static Thumb *alloc_slot(void)
{
    Thumb *best = NULL;
    Thumb *t;
    int i;

    for (i = 0; i < THUMB_SLOTS; ++i)
        if (g_slots[i].state == TS_FREE)
            return &g_slots[i];

    for (i = 0; i < THUMB_SLOTS; ++i)
    {
        t = &g_slots[i];
        if (t->state == TS_LOADING)
            continue;
        if ((LONG)(g_frame - t->lastUsed) < GPU_SAFE_FRAMES)
            continue;
        if (!best || (LONG)(t->lastUsed - best->lastUsed) < 0)
            best = t;
    }
    if (best)
        clear_slot(best);
    return best;
}

/* Busca la miniatura (o la pide) y marca que se uso en este cuadro.
 * Devuelve el lugar y su estado, o NULL. */
static Thumb *touch(const char *id, int kind, int *state)
{
    Thumb *t = NULL;
    int i, wake = 0;

    *state = TS_FREE;
    if (!g_ok || !id || !id[0] || strlen(id) >= KEY_SIZE)
        return NULL;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < THUMB_SLOTS; ++i)
    {
        if (g_slots[i].state != TS_FREE && g_slots[i].kind == kind && strcmp(g_slots[i].key, id) == 0)
        {
            t = &g_slots[i];
            break;
        }
    }
    if (!t)
    {
        t = alloc_slot();
        if (t)
        {
            strcpy(t->key, id);
            t->kind = kind;
            t->state = TS_WANTED;
            t->retryTick = 0;
            wake = 1;
        }
    }
    else if (t->state == TS_FAILED && t->retryTick &&
             (LONG)(GetTickCount() - t->retryTick) >= 0)
    {
        t->state = TS_WANTED;
        wake = 1;
    }
    if (t)
    {
        if (wake || t->lastUsed != g_frame)
            t->order = g_touches++;    /* la primera vez que se pide en este cuadro */
        t->lastUsed = g_frame;
        *state = t->state;
    }
    LeaveCriticalSection(&g_lock);

    if (wake)
        SetEvent(g_wake);
    return t;
}

/* JPEG -> textura (como mucho una por cuadro) */
static int decode(Thumb *t)
{
    void *tex;
    long hr = 0;
    int len = t->resp.bodyLen;

    /* tamano de la textura segun lo que es (D3DX la escala) */
    static const int texW[3] = { 320, 88, 1060 };
    static const int texH[3] = { 180, 88, 175 };

    g_decoded = 1;
    tex = gfx_texture_load(t->resp.body, len, texW[t->kind], texH[t->kind], &hr);
    http_free(&t->resp);

    EnterCriticalSection(&g_lock);
    if (tex)
    {
        t->tex = tex;
        t->readyTick = GetTickCount();
        t->state = TS_READY;
    }
    else
    {
        t->state = TS_FAILED;
        t->retryTick = 0;
        ++g_jpegFails;
        _snprintf(g_jpegErr, sizeof(g_jpegErr) - 1, "hr 0x%08lX, %d bytes", (unsigned long)hr, len);
        g_jpegErr[sizeof(g_jpegErr) - 1] = 0;
    }
    LeaveCriticalSection(&g_lock);

    if (!tex)
    {
        OutputDebugStringA("[thumb] D3DX no pudo leer el JPEG: ");
        OutputDebugStringA(g_jpegErr);
        OutputDebugStringA("\n");
    }
    return t->state;
}

/* Busca, decodifica si toca, y dice con que alfa dibujarla (aparece poco a
 * poco). Devuelve la textura o NULL. */
static void *ready_tex(const char *key, int kind, unsigned int *alpha)
{
    Thumb *t;
    int state;
    DWORD age;

    t = touch(key, kind, &state);
    if (t && state == TS_DOWNLOADED && !g_decoded)
        state = decode(t);
    if (!t || state != TS_READY)
        return NULL;
    age = GetTickCount() - t->readyTick;
    *alpha = age >= FADE_MS ? 255 : (unsigned int)(age * 255 / FADE_MS);
    return t->tex;
}

int thumb_draw(const char *id, float x, float y, float w, float h)
{
    unsigned int a = 0;
    void *tex = ready_tex(id, THUMB_VIDEO, &a);

    if (!tex || a < 255)
        gfx_rect(x, y, w, h, COL_HOLDER);
    if (tex)
        gfx_image(tex, x, y, w, h, (a << 24) | 0x00FFFFFF);
    return tex != NULL;
}

int thumb_draw_avatar(const char *url, float x, float y, float size, const char *name)
{
    unsigned int a = 0;
    void *tex = (url && url[0]) ? ready_tex(url, THUMB_AVATAR, &a) : NULL;
    char letter[8];
    float scale;

    if (!tex || a < 255)
    {
        /* circulo gris con la inicial del canal mientras llega (o si no hay) */
        gfx_circle(x, y, size, g_colAvatar);
        if (name && name[0] && size >= 20.0f)
        {
            int n = 1;
            while (n < 4 && ((unsigned char)name[n] & 0xC0) == 0x80)
                ++n;   /* la primera letra completa (UTF-8) */
            memcpy(letter, name, n);
            letter[n] = 0;
            scale = size / 40.0f;
            gfx_text(x + (size - gfx_text_width(scale, letter)) * 0.5f,
                     y + (size - 24.0f * scale) * 0.5f, scale, g_colLetter, letter);
        }
    }
    if (tex)
        gfx_image_circle(tex, x, y, size, (a << 24) | 0x00FFFFFF);
    return tex != NULL;
}

int thumb_draw_banner(const char *url, float x, float y, float w, float h)
{
    unsigned int a = 0;
    void *tex = (url && url[0]) ? ready_tex(url, THUMB_BANNER, &a) : NULL;
    float v0, v1, texAspect = 1060.0f / 175.0f, boxAspect = w / h;

    if (!tex || a < 255)
        gfx_rect(x, y, w, h, g_colHolder);
    if (!tex)
        return 0;
    /* se recorta arriba y abajo para llenar el recuadro sin deformarla */
    v0 = 0.0f;
    v1 = 1.0f;
    if (boxAspect > texAspect)
    {
        float used = texAspect / boxAspect;
        v0 = (1.0f - used) * 0.5f;
        v1 = v0 + used;
    }
    gfx_image_uv(tex, x, y, w, h, 0.0f, v0, 1.0f, v1, (a << 24) | 0x00FFFFFF);
    return 1;
}

void thumb_set_colors(unsigned int holder, unsigned int avatar, unsigned int letter)
{
    g_colHolder = holder;
    g_colAvatar = avatar;
    g_colLetter = letter;
}

void thumb_prefetch(const char *id)
{
    unsigned int a;
    ready_tex(id, THUMB_VIDEO, &a);
}

void thumb_prefetch_avatar(const char *url)
{
    unsigned int a;
    if (url && url[0])
        ready_tex(url, THUMB_AVATAR, &a);
}

int thumb_status(char *out, int size)
{
    int shown = 1;

    if (!g_ok)
        return 0;
    EnterCriticalSection(&g_lock);
    if (g_jpegFails)
        _snprintf(out, size - 1, "JPEG sin leer: %d (%s)", g_jpegFails, g_jpegErr);
    else if (g_netErr[0])
        _snprintf(out, size - 1, "Miniaturas: %s", g_netErr);
    else
        shown = 0;
    LeaveCriticalSection(&g_lock);
    if (shown)
        out[size - 1] = 0;
    return shown;
}
