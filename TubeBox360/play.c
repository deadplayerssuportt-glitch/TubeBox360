/* play.c - Pantalla de reproduccion (ver play.h). C89. */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lang.h"
#include "play.h"
#include "gfx.h"
#include "gpuvid.h"
#include "media.h"
#include "avc.h"
#include "text.h"
#include "audio.h"
#include "subs.h"
#include "thumb.h"
#include "ui_assets.h"
#include "net.h"
#include "report.h"
#include "cuenta.h"
#include "community.h"

/* la ruedita de carga (ui.c); ui.h no se incluye porque aqui los colores
 * COL_... son otros (el reproductor siempre es oscuro) */
#ifdef __cplusplus
extern "C" {
#endif
void ui_draw_spinner(float cx, float cy, float rad);
void ui_draw_sub_pill(float x, float y, int subscribed, int focused, int enabled);
#ifdef __cplusplus
}
#endif
#define UI_SUB_W  184.0f   /* (igual que en ui.h) */
#define UI_SUB_H  46.0f
#include "../ffaac/aac.h"

#define FRAG_SLOTS    3       /* fragmentos descargados por adelantado (~15 s) */
#define MAX_SAMPLES   1024    /* cuadros por fragmento (sobra) */
#define START_FRAMES  2       /* cuadros listos antes de arrancar el reloj */
#define FETCH_TRIES   3
#define AUDIO_START   0.5     /* segundos de audio en cola antes de arrancar */
#define AUDIO_WAIT    8000    /* ms que se espera al audio antes de seguir sin el */
#define MAX_ASAMPLES  2048
#define PCM_CAP       8192

#define COL_TEXT   0xFFFFFFFF
#define COL_SOFT   0xFFD0D0D0
#define COL_DIM    0xFF8A8A8A
#define COL_OK     0xFF4CC36A
#define COL_WARN   0xFFE6B422
#define COL_ERR    0xFFE62117
#define COL_BOX    0xB0000000

typedef struct
{
    HttpResponse r;
    int          seg;
} FragSlot;

/* Datos del video (los escribe la descarga antes de g_initDone = 1) */
static YtFormat  g_fmt;
static char      g_title[256];
static FmpTrack  g_track;

/* Estado compartido entre hilos */
static volatile LONG g_active;
static volatile LONG g_stop;
static volatile LONG g_initDone;     /* 0 esperando, 1 listo, -1 fallo */
static volatile LONG g_decReady;     /* 0 esperando, 1 abierto, -1 fallo */
static volatile LONG g_dlDone;
static volatile LONG g_feedDone;
static volatile LONG g_bytes;
static volatile LONG g_dlMs;
static volatile LONG g_segLoaded;
static volatile LONG g_segFed;
static volatile LONG g_hasErr;
static char          g_err[192];

/* Diagnostico: que hacen la descarga y el audio, y desde cuando */
static volatile LONG  g_dlBusy;        /* 1 bajando un fragmento */
static volatile DWORD g_dlTick;
static volatile LONG  g_auWhat;        /* 0 cola llena, 1 decodificando, 2 bajando */
static volatile DWORD g_auTick;
static int            g_holeNoted;     /* prueba 4: ya se anoto este hueco */

static CRITICAL_SECTION g_fragLock;
static int              g_lockReady;
static FragSlot         g_frags[FRAG_SLOTS];
static int              g_fragHead, g_fragCount;

static HANDLE g_dlThread;
static HANDLE g_feedThread;
static HANDLE g_audThread;

/* Audio (los escribe el hilo de audio) */
static YtFormat      g_afmt;
static int           g_hasAudio;
static FmpTrack      g_atrack;
static volatile LONG g_audState;     /* 0 cargando, 1 sonando/listo, 2 todo enviado, -1 fallo */
static volatile LONG g_audFrag;
static volatile LONG g_audBytes;
static char          g_audErr[160];
static char          g_audInfo[96];
static FmpSample     g_asamples[MAX_ASAMPLES];
static short         g_pcm[PCM_CAP];

/* Solo hilo del alimentador */
static FmpSample      g_samples[MAX_SAMPLES];
static unsigned char *g_au;
static int            g_auCap;

/* Solo hilo principal */
static int    g_clockOn;
static int    g_useAudio;      /* el reloj lo lleva el audio */
static DWORD  g_videoReadyTick;
static double g_clockBase;
static DWORD  g_clockTick;
static double g_clock = -1.0;
static long   g_rebuffers;
static int    g_ended;
static int    g_showStats = 0;  /* el panel de datos se ve con X */
static int    g_paused;
static double g_seekDelta;      /* segundos pedidos con la cruceta / LB / RB */
static DWORD  g_seekTick;
static DWORD  g_osdTick;        /* hasta cuando se ve la barra de progreso */
static double g_lengthSec;      /* duracion del video */
static double g_startSec;       /* desde donde se pidio empezar (busqueda) */
static volatile double g_videoStartPts;  /* donde empieza de verdad (inicio del fragmento) */
static int    g_movedThread;

/* Pistas de audio que se cambian con Y (5d). Las llena play_set_tracks y
 * sobreviven a los reinicios por busqueda. */
#define MAX_TRACKS 6
static YtFormat g_tracks[MAX_TRACKS];
static char     g_trackNames[MAX_TRACKS][80];
static int      g_trackCount;
static int      g_trackCur;           /* la que suena */
static int      g_trackWant = -1;     /* elegida con Y; se aplica tras una pausa corta */
static DWORD    g_trackTick;

/* Subtitulos que se cambian con la cruceta abajo (5d parte 2). Los baja
 * subs.c; no se vuelven a bajar al buscar ni al cambiar de audio. */
static YtCaption g_caps[YT_MAX_CAPTIONS];
static int       g_capCount;
static int       g_capCur = -1;       /* -1 = apagados */

/* Aviso arriba a la derecha (cambios de audio/subtitulos, avisos) */
static char      g_toast[160];
static DWORD     g_toastTick;

/* Datos del video para la capa de controles */
static char      g_infoChannel[128];
static char      g_infoViews[64];
static char      g_infoPublished[48];
static char      g_infoAvatar[320];
static char      g_infoId[16];        /* videoId (me gusta / no me gusta) */
static YtVideo   g_infoVideo;         /* el video completo (para recomendarlo) */

static void show_toast(const char *text);
static int       g_infoHasChannel;

/* Calidades (todas las compatibles); se cambia reiniciando en el mismo
 * punto, como el audio */
#define MAX_QUALS 12
static YtFormat  g_quals[MAX_QUALS];
static char      g_qualNames[MAX_QUALS][24];
static int       g_qualCount;
static int       g_qualCur;
static int       g_qualDef = -1;      /* la predeterminada (480p o menos) */
static int       g_qualWant = -1;

/* Capa de controles (estilo YouTube TV / SmartTube): titulo, dos filas de
 * botones y la barra de progreso. Se maneja con la cruceta. */
/* filas de la capa: cerrar (arriba a la izquierda), controles (al centro,
 * encima de la barra), barra y la fila de abajo */
enum { ROW_CLOSE, ROW_TOP, ROW_BAR, ROW_BOTTOM };
enum { BT_REW, BT_PLAY, BT_FWD, BT_COUNT };
enum { BB_QUALITY, BB_CHANNEL, BB_LIKE, BB_DISLIKE, BB_SUBSCRIBE, BB_STAR, BB_SUBS, BB_AUDIO, BB_INFO, BB_COUNT };
enum { MENU_NONE, MENU_QUALITY, MENU_SUBS, MENU_AUDIO };
#define OVL_HIDE_MS 5000
static int       g_ovl;               /* 1 = a la vista */
static DWORD     g_ovlTick;           /* ultimo boton tocado */
static int       g_row = ROW_TOP;
static int       g_col = BT_PLAY;
static int       g_menu = MENU_NONE;
static int       g_menuSel;
static int      g_oldCore;
/* Mensaje que se ve mientras se espera a que terminen los hilos al
 * detener (lo pone do_seek: "Buscando 11:33...", "Cambiando el audio...") */
static char     g_waitMsg[160];
static int    g_oldPrio;
static DWORD  g_startTick;

/* ------------------------------------------------------------------ */

static void set_error(const char *msg)
{
    if (g_hasErr)
        return;
    strncpy(g_err, msg, sizeof(g_err) - 1);
    g_err[sizeof(g_err) - 1] = 0;
    __lwsync();
    g_hasErr = 1;
    report_error("reproductor", "%s (%s, itag %d)", msg, g_title, g_fmt.itag);
}

static int frag_count(void)
{
    int n;
    EnterCriticalSection(&g_fragLock);
    n = g_fragCount;
    LeaveCriticalSection(&g_fragLock);
    return n;
}

/* ------------------------------------------------------------------ */
/* Hilo de descarga                                                    */

static DWORD WINAPI dl_thread(LPVOID param)
{
    char err[160];
    char msg[192];
    unsigned bytes = 0;
    int seg, tries, ok, slot;
    const FmpSegment *s;
    HttpResponse r;
    DWORD t0;

    (void)param;
    XSetThreadProcessor(GetCurrentThread(), 2);
    gv_diag_thread_name("descarga");
    net_set_cancel(&g_stop);    /* al detener, la peticion en curso se corta sola */

    if (!media_load_init(&g_fmt, &g_track, &bytes, err, sizeof(err)))
    {
        _snprintf(msg, sizeof(msg) - 1, "Encabezado: %s", err);
        msg[sizeof(msg) - 1] = 0;
        set_error(msg);
        __lwsync();
        g_initDone = -1;
        g_dlDone = 1;
        return 0;
    }
    if (!g_track.isVideo || g_track.segmentCount <= 0 || !g_track.spsLen || !g_track.ppsLen)
    {
        set_error("El formato no es H.264 fragmentado con indice");
        __lwsync();
        g_initDone = -1;
        g_dlDone = 1;
        return 0;
    }
    InterlockedExchangeAdd(&g_bytes, (LONG)bytes);

    /* Fragmento desde el que se empieza (al buscar). Cada fragmento de
     * YouTube empieza con un cuadro clave, asi que se puede arrancar en
     * cualquiera. */
    {
        double ts = g_track.sidxTimescale ? (double)g_track.sidxTimescale : (double)g_track.timescale;
        double acc = 0.0, d;
        int k;

        seg = 0;
        for (k = 0; k < g_track.segmentCount; ++k)
        {
            d = ts > 0.0 ? g_track.segments[k].duration / ts : 0.0;
            if (acc + d > g_startSec || k == g_track.segmentCount - 1)
                break;
            acc += d;
        }
        seg = k;
        g_videoStartPts = acc;
        if (g_lengthSec <= 0.0)
        {
            double total = 0.0;
            for (k = 0; k < g_track.segmentCount; ++k)
                total += ts > 0.0 ? g_track.segments[k].duration / ts : 0.0;
            g_lengthSec = total;
        }
        g_segLoaded = seg;
        g_segFed = seg;
    }
    __lwsync();
    g_initDone = 1;

    while (seg < g_track.segmentCount && !g_stop)
    {
        if (frag_count() >= FRAG_SLOTS)
        {
            Sleep(20);
            continue;
        }

        s = &g_track.segments[seg];
        t0 = GetTickCount();
        g_dlTick = t0;
        g_dlBusy = 1;
        ok = 0;
        for (tries = 0; tries < FETCH_TRIES && !ok && !g_stop; ++tries)
            ok = media_fetch_range(g_fmt.url, s->offset, s->offset + s->size - 1.0, &r,
                                   err, sizeof(err));
        g_dlTick = GetTickCount();
        g_dlBusy = 0;
        if (!ok)
        {
            if (!g_stop)
            {
                _snprintf(msg, sizeof(msg) - 1, "Fragmento %d: %s", seg + 1, err);
                msg[sizeof(msg) - 1] = 0;
                set_error(msg);
            }
            break;
        }
        InterlockedExchangeAdd(&g_dlMs, (LONG)(GetTickCount() - t0));
        InterlockedExchangeAdd(&g_bytes, (LONG)r.bodyLen);

        EnterCriticalSection(&g_fragLock);
        slot = (g_fragHead + g_fragCount) % FRAG_SLOTS;
        g_frags[slot].r = r;
        g_frags[slot].seg = seg;
        ++g_fragCount;
        LeaveCriticalSection(&g_fragLock);

        ++seg;
        g_segLoaded = seg;
    }

    __lwsync();
    g_dlDone = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hilo del alimentador                                                */

/* Saca el siguiente fragmento. 1 si hay, 0 si se acabaron o hay que parar. */
static int next_fragment(FragSlot *out)
{
    LONG done;

    while (!g_stop)
    {
        done = g_dlDone;
        __lwsync();
        EnterCriticalSection(&g_fragLock);
        if (g_fragCount > 0)
        {
            *out = g_frags[g_fragHead];
            g_fragHead = (g_fragHead + 1) % FRAG_SLOTS;
            --g_fragCount;
            LeaveCriticalSection(&g_fragLock);
            return 1;
        }
        LeaveCriticalSection(&g_fragLock);
        if (done)
            return 0;
        Sleep(10);
    }
    return 0;
}

static DWORD WINAPI feed_thread(LPVOID param)
{
    FragSlot fs;
    int n, i, need, got, r = 1, first = 1;
    const unsigned char *body;
    char msg[96];
    GvAuInfo ai;

    (void)param;
    XSetThreadProcessor(GetCurrentThread(), 3);
    gv_diag_thread_name("alimentador");

    while (!g_stop && g_decReady == 0)
        Sleep(10);
    if (g_decReady < 0)
        goto done;
    __lwsync();

    while (r >= 0 && next_fragment(&fs))
    {
        body = (const unsigned char *)fs.r.body;
        n = fmp_fragment_samples(body, fs.r.bodyLen, &g_track, g_samples, MAX_SAMPLES);
        if (n < 0)
        {
            _snprintf(msg, sizeof(msg) - 1, "Fragmento %d no se pudo leer", fs.seg + 1);
            msg[sizeof(msg) - 1] = 0;
            set_error(msg);
            http_free(&fs.r);
            break;
        }

        for (i = 0; i < n && !g_stop; ++i)
        {
            /* SPS y PPS solo en el primer cuadro (es clave) */
            need = avc_to_annexb(&g_track, body + g_samples[i].offset, (int)g_samples[i].size,
                                 first, NULL, 0);
            if (need < 0)
                continue;   /* cuadro dañado: se salta */
            if (need > g_auCap)
            {
                free(g_au);
                g_auCap = need + 64 * 1024;
                g_au = (unsigned char *)malloc(g_auCap);
                if (!g_au)
                {
                    g_auCap = 0;
                    set_error("Sin memoria para los cuadros");
                    r = -1;
                    break;
                }
            }
            got = avc_to_annexb(&g_track, body + g_samples[i].offset, (int)g_samples[i].size,
                                first, g_au, g_auCap);
            if (got < 0)
                continue;
            first = 0;

            /* de donde viene el cuadro (para el diagnostico de atascos) */
            ai.frag = fs.seg + 1;
            ai.sample = i;
            avc_describe_sample(&g_track, body + g_samples[i].offset, (int)g_samples[i].size,
                                ai.desc, sizeof(ai.desc));

            /* gv_feed devuelve 0 si vamos muy adelantados: esperar */
            for (;;)
            {
                r = gv_feed(g_au, got, g_samples[i].pts, g_samples[i].key, &ai);
                if (r != 0 || g_stop)
                    break;
                Sleep(4);
            }
            if (r < 0)
                break;
        }

        http_free(&fs.r);
        InterlockedIncrement(&g_segFed);
    }

done:
    free(g_au);
    g_au = NULL;
    g_auCap = 0;
    __lwsync();
    g_feedDone = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hilo de audio: baja, decodifica (aac.c) y manda a XAudio2 (audio.cpp) */

static void audio_fail(const char *msg)
{
    report_error("audio", "%s (%s, itag %d)", msg, g_title, g_afmt.itag);
    strncpy(g_audErr, msg, sizeof(g_audErr) - 1);
    g_audErr[sizeof(g_audErr) - 1] = 0;
    __lwsync();
    g_audState = -1;
}

static DWORD WINAPI audio_thread(LPVOID param)
{
    char err[160], msg[192];
    unsigned bytes = 0;
    AacDec *dec = NULL;
    HttpResponse r;
    const FmpSegment *sg;
    const unsigned char *body;
    int seg, n, i, k, got, tries, ok, opened = 0, first = 0;
    double startPts;

    (void)param;
    /* nucleo 2 (con la descarga del video): el 4 lo usa el decodificador */
    XSetThreadProcessor(GetCurrentThread(), 2);
    gv_diag_thread_name("audio");
    net_set_cancel(&g_stop);

    if (!media_load_init(&g_afmt, &g_atrack, &bytes, err, sizeof(err)))
    {
        _snprintf(msg, sizeof(msg) - 1, "Audio: %s", err);
        msg[sizeof(msg) - 1] = 0;
        audio_fail(msg);
        return 0;
    }
    InterlockedExchangeAdd(&g_audBytes, (LONG)bytes);
    if (g_atrack.isVideo || g_atrack.ascLen <= 0 || g_atrack.segmentCount <= 0)
    {
        audio_fail("Audio: no es AAC fragmentado");
        goto done;
    }
    dec = aac_open(g_atrack.asc, g_atrack.ascLen, g_atrack.sampleRate, g_atrack.channels,
                   err, sizeof(err));
    if (!dec)
    {
        _snprintf(msg, sizeof(msg) - 1, "Audio: %s", err);
        msg[sizeof(msg) - 1] = 0;
        audio_fail(msg);
        goto done;
    }

    /* Se empieza donde empieza el video (al buscar, en el inicio del
     * fragmento de video que toco) */
    while (!g_stop && g_initDone == 0)
        Sleep(10);
    __lwsync();
    startPts = g_initDone == 1 ? g_videoStartPts : g_startSec;
    {
        double ts = g_atrack.sidxTimescale ? (double)g_atrack.sidxTimescale : (double)g_atrack.timescale;
        double acc = 0.0, d;

        for (first = 0; first < g_atrack.segmentCount - 1; ++first)
        {
            d = ts > 0.0 ? g_atrack.segments[first].duration / ts : 0.0;
            if (acc + d > startPts)
                break;
            acc += d;
        }
    }

    for (seg = first; seg < g_atrack.segmentCount && !g_stop; ++seg)
    {
        sg = &g_atrack.segments[seg];
        g_auTick = GetTickCount();
        g_auWhat = 2;
        ok = 0;
        for (tries = 0; tries < FETCH_TRIES && !ok && !g_stop; ++tries)
            ok = media_fetch_range(g_afmt.url, sg->offset, sg->offset + sg->size - 1.0, &r,
                                   err, sizeof(err));
        if (!ok)
        {
            if (!g_stop)
            {
                _snprintf(msg, sizeof(msg) - 1, "Audio fragmento %d: %s", seg + 1, err);
                msg[sizeof(msg) - 1] = 0;
                audio_fail(msg);
            }
            break;
        }
        InterlockedExchangeAdd(&g_audBytes, (LONG)r.bodyLen);
        body = (const unsigned char *)r.body;
        n = fmp_fragment_samples(body, r.bodyLen, &g_atrack, g_asamples, MAX_ASAMPLES);

        for (i = 0; i < n && !g_stop; ++i)
        {
            if (g_auWhat != 1)
            {
                g_auTick = GetTickCount();
                g_auWhat = 1;
            }
            got = aac_decode(dec, body + g_asamples[i].offset, (int)g_asamples[i].size,
                             g_pcm, PCM_CAP);
            if (got <= 0)
                continue;   /* cuadro dañado o sin sonido: se salta */

            /* antes de donde empieza el video: se decodifica (el AAC
             * necesita el cuadro anterior) pero no se oye */
            if (!opened && i + 1 < n && g_asamples[i + 1].pts <= startPts)
                continue;

            if (!opened)
            {
                /* la voz se crea con lo que dice el decodificador (HE-AAC
                 * puede doblar la frecuencia) */
                if (!au_open(aac_rate(dec), aac_channels(dec), g_asamples[i].pts))
                {
                    _snprintf(msg, sizeof(msg) - 1, "Audio: %s", au_status());
                    msg[sizeof(msg) - 1] = 0;
                    audio_fail(msg);
                    http_free(&r);
                    goto done;
                }
                _snprintf(g_audInfo, sizeof(g_audInfo) - 1, "AAC %d Hz, %d canales",
                          aac_rate(dec), aac_channels(dec));
                g_audInfo[sizeof(g_audInfo) - 1] = 0;
                opened = 1;
                __lwsync();
                g_audState = 1;
            }

            for (;;)
            {
                k = au_write(g_pcm, got);
                if (k != 0 || g_stop)
                    break;
                if (g_auWhat != 0)
                {
                    g_auTick = GetTickCount();
                    g_auWhat = 0;
                }
                Sleep(10);   /* la cola esta llena: ~3 s adelante */
            }
            if (k < 0)
                break;
        }
        http_free(&r);
        g_audFrag = seg + 1;
    }

    if (opened && !g_stop)
    {
        au_flush();
        __lwsync();
        g_audState = 2;
    }

done:
    if (dec)
        aac_close(dec);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Diagnostico de atascos (4f, paso 1): lo que play.c sabe y gpuvid no */

/* Linea de contexto que gpuvid agrega a cada evento */
static void diag_context(char *buf, int size)
{
    double mbps = g_dlMs > 0 ? (double)g_bytes * 8.0 / ((double)g_dlMs * 1000.0) : 0.0;

    static const char *auw[3] = { "cola llena", "decodificando", "bajando" };
    DWORD now = GetTickCount();

    _snprintf(buf, size - 1,
              "contexto: fragmentos bajados hasta el %ld, alimentados hasta el %ld, en espera %d; "
              "descarga %s (hace %lu ms); audio %s (hace %lu ms), en cola %.1f s; red %.1f Mbit/s; recargas %ld",
              (long)g_segLoaded, (long)g_segFed, frag_count(),
              g_dlBusy ? "bajando" : "esperando", (unsigned long)(now - g_dlTick),
              auw[g_auWhat >= 0 && g_auWhat <= 2 ? g_auWhat : 0], (unsigned long)(now - g_auTick),
              g_hasAudio ? au_buffered() : 0.0, mbps, g_rebuffers);
    buf[size - 1] = 0;
}

static void log_session_start(void)
{
    char params[320];
    char line[1400];
    SYSTEMTIME t;
    double avg;
    int len = (int)g_lengthSec, at = (int)g_videoStartPts;

    GetLocalTime(&t);
    avc_describe_params(g_track.sps, g_track.spsLen, g_track.pps, g_track.ppsLen,
                        params, sizeof(params));
    avg = g_track.segmentCount > 0 && len > 0 ? (double)len / g_track.segmentCount : 0.0;
    _snprintf(line, sizeof(line) - 1,
              "\n========== %04d-%02d-%02d %02d:%02d:%02d ==========\n"
              "Video: %s\n"
              "itag %d  %dx%d  H.264 %s %d.%d  %d fragmentos (~%.1f s c/u)  duracion %d:%02d  "
              "empieza en %d:%02d\n"
              "%s\n"
              "Audio: %s\n"
              "%s  (prueba %d: %s)\n",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, g_title,
              g_fmt.itag, g_track.width, g_track.height, fmp_profile_name(g_track.profile),
              g_track.level / 10, g_track.level % 10, g_track.segmentCount, avg,
              len / 60, len % 60, at / 60, at % 60, params,
              !g_hasAudio ? "sin audio" : (g_trackCount > 0 ? g_trackNames[g_trackCur] : "pista unica"),
              gv_status(), gv_test_mode(), gv_test_mode_name(gv_test_mode()));
    line[sizeof(line) - 1] = 0;
    gv_log(line);
}

/* ------------------------------------------------------------------ */
/* API                                                                 */

int play_active(void)
{
    return g_active ? 1 : 0;
}

static void start_at(const YtFormat *f, const YtFormat *audio, const char *title,
                     double lengthSec, double startSec);

void play_start(const YtFormat *f, const YtFormat *audio, const char *title, double lengthSec)
{
    /* la capa aparece unos segundos al empezar, con el boton de pausa elegido */
    g_ovl = 1;
    g_ovlTick = GetTickCount();
    g_row = ROW_TOP;
    g_col = BT_PLAY;
    g_menu = MENU_NONE;
    g_showStats = 0;
    g_toastTick = 0;
    g_qualWant = -1;
    start_at(f, audio, title, lengthSec, 0.0);
}

void play_set_info(const YtVideo *v)
{
    if (v)
        g_infoVideo = *v;
    else
        memset(&g_infoVideo, 0, sizeof(g_infoVideo));
    strncpy(g_infoChannel, v ? v->channel : "", sizeof(g_infoChannel) - 1);
    g_infoChannel[sizeof(g_infoChannel) - 1] = 0;
    strncpy(g_infoViews, v ? v->views : "", sizeof(g_infoViews) - 1);
    g_infoViews[sizeof(g_infoViews) - 1] = 0;
    strncpy(g_infoPublished, v ? v->published : "", sizeof(g_infoPublished) - 1);
    g_infoPublished[sizeof(g_infoPublished) - 1] = 0;
    strncpy(g_infoAvatar, v ? v->avatar : "", sizeof(g_infoAvatar) - 1);
    g_infoAvatar[sizeof(g_infoAvatar) - 1] = 0;
    g_infoHasChannel = v && v->channelId[0];
    strncpy(g_infoId, v && v->kind != YT_KIND_CHANNEL ? v->id : "", sizeof(g_infoId) - 1);
    g_infoId[sizeof(g_infoId) - 1] = 0;
    cuenta_rating_request(g_infoId);   /* con sesion de Google: "me gusta" ya marcado? */
}

/* Suscribirse al canal del video (cancelar pide A dos veces) */
static DWORD g_unsubArm = 0;

static void subscribe_press(void)
{
    char buf[200];
    const char *ch = g_infoVideo.channelId;
    int st;

    if (!cuenta_signed_in())
    {
        show_toast(T(S_SUB_NEED));
        return;
    }
    if (strncmp(ch, "UC", 2) != 0)
        return;
    st = cuenta_sub_state(ch);
    if (st == SUB_UNKNOWN)
        return;
    if (st == SUB_YES)
    {
        if (!g_unsubArm || GetTickCount() - g_unsubArm > 3000)
        {
            g_unsubArm = GetTickCount();
            show_toast(T(S_UNSUB_CONFIRM));
            return;
        }
        g_unsubArm = 0;
        cuenta_sub_set(ch, 0);
        _snprintf(buf, sizeof(buf) - 1, T(S_UNSUB_DONE), g_infoChannel);
    }
    else
    {
        cuenta_sub_set(ch, 1);
        _snprintf(buf, sizeof(buf) - 1, T(S_SUB_DONE), g_infoChannel);
    }
    buf[sizeof(buf) - 1] = 0;
    show_toast(buf);
}

/* Estrella: recomendar este video a la comunidad */
static void star_press(void)
{
    char buf[160];
    int wait;

    if (!community_enabled() || !g_infoId[0])
        return;
    if (community_is_recommended(g_infoId))
    {
        show_toast(T(S_STAR_ON));
        return;
    }
    if ((wait = community_wait_secs()) > 0)
    {
        _snprintf(buf, sizeof(buf) - 1, T(S_STAR_WAIT), wait);
        buf[sizeof(buf) - 1] = 0;
        show_toast(buf);
        return;
    }
    community_recommend(&g_infoVideo);
}

/* Me gusta / no me gusta: A sobre el mismo que ya estaba lo quita */
static void rate_press(int which)
{
    int cur;

    if (!cuenta_signed_in())
    {
        show_toast(T(S_RATE_NEED));
        return;
    }
    if (!g_infoId[0])
        return;
    cur = cuenta_rating(g_infoId);
    cuenta_rate(g_infoId, cur == which ? RATE_NONE : which);
}

void play_set_qualities(const YtFormat **fmts, const char **names, int count, int current, int def)
{
    int i;

    if (count > MAX_QUALS)
        count = MAX_QUALS;
    if (count < 0)
        count = 0;
    for (i = 0; i < count; ++i)
    {
        memcpy(&g_quals[i], fmts[i], sizeof(YtFormat));
        strncpy(g_qualNames[i], names[i] ? names[i] : "", sizeof(g_qualNames[i]) - 1);
        g_qualNames[i][sizeof(g_qualNames[i]) - 1] = 0;
    }
    g_qualCount = count;
    g_qualCur = (current >= 0 && current < count) ? current : 0;
    g_qualDef = (def >= 0 && def < count) ? def : -1;
    g_qualWant = -1;
}

static void start_at(const YtFormat *f, const YtFormat *audio, const char *title,
                     double lengthSec, double startSec)
{
    char msg[192];

    if (g_active)
        return;

    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_fragLock);
        g_lockReady = 1;
    }

    memcpy(&g_fmt, f, sizeof(g_fmt));
    strncpy(g_title, title ? title : "", sizeof(g_title) - 1);
    g_title[sizeof(g_title) - 1] = 0;
    memset(&g_track, 0, sizeof(g_track));
    g_stop = 0;
    g_initDone = 0;
    g_decReady = 0;
    g_dlDone = 0;
    g_feedDone = 0;
    g_bytes = 0;
    g_dlMs = 0;
    g_segLoaded = 0;
    g_segFed = 0;
    g_hasErr = 0;
    g_err[0] = 0;
    g_fragHead = g_fragCount = 0;
    g_clockOn = 0;
    g_clock = -1.0;
    g_rebuffers = 0;
    g_ended = 0;
    g_movedThread = 0;
    g_startTick = GetTickCount();
    g_dlThread = NULL;
    g_feedThread = NULL;
    g_audThread = NULL;
    g_useAudio = 0;
    g_videoReadyTick = 0;
    g_hasAudio = audio != NULL;
    if (audio)
        memcpy(&g_afmt, audio, sizeof(g_afmt));
    memset(&g_atrack, 0, sizeof(g_atrack));
    g_audState = audio ? 0 : -1;
    g_audFrag = 0;
    g_audBytes = 0;
    g_audErr[0] = 0;
    g_audInfo[0] = 0;
    g_lengthSec = lengthSec;
    g_startSec = startSec < 0.0 ? 0.0 : startSec;
    g_videoStartPts = g_startSec;
    g_paused = 0;
    g_seekDelta = 0.0;
    g_osdTick = GetTickCount() + 3000;
    g_holeNoted = 0;
    g_dlBusy = 0;
    g_dlTick = g_auTick = GetTickCount();
    g_auWhat = 0;
    g_active = 1;
    gv_set_diag_hook(diag_context);

    if (!gv_available())
    {
        _snprintf(msg, sizeof(msg) - 1, "Decodificador de la consola no disponible: %s", gv_status());
        msg[sizeof(msg) - 1] = 0;
        set_error(msg);
        g_initDone = -1;
        g_decReady = -1;
        return;
    }

    g_dlThread = CreateThread(NULL, 256 * 1024, dl_thread, NULL, 0, NULL);
    g_feedThread = CreateThread(NULL, 64 * 1024, feed_thread, NULL, 0, NULL);
    if (g_hasAudio)
        g_audThread = CreateThread(NULL, 256 * 1024, audio_thread, NULL, 0, NULL);
    if (!g_dlThread || !g_feedThread)
    {
        set_error("No se pudieron crear los hilos");
        g_stop = 1;
    }
}

/* Posicion actual en segundos (para la barra y la busqueda) */
static double current_pos(double curPts)
{
    if (g_clock >= 0.0)
        return g_clock;
    if (curPts >= 0.0)
        return curPts;
    return g_videoStartPts;
}

static void format_time(double sec, char *out, int size)
{
    int s = sec < 0.0 ? 0 : (int)sec;
    if (s >= 3600)
        _snprintf(out, size - 1, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else
        _snprintf(out, size - 1, "%d:%02d", s / 60, s % 60);
    out[size - 1] = 0;
}

/* Saltar o cambiar de pista de audio: se detiene todo y se vuelve a
 * empezar desde el fragmento que contiene la nueva posicion (como un
 * reinicio, pero a proposito). Al cambiar de pista se repiten unos
 * segundos del video: empieza en el inicio del fragmento. */
static void do_seek(double curPts)
{
    static YtFormat vf, af;
    static char title[256];
    double target, len = g_lengthSec;
    int hasAudio = g_hasAudio;
    char t[32], msg[160];

    target = current_pos(curPts) + g_seekDelta;
    if (len > 0.0 && target > len - 3.0)
        target = len - 3.0;
    if (target < 0.0)
        target = 0.0;
    g_seekDelta = 0.0;

    memcpy(&vf, &g_fmt, sizeof(vf));
    msg[0] = 0;
    if (g_qualWant >= 0 && g_qualWant < g_qualCount)
    {
        memcpy(&vf, &g_quals[g_qualWant], sizeof(vf));
        g_qualCur = g_qualWant;
        _snprintf(msg, sizeof(msg) - 1, T(S_CHANGING_TO), g_qualNames[g_qualCur]);
    }
    g_qualWant = -1;
    if (g_trackWant >= 0 && g_trackWant < g_trackCount)
    {
        memcpy(&af, &g_tracks[g_trackWant], sizeof(af));
        hasAudio = 1;
        g_trackCur = g_trackWant;
        g_trackWant = -1;
        if (!msg[0])
            _snprintf(msg, sizeof(msg) - 1, T(S_AUDIO_TO), g_trackNames[g_trackCur]);
    }
    else
    {
        g_trackWant = -1;
        if (hasAudio)
            memcpy(&af, &g_afmt, sizeof(af));
        if (!msg[0])
        {
            format_time(target, t, sizeof(t));
            _snprintf(msg, sizeof(msg) - 1, T(S_SEEKING), t);
        }
    }
    strcpy(title, g_title);
    msg[sizeof(msg) - 1] = 0;
    strcpy(g_waitMsg, msg);
    gfx_begin(0xFF000000);
    gfx_text((SCREEN_W - gfx_text_width(1.0f, msg)) * 0.5f, 380.0f, 1.0f, COL_SOFT, msg);
    ui_draw_spinner(SCREEN_W * 0.5f, 320.0f, 24.0f);
    gfx_end();

    play_stop();
    start_at(&vf, hasAudio ? &af : NULL, title, len, target);
}

void play_update(void)
{
    GvStats st;
    char msg[192];

    if (!g_active)
        return;

    /* El decodificador se abre desde este hilo cuando ya conocemos el nivel */
    if (g_initDone == 1 && g_decReady == 0)
    {
        __lwsync();
        if (gv_open(g_track.level))
        {
            /* Como en el reproductor de HG-ERIK: el hilo que saca y dibuja
             * los cuadros va en el nucleo 5 con prioridad alta, para que
             * nunca se quede esperando el candado del decodificador.
             * (Con HIGHEST casi nunca lo conseguia y el video se congelaba.
             * El audio ya no sufre porque XAudio2 corre en el hilo 0.) */
            g_oldCore = (int)GetCurrentProcessorNumber();
            g_oldPrio = GetThreadPriority(GetCurrentThread());
            XSetThreadProcessor(GetCurrentThread(), 5);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            g_movedThread = 1;
            gv_diag_thread_name("principal");
            log_session_start();
            __lwsync();
            g_decReady = 1;
        }
        else
        {
            _snprintf(msg, sizeof(msg) - 1, "No abrio el decodificador: %s", gv_status());
            msg[sizeof(msg) - 1] = 0;
            set_error(msg);
            g_decReady = -1;
        }
    }
    if (g_initDone < 0 && g_decReady == 0)
        g_decReady = -1;   /* el alimentador deja de esperar */
    if (g_decReady != 1)
        return;

    gv_get_stats(&st);

    /* busqueda pendiente: se hace medio segundo despues del ultimo boton
     * (asi tres toques seguidos son un solo salto de 30 s) */
    if (g_seekDelta != 0.0 && GetTickCount() - g_seekTick > 450)
    {
        do_seek(st.curPts);
        return;
    }
    /* pista de audio pendiente: casi un segundo despues del ultimo Y, para
     * poder pasar de largo por las pistas sin reiniciar en cada una */
    if (g_trackWant >= 0 && g_seekDelta == 0.0 && GetTickCount() - g_trackTick > 900)
    {
        do_seek(st.curPts);
        return;
    }
    /* calidad elegida en el menu: de una vez */
    if (g_qualWant >= 0 && g_seekDelta == 0.0)
    {
        do_seek(st.curPts);
        return;
    }

    if (!g_clockOn && !g_paused)
    {
        /* arranca cuando hay unos cuadros listos (o ya no vendran mas) y,
         * si hay audio, cuando ya hay medio segundo de sonido en cola */
        if (st.curPts >= 0.0 && (st.queued >= START_FRAMES || g_feedDone))
        {
            int audioReady, audioGone;

            if (!g_videoReadyTick)
                g_videoReadyTick = GetTickCount();
            audioReady = g_audState == 2 || (g_audState == 1 && au_buffered() >= AUDIO_START);
            audioGone = g_audState < 0 || GetTickCount() - g_videoReadyTick > AUDIO_WAIT;

            if (g_useAudio && g_audState < 0)
                g_useAudio = 0;   /* el audio fallo a medio video */

            if (audioReady)
            {
                g_useAudio = 1;
                au_play(1);
                g_clockOn = 1;
            }
            else if (!g_useAudio && audioGone)
            {
                /* sin audio (no hay, fallo o tardo demasiado): reloj normal */
                g_clockBase = st.curPts;
                g_clockTick = GetTickCount();
                g_clockOn = 1;
            }
            /* si no, se espera a que el audio junte su medio segundo */
        }
    }
    else if (st.queued == 0 && !g_feedDone && st.curPts >= 0.0 &&
             g_clock > st.curPts + 0.5 && !g_holeNoted)
    {
        /* se acabaron los cuadros listos: pausa el reloj (y el audio) y
         * vuelve a juntar. Prueba 4: el reloj y el audio siguen; cuando
         * vuelven los cuadros se tiran los atrasados hasta alcanzar al audio. */
        char d[224];
        DWORD t0 = GetTickCount(), tAudio = 0;

        ++g_rebuffers;
        if (gv_test_mode() == 4)
            g_holeNoted = 1;
        else
        {
            g_clockOn = 0;
            if (g_useAudio)
                au_play(0);
            tAudio = GetTickCount() - t0;
        }
        _snprintf(d, sizeof(d) - 1, "cola de entrada del decodificador %d, fragmentos en espera %d -> %s; "
                  "pausar el audio tardo %lu ms",
                  st.inQueue, frag_count(),
                  st.inQueue == 0 ? "faltan datos (descarga o red)" : "el decodificador no sacaba cuadros",
                  (unsigned long)tAudio);
        d[sizeof(d) - 1] = 0;
        gv_diag_note('C', st.curPts, gv_test_mode() == 4
                     ? "SIN CUADROS (prueba 4: el reloj y el audio siguen)"
                     : "RECARGA (se acabaron los cuadros listos: reloj y audio en pausa)", d);
    }
    if (st.queued > 0)
        g_holeNoted = 0;

    /* el audio se acabo antes que el video (o fallo): sigue con el reloj normal */
    if (g_clockOn && g_useAudio &&
        ((g_audState == 2 && au_buffered() <= 0.01) || g_audState < 0))
    {
        g_clockBase = au_clock();
        g_clockTick = GetTickCount();
        g_useAudio = 0;
    }

    if (!g_clockOn)
        g_clock = -1.0;
    else if (g_useAudio)
        g_clock = au_clock();
    else
        g_clock = g_clockBase + (GetTickCount() - g_clockTick) / 1000.0;
    gv_update(g_clock);

    if (g_feedDone && st.queued == 0 && g_clockOn && st.curPts >= 0.0 &&
        g_clock > st.curPts + 1.0)
        g_ended = 1;
}

static void show_toast(const char *text)
{
    strncpy(g_toast, text, sizeof(g_toast) - 1);
    g_toast[sizeof(g_toast) - 1] = 0;
    g_toastTick = GetTickCount() + 3000;
}

static void toggle_pause(void)
{
    g_paused = !g_paused;
    if (g_paused && g_clockOn)
    {
        if (g_useAudio)
            au_play(0);
        g_clockOn = 0;   /* al continuar arranca solo (ver play_update) */
    }
}

static void add_seek(double d)
{
    if (g_decReady != 1)
        return;
    g_seekDelta += d;
    g_seekTick = GetTickCount();
}

static int menu_count(int menu)
{
    if (menu == MENU_QUALITY)
        return g_qualCount;
    if (menu == MENU_SUBS)
        return g_capCount + 1;   /* "Apagados" primero */
    if (menu == MENU_AUDIO)
        return g_trackCount;
    return 0;
}

static void open_menu(int menu)
{
    if (menu_count(menu) <= 0)
        return;
    g_menu = menu;
    if (menu == MENU_QUALITY)
        g_menuSel = g_qualWant >= 0 ? g_qualWant : g_qualCur;
    else if (menu == MENU_SUBS)
        g_menuSel = g_capCur + 1;
    else
        g_menuSel = g_trackWant >= 0 ? g_trackWant : g_trackCur;
}

static void menu_choose(void)
{
    char msg[160];

    if (g_menu == MENU_QUALITY)
    {
        if (g_menuSel != g_qualCur && g_decReady == 1)
            g_qualWant = g_menuSel;   /* play_update reinicia en el mismo punto */
    }
    else if (g_menu == MENU_SUBS)
    {
        g_capCur = g_menuSel - 1;
        if (g_capCur >= 0)
        {
            subs_load(g_caps[g_capCur].url);
            _snprintf(msg, sizeof(msg) - 1, T(S_SUBS_X), g_caps[g_capCur].name);
        }
        else
        {
            subs_clear();
            _snprintf(msg, sizeof(msg) - 1, "%s", T(S_SUBS_OFF));
        }
        msg[sizeof(msg) - 1] = 0;
        show_toast(msg);
    }
    else if (g_menu == MENU_AUDIO)
    {
        if (g_menuSel != g_trackCur && g_decReady == 1)
        {
            g_trackWant = g_menuSel;
            g_trackTick = GetTickCount() - 1000;   /* sin esperar */
        }
    }
    g_menu = MENU_NONE;
}

/* Boton elegido con A */
static int press_button(void)
{
    if (g_row == ROW_BAR)
    {
        toggle_pause();
        return PLAY_ACT_NONE;
    }
    if (g_row == ROW_CLOSE)
        return PLAY_ACT_STOP;
    if (g_row == ROW_TOP)
    {
        switch (g_col)
        {
        case BT_PLAY:  toggle_pause();  break;
        case BT_REW:   add_seek(-10.0); break;
        case BT_FWD:   add_seek(10.0);  break;
        }
        return PLAY_ACT_NONE;
    }
    switch (g_col)
    {
    case BB_QUALITY:
        open_menu(MENU_QUALITY);
        break;
    case BB_CHANNEL:
        if (g_infoHasChannel)
            return PLAY_ACT_CHANNEL;
        show_toast(T(S_NO_CHAN_V));
        break;
    case BB_SUBS:
        if (g_capCount > 0)
            open_menu(MENU_SUBS);
        else
            show_toast(T(S_NO_SUBS));
        break;
    case BB_AUDIO:
        if (g_trackCount > 1)
            open_menu(MENU_AUDIO);
        else
            show_toast(T(S_ONE_TRACK));
        break;
    case BB_LIKE:
        rate_press(RATE_LIKE);
        break;
    case BB_DISLIKE:
        rate_press(RATE_DISLIKE);
        break;
    case BB_STAR:
        star_press();
        break;
    case BB_SUBSCRIBE:
        subscribe_press();
        break;
    case BB_INFO:
        g_showStats = !g_showStats;
        break;
    }
    return PLAY_ACT_NONE;
}

int play_input(unsigned short pressed)
{
    int act = PLAY_ACT_NONE;
    int cols;

    if (!g_active)
        return PLAY_ACT_NONE;
    if (pressed)
        g_ovlTick = GetTickCount();

    /* atajos que funcionan siempre */
    if (pressed & XINPUT_GAMEPAD_X)
        g_showStats = !g_showStats;
    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
        add_seek(60.0);
    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
        add_seek(-60.0);
    if ((pressed & XINPUT_GAMEPAD_Y) && g_trackCount > 1 && g_decReady == 1)
    {
        /* Y: siguiente pista de audio (se aplica casi un segundo despues) */
        int next = ((g_trackWant >= 0 ? g_trackWant : g_trackCur) + 1) % g_trackCount;
        char msg[160];

        g_trackWant = (next == g_trackCur) ? -1 : next;
        g_trackTick = GetTickCount();
        _snprintf(msg, sizeof(msg) - 1, T(S_AUDIO_X), g_trackNames[g_trackWant >= 0 ? g_trackWant : g_trackCur]);
        msg[sizeof(msg) - 1] = 0;
        show_toast(msg);
    }

    /* capa escondida: A pausa (y la muestra), izq/der salta 10 s, B sale */
    if (!g_ovl)
    {
        if (pressed & XINPUT_GAMEPAD_B)
            return PLAY_ACT_STOP;
        if (pressed & XINPUT_GAMEPAD_A)
        {
            toggle_pause();
            g_ovl = 1;
            g_row = ROW_TOP;
            g_col = BT_PLAY;
        }
        else if (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))
        {
            add_seek((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ? 10.0 : -10.0);
            g_ovl = 1;
            g_row = ROW_BAR;
        }
        else if (pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                            XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
        {
            g_ovl = 1;
            g_row = ROW_TOP;
            g_col = BT_PLAY;
        }
        return PLAY_ACT_NONE;
    }

    /* menu abierto (calidad, subtitulos o audio) */
    if (g_menu != MENU_NONE)
    {
        int n = menu_count(g_menu);
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) && g_menuSel > 0)
            --g_menuSel;
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) && g_menuSel < n - 1)
            ++g_menuSel;
        if (pressed & XINPUT_GAMEPAD_A)
            menu_choose();
        else if (pressed & XINPUT_GAMEPAD_B)
            g_menu = MENU_NONE;
        return PLAY_ACT_NONE;
    }

    /* capa a la vista: la cruceta mueve entre botones y barra */
    if (pressed & XINPUT_GAMEPAD_B)
    {
        g_ovl = 0;
        return PLAY_ACT_NONE;
    }
    if ((pressed & XINPUT_GAMEPAD_DPAD_UP) && g_row > ROW_CLOSE)
    {
        --g_row;
        if (g_row == ROW_TOP)
            g_col = BT_PLAY;   /* al subir se llega al de pausa (el del centro) */
    }
    if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) && g_row < ROW_BOTTOM)
    {
        ++g_row;
        if (g_row == ROW_TOP)
            g_col = BT_PLAY;
        else if (g_row == ROW_BOTTOM)
            g_col = 0;
    }
    cols = g_row == ROW_CLOSE ? 1 : g_row == ROW_TOP ? BT_COUNT : BB_COUNT;
    if (g_col >= cols)
        g_col = cols - 1;
    if (g_row == ROW_BAR)
    {
        if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)
            add_seek(10.0);
        if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)
            add_seek(-10.0);
    }
    else
    {
        if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) && g_col < cols - 1)
            ++g_col;
        if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) && g_col > 0)
            --g_col;
    }
    if (pressed & XINPUT_GAMEPAD_A)
        act = press_button();
    return act;
}

void play_set_tracks(const YtFormat **audios, const char **names, int count, int current)
{
    int i;

    if (count > MAX_TRACKS)
        count = MAX_TRACKS;
    if (count < 0)
        count = 0;
    for (i = 0; i < count; ++i)
    {
        memcpy(&g_tracks[i], audios[i], sizeof(YtFormat));
        strncpy(g_trackNames[i], names[i] ? names[i] : "", sizeof(g_trackNames[i]) - 1);
        g_trackNames[i][sizeof(g_trackNames[i]) - 1] = 0;
    }
    g_trackCount = count;
    g_trackCur = (current >= 0 && current < count) ? current : 0;
    g_trackWant = -1;
}

int play_current_track(void)
{
    return g_trackCur;
}

void play_set_captions(const YtCaption *caps, int count, int current)
{
    if (count > YT_MAX_CAPTIONS)
        count = YT_MAX_CAPTIONS;
    if (count < 0)
        count = 0;
    if (count > 0)
        memcpy(g_caps, caps, sizeof(YtCaption) * count);
    g_capCount = count;
    g_capCur = (current >= 0 && current < count) ? current : -1;
    if (g_capCur >= 0)
        subs_load(g_caps[g_capCur].url);
    else
        subs_clear();
}

int play_current_caption(void)
{
    return g_capCur;
}

int play_ended(void)
{
    return g_active && g_ended && !g_hasErr;
}

void play_hide_overlay(void)
{
    g_ovl = 0;
    g_menu = MENU_NONE;
    g_showStats = 0;
    g_row = ROW_TOP;
    g_col = BT_PLAY;
}

static void stats_line(float *y, unsigned int color, const char *s)
{
    char fitted[160];
    fit_text(s, 96, fitted, sizeof(fitted));
    gfx_text_mono(44.0f, *y, 1.0f, color, fitted);
    *y += 28.0f;
}

/* La capa se esconde sola si no se toca nada, salvo en pausa, con un
 * menu abierto o con el panel de datos */
static int osd_visible(void)
{
    if (g_ovl && !g_paused && g_menu == MENU_NONE && !g_showStats && g_seekDelta == 0.0 &&
        g_qualWant < 0 && GetTickCount() - g_ovlTick > OVL_HIDE_MS)
    {
        g_ovl = 0;
        g_row = ROW_TOP;
        g_col = BT_PLAY;
    }
    return g_ovl;
}

/* Texto con una sombra suave (se lee sobre cualquier parte del video) */
static float text_shadow(float x, float y, float scale, unsigned int color, const char *s)
{
    gfx_text(x + 1.0f, y + 1.0f, scale, 0x99000000, s);
    return gfx_text(x, y, scale, color, s);
}

static void text_right_shadow(float right, float y, float scale, unsigned int color, const char *s)
{
    text_shadow(right - gfx_text_width(scale, s), y, scale, color, s);
}

/* "Negrita" con la letra normal (dos pasadas a 1 px), alineada a la
 * derecha. Devuelve la x donde empieza. */
static float text_bold_right(float right, float y, float scale, unsigned int color, const char *s)
{
    float x = right - gfx_text_width(scale, s) - 1.0f;

    gfx_text(x + 1.0f, y + 1.0f, scale, 0x80000000, s);
    gfx_text(x + 2.0f, y + 1.0f, scale, 0x80000000, s);
    gfx_text(x, y, scale, color, s);
    gfx_text(x + 1.0f, y, scale, color, s);
    return x;
}

/* Aviso arriba a la derecha (debajo del reloj si la capa esta a la vista) */
static void draw_toast(void)
{
    char line[192];
    float w, y = g_ovl ? 104.0f : 40.0f;

    if ((int)(g_toastTick - GetTickCount()) <= 0)
        return;
    /* si los subtitulos fallaron, el motivo */
    if (g_capCur >= 0 && subs_state() == -1)
        _snprintf(line, sizeof(line) - 1, T(S_SUBS_X), subs_error());
    else if (g_capCur >= 0 && subs_state() == 1)
        _snprintf(line, sizeof(line) - 1, T(S_X_LOADING), g_toast);
    else
        _snprintf(line, sizeof(line) - 1, "%s", g_toast);
    line[sizeof(line) - 1] = 0;
    gfx_fit_text(line, 700.0f, 0.95f, line, sizeof(line));
    w = gfx_text_width(0.95f, line);
    gfx_rect(SCREEN_W - 50.0f - w - 24.0f, y, w + 24.0f, 34.0f, 0xC0101010);
    gfx_rect(SCREEN_W - 50.0f - w - 24.0f, y, 4.0f, 34.0f, 0xFFE62117);
    gfx_text(SCREEN_W - 50.0f - w - 10.0f, y + 5.0f, 0.95f, COL_TEXT, line);
}

/* Posiciones de la capa (pantalla de 1280x720), medidas sobre la captura
 * de SmartTube que se tomo de modelo */
#define OV_X        75.0f
#define OV_RIGHT    1203.0f
#define OV_TITLE_Y  402.0f      /* titulo: Roboto Bold 32, linea base en 432 */
#define OV_TITLE_S  1.58f
#define OV_META_Y   443.0f      /* canal - vistas - fecha: base en 462 */
#define OV_TOP_CY   523.0f      /* controles centrados encima de la barra */
#define OV_TOP_STEP 92.0f
#define OV_PLAY_SZ  68.0f       /* el de pausa/reproducir, mas grande */
#define OV_CLOSE_CX 93.0f       /* cerrar: esquina de arriba a la izquierda */
#define OV_CLOSE_CY 62.0f
#define OV_BAR_Y    569.0f      /* barra de 8 px con puntas redondas */
#define OV_BAR_H    8.0f
#define OV_BAR_X0   81.0f
#define OV_BAR_X1   1199.0f
#define OV_BOT_CY   624.0f
#define OV_STEP     85.0f
#define OV_ICON     56.0f
#define OV_INFO_S   1.2f        /* columna de datos de la derecha */
#define COL_INFO    0xFFAEAEAE
#define COL_TITLE   0xFFF0F0F0
#define COL_TRACK   0xFF888888  /* barra: lo que falta, lo bajado y lo visto */
#define COL_LOADED  0xFFFF8080
#define COL_PLAYED  0xFFFF0000

/* Tramo de la barra de x0 a x1; las puntas que tocan los extremos van
 * redondas */
static void bar_part(float x0, float x1, unsigned int color)
{
    float r = OV_BAR_H * 0.5f;

    if (x1 <= x0)
        return;
    gfx_rect(x0, OV_BAR_Y, x1 - x0, OV_BAR_H, color);
    if (x0 <= OV_BAR_X0)
        gfx_circle(OV_BAR_X0 - r, OV_BAR_Y, OV_BAR_H, color);
    if (x1 >= OV_BAR_X1)
        gfx_circle(OV_BAR_X1 - r, OV_BAR_Y, OV_BAR_H, color);
}

static float button_cx(int col)
{
    float base = OV_X + 18.0f + col * OV_STEP;
    if (col == BB_SUBSCRIBE)   /* el boton Suscribirse es ancho: su centro */
        return base - OV_ICON * 0.5f + UI_SUB_W * 0.5f;
    if (col > BB_SUBSCRIBE)
        return base + (UI_SUB_W - OV_ICON);
    return base;
}

/* Controles del centro: -10 s, pausa, +10 s */
static float top_cx(int col)
{
    return SCREEN_W * 0.5f + (col - BT_PLAY) * OV_TOP_STEP;
}

/* Un boton de size px: orbe rojo detras si esta elegido; si no hay
 * iconos, su letra */
static void draw_button_sz(int icon, float cx, float cy, float size, int focused, int enabled,
                           const char *fallback)
{
    unsigned int col = enabled ? 0xFFFFFFFF : 0x80FFFFFF;
    float h = size * 0.5f;

    if (focused)
        gfx_icon(UI_ICON_ORB, cx - h, cy - h, size, 0xFFFFFFFF);
    if (!gfx_icon(icon, cx - h, cy - h, size, col))
    {
        if (focused)
            gfx_circle(cx - h + 2.0f, cy - h + 2.0f, size - 4.0f, 0xFFE62117);
        gfx_text(cx - gfx_text_width(0.8f, fallback) * 0.5f, cy - 10.0f, 0.8f, col, fallback);
    }
}

static void draw_button(int icon, float cx, float cy, int focused, int enabled, const char *fallback)
{
    draw_button_sz(icon, cx, cy, OV_ICON, focused, enabled, fallback);
}

/* Nombre del boton elegido, junto a el */
static void draw_tooltip(float cx, float y, const char *text)
{
    char line[160];
    float w;

    gfx_fit_text(text, 520.0f, 0.8f, line, sizeof(line));
    w = gfx_text_width(0.8f, line);
    if (cx - w * 0.5f - 10.0f < 40.0f)
        cx = 40.0f + w * 0.5f + 10.0f;
    gfx_rect(cx - w * 0.5f - 10.0f, y, w + 20.0f, 26.0f, 0xC0101010);
    gfx_text(cx - w * 0.5f, y + 3.0f, 0.8f, COL_TEXT, line);
}

/* Igual, pero empezando en x (a la derecha de un boton) */
static void draw_tooltip_at(float x, float y, const char *text)
{
    float w = gfx_text_width(0.8f, text);

    gfx_rect(x, y, w + 20.0f, 26.0f, 0xC0101010);
    gfx_text(x + 10.0f, y + 3.0f, 0.8f, COL_TEXT, text);
}


/* Menu de calidad, subtitulos o audio, arriba de su boton */
static void draw_menu(void)
{
    char title[64];
    char label[160];
    char line[160];
    int n = menu_count(g_menu), i, current;
    float itemH = 40.0f, w = g_menu == MENU_QUALITY ? 380.0f : 480.0f, h, x, y, cx;

    if (g_menu == MENU_QUALITY)
    {
        strcpy(title, T(S_QUALITY));
        cx = button_cx(BB_QUALITY);
        current = g_qualCur;
    }
    else if (g_menu == MENU_SUBS)
    {
        strcpy(title, T(S_SUBTITLES));
        cx = button_cx(BB_SUBS);
        current = g_capCur + 1;
    }
    else
    {
        strcpy(title, T(S_AUDIO_LANG));
        cx = button_cx(BB_AUDIO);
        current = g_trackCur;
    }

    h = 48.0f + n * itemH + 8.0f;
    x = cx - 40.0f;
    if (x + w > SCREEN_W - 60.0f)
        x = SCREEN_W - 60.0f - w;
    y = OV_BAR_Y - 12.0f - h;
    if (y < 40.0f)
        y = 40.0f;

    /* vidrio oscuro con brillo arriba */
    gfx_rect(x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f, 0xFF3C3C3C);
    gfx_rect(x, y, w, h, 0xF0161616);
    gfx_rect_vgrad(x, y, w, 24.0f, 0x30FFFFFF, 0x00FFFFFF);
    gfx_text(x + 16.0f, y + 12.0f, 0.9f, 0xFFB8B8B8, title);

    for (i = 0; i < n; ++i)
    {
        float iy = y + 48.0f + i * itemH;
        int on = i == g_menuSel;

        if (g_menu == MENU_QUALITY)
            strcpy(label, g_qualNames[i]);
        else if (g_menu == MENU_SUBS)
        {
            if (i == 0)
                strcpy(label, T(S_OFF));
            else
                strcpy(label, g_caps[i - 1].name);
        }
        else
            strcpy(label, g_trackNames[i]);

        if (on)
        {
            gfx_rect_vgrad(x + 6.0f, iy, w - 12.0f, itemH - 4.0f, 0xFFF0392E, 0xFFB8150D);
            gfx_rect_vgrad(x + 6.0f, iy, w - 12.0f, (itemH - 4.0f) * 0.5f, 0x38FFFFFF, 0x08FFFFFF);
        }
        if (i == current)
        {
            if (!gfx_icon(UI_ICON_CHECK, x + 12.0f, iy + 2.0f, 32.0f, 0xFFFFFFFF))
                gfx_text(x + 18.0f, iy + 8.0f, 0.9f, COL_TEXT, "*");
        }
        gfx_fit_text(label, g_menu == MENU_QUALITY ? w - 170.0f : w - 70.0f, 0.95f, line, sizeof(line));
        gfx_text(x + 52.0f, iy + 7.0f, 0.95f, on ? COL_TEXT : 0xFFD8D8D8, line);

        /* la calidad predeterminada lleva su marca en rojo */
        if (g_menu == MENU_QUALITY && i == g_qualDef)
        {
            const char *tag = T(S_DEFAULT);
            gfx_text(x + w - 18.0f - gfx_text_width(0.75f, tag), iy + 10.0f, 0.75f,
                     on ? COL_TEXT : 0xFFFF4A3E, tag);
        }
    }
}

/* Capa de controles: titulo, botones, barra de progreso y datos */
static void draw_overlay(const GvStats *st)
{
    char line[256];
    char a[32], b[32];
    SYSTEMTIME t;
    double pos, len = g_lengthSec, frac, buf = 0.0;
    float bx0 = OV_BAR_X0, bx1 = OV_BAR_X1, kx, barCy = OV_BAR_Y + OV_BAR_H * 0.5f, x;
    int i, focusTop, focusBot, height;
    long endSec;
    static const int topIcons[BT_COUNT] = { UI_ICON_REW10, UI_ICON_PAUSE, UI_ICON_FWD10 };
    /* (uno por boton, en el orden de BB_...; los que se dibujan aparte van en 0) */
    static const int botIcons[] = { UI_ICON_HQ, 0, UI_ICON_LIKE, UI_ICON_DISLIKE, 0, UI_ICON_STAR,
                                    UI_ICON_CC, UI_ICON_GLOBE, UI_ICON_INFO };
    /* si se agrega un boton y no su icono, esto no compila (error de tamano negativo) */
    typedef char check_bot_icons[(sizeof(botIcons) / sizeof(botIcons[0]) == BB_COUNT) ? 1 : -1];
    int starred = community_is_recommended(g_infoId);
    int rating = cuenta_rating(g_infoId);

    if (!osd_visible())
        return;

    /* oscurecer: todo un poco y mas hacia abajo */
    gfx_rect(0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0x30000000);
    gfx_rect_vgrad(0.0f, 300.0f, (float)SCREEN_W, (float)SCREEN_H - 300.0f, 0x00000000, 0xD8000000);

    /* donde va el video */
    pos = current_pos(st->curPts) + g_seekDelta;
    if (pos < 0.0)
        pos = 0.0;
    if (len > 0.0 && pos > len)
        pos = len;
    frac = len > 0.0 ? pos / len : 0.0;

    /* hora arriba a la derecha y, debajo, a que hora acaba el video */
    GetLocalTime(&t);
    _snprintf(line, sizeof(line) - 1, "%02d:%02d", t.wHour, t.wMinute);
    line[sizeof(line) - 1] = 0;
    text_bold_right(OV_RIGHT, 30.0f, 1.2f, 0xFFF6F6F6, line);
    if (len > 0.0)
    {
        endSec = (t.wHour * 3600L + t.wMinute * 60L + t.wSecond + (long)(len - pos + 0.5)) % 86400L;
        _snprintf(line, sizeof(line) - 1, "%02ld:%02ld", endSec / 3600L, (endSec / 60L) % 60L);
        line[sizeof(line) - 1] = 0;
        x = text_bold_right(OV_RIGHT, 64.0f, 1.2f, 0xFFF6F6F6, line);
        gfx_icon(UI_ICON_HOURGLASS, x - 10.0f - 22.0f, 64.0f, 28.0f, 0xFFFFFFFF);
    }

    /* titulo y canal · vistas · fecha (sin el panel de datos encima) */
    if (!g_showStats)
    {
        gfx_font_bold(1);
        gfx_fit_text(g_title, OV_RIGHT - OV_X, OV_TITLE_S, line, sizeof(line));
        text_shadow(OV_X, OV_TITLE_Y, OV_TITLE_S, COL_TITLE, line);
        gfx_font_bold(0);
        line[0] = 0;
        if (g_infoChannel[0])
            strcat(line, g_infoChannel);
        if (g_infoViews[0])
        {
            if (line[0])
                strcat(line, " \xE2\x80\xA2 ");
            strcat(line, g_infoViews);
        }
        if (g_infoPublished[0])
        {
            if (line[0])
                strcat(line, " \xE2\x80\xA2 ");
            strcat(line, g_infoPublished);
        }
        gfx_fit_text(line, OV_RIGHT - OV_X, 1.0f, line, sizeof(line));
        text_shadow(OV_X, OV_META_Y, 1.0f, COL_TITLE, line);
    }

    focusTop = g_menu == MENU_NONE && g_row == ROW_TOP;
    focusBot = g_menu == MENU_NONE && g_row == ROW_BOTTOM;

    /* cerrar, en la esquina de arriba a la izquierda */
    draw_button(UI_ICON_CLOSE, OV_CLOSE_CX, OV_CLOSE_CY, g_menu == MENU_NONE && g_row == ROW_CLOSE, 1, "X");

    /* al centro, encima de la barra: -10 s, pausa/reproducir, +10 s */
    for (i = 0; i < BT_COUNT; ++i)
    {
        int icon = topIcons[i];
        if (i == BT_PLAY && g_paused)
            icon = UI_ICON_PLAY;
        draw_button_sz(icon, top_cx(i), OV_TOP_CY, i == BT_PLAY ? OV_PLAY_SZ : OV_ICON,
                       focusTop && g_col == i, 1,
                       i == BT_PLAY ? (g_paused ? "Ver" : "Pausa") : i == BT_REW ? "-10" : "+10");
    }

    /* derecha: 480/30/H.264 y la fecha */
    height = g_track.height > 0 ? g_track.height : g_fmt.height;
    _snprintf(line, sizeof(line) - 1, "%d/%d/H.264", height, g_fmt.fps > 0 ? g_fmt.fps : 30);
    line[sizeof(line) - 1] = 0;
    text_right_shadow(OV_RIGHT, OV_TOP_CY - 29.0f, OV_INFO_S, COL_INFO, line);
    if (lang_get() == LANG_JA)   /* japones: "11<mes>23<dia>(<lun>) 15:00" */
        _snprintf(line, sizeof(line) - 1, "%s%d\xE6\x97\xA5(%s) %02d:%02d", lang_month(t.wMonth - 1), t.wDay,
                  lang_day(t.wDayOfWeek), t.wHour, t.wMinute);
    else
        _snprintf(line, sizeof(line) - 1, "%s %d %s %02d:%02d", lang_day(t.wDayOfWeek), t.wDay,
                  lang_month(t.wMonth - 1), t.wHour, t.wMinute);
    line[sizeof(line) - 1] = 0;
    text_right_shadow(OV_RIGHT, OV_TOP_CY - 1.0f, OV_INFO_S, COL_INFO, line);

    /* barra: lo que falta (gris), lo bajado (rojo claro), lo visto (rojo)
     * y el punto */
    if (g_initDone == 1 && g_track.segmentCount > 0)
    {
        double ts = g_track.sidxTimescale ? (double)g_track.sidxTimescale : (double)g_track.timescale;
        for (i = 0; i < g_segLoaded && i < g_track.segmentCount && ts > 0.0; ++i)
            buf += g_track.segments[i].duration / ts;
    }
    kx = bx0 + (float)((bx1 - bx0) * frac);
    bar_part(bx0, bx1, COL_TRACK);
    if (len > 0.0 && buf > 0.0)
        bar_part(bx0, bx0 + (float)((bx1 - bx0) * (buf > len ? 1.0 : buf / len)), COL_LOADED);
    bar_part(bx0, kx, COL_PLAYED);
    if (g_menu == MENU_NONE && g_row == ROW_BAR)
    {
        gfx_icon(UI_ICON_ORB_S, kx - 16.0f, barCy - 16.0f, 32.0f, 0xFFFFFFFF);
        format_time(pos, a, sizeof(a));
        x = gfx_text_width(0.75f, a) + 14.0f;
        gfx_rect(kx - x * 0.5f, OV_BAR_Y - 26.0f, x, 21.0f, 0xD0101010);
        gfx_text(kx - x * 0.5f + 7.0f, OV_BAR_Y - 25.0f, 0.75f, COL_TEXT, a);
    }
    else if (!gfx_icon(UI_ICON_KNOB, kx - 13.0f, barCy - 13.0f, 26.0f, 0xFFFFFFFF))
        gfx_circle(kx - 8.0f, barCy - 8.0f, 16.0f, 0xFFFFFFFF);

    /* fila de abajo: calidad, canal, subtitulos, idioma, informacion */
    for (i = 0; i < BB_COUNT; ++i)
    {
        float cx = button_cx(i);
        int on = focusBot && g_col == i;

        if (i == BB_CHANNEL)
        {
            if (on)
                gfx_icon(UI_ICON_ORB, cx - OV_ICON * 0.5f, OV_BOT_CY - OV_ICON * 0.5f, OV_ICON, 0xFFFFFFFF);
            thumb_draw_avatar(g_infoAvatar, cx - 19.0f, OV_BOT_CY - 19.0f, 38.0f, g_infoChannel);
            continue;
        }
        if (i == BB_LIKE || i == BB_DISLIKE)
        {
            int mine = rating == (i == BB_LIKE ? RATE_LIKE : RATE_DISLIKE);
            int icon = i == BB_LIKE ? (mine ? UI_ICON_LIKE_ON : UI_ICON_LIKE)
                                    : (mine ? UI_ICON_DISLIKE_ON : UI_ICON_DISLIKE);
            draw_button(icon, cx, OV_BOT_CY, on, cuenta_signed_in() && g_infoId[0], i == BB_LIKE ? "+" : "-");
            continue;
        }
        if (i == BB_SUBSCRIBE)
        {
            int ss;
            cuenta_sub_request(g_infoVideo.channelId);
            ss = cuenta_sub_state(g_infoVideo.channelId);
            ui_draw_sub_pill(cx - UI_SUB_W * 0.5f, OV_BOT_CY - UI_SUB_H * 0.5f, ss == SUB_YES, on,
                             cuenta_signed_in() && ss != SUB_UNKNOWN);
            continue;
        }
        if (i == BB_STAR)
        {
            draw_button(starred ? UI_ICON_STAR_ON : UI_ICON_STAR, cx, OV_BOT_CY, on,
                        community_enabled() && g_infoId[0], "*");
            continue;
        }
        draw_button(botIcons[i], cx, OV_BOT_CY, on,
                    i == BB_SUBS ? g_capCount > 0 : i == BB_AUDIO ? g_trackCount > 1 : 1,
                    i == BB_QUALITY ? "HQ" : i == BB_SUBS ? "CC" : i == BB_AUDIO ? "Idioma" : "Info");
    }

    /* derecha: lo que queda y el tiempo */
    format_time(len - pos > 0.0 ? len - pos : 0.0, a, sizeof(a));
    _snprintf(line, sizeof(line) - 1, T(S_REMAINING), a);
    line[sizeof(line) - 1] = 0;
    text_right_shadow(OV_RIGHT, OV_BOT_CY - 29.0f, OV_INFO_S, COL_INFO, line);
    format_time(pos, a, sizeof(a));
    format_time(len, b, sizeof(b));
    if (g_seekDelta != 0.0)
        _snprintf(line, sizeof(line) - 1, "(%+d s)  %s / %s", (int)g_seekDelta, a, b);
    else
        _snprintf(line, sizeof(line) - 1, "%s / %s", a, b);
    line[sizeof(line) - 1] = 0;
    text_right_shadow(OV_RIGHT, OV_BOT_CY + 0.0f, OV_INFO_S, COL_INFO, line);

    /* nombre del boton elegido */
    if (g_menu == MENU_NONE && g_row == ROW_CLOSE)
        draw_tooltip_at(OV_CLOSE_CX + OV_ICON * 0.5f + 6.0f, OV_CLOSE_CY - 13.0f, T(S_CLOSE_VIDEO));
    else if (g_menu == MENU_NONE && g_row == ROW_BOTTOM)
    {
        switch (g_col)
        {
        case BB_QUALITY:
            _snprintf(line, sizeof(line) - 1, T(S_QUALITY_X), g_qualCount ? g_qualNames[g_qualCur] : "-");
            break;
        case BB_CHANNEL:
            _snprintf(line, sizeof(line) - 1, "%s%s%s", T(S_GO_CHANNEL), g_infoChannel[0] ? ": " : "", g_infoChannel);
            break;
        case BB_SUBS:
            _snprintf(line, sizeof(line) - 1, T(S_SUBS_X),
                      g_capCur >= 0 ? g_caps[g_capCur].name : (g_capCount ? T(S_OFF_LC) : T(S_NONE)));
            break;
        case BB_AUDIO:
            _snprintf(line, sizeof(line) - 1, T(S_AUDIO_LANG_X),
                      g_trackCount > 0 ? g_trackNames[g_trackCur] : T(S_SINGLE_TRACK));
            break;
        case BB_LIKE:
            _snprintf(line, sizeof(line) - 1, "%s", T(rating == RATE_LIKE ? S_LIKE_ON : S_LIKE));
            break;
        case BB_DISLIKE:
            _snprintf(line, sizeof(line) - 1, "%s", T(rating == RATE_DISLIKE ? S_DISLIKE_ON : S_DISLIKE));
            break;
        case BB_STAR:
            _snprintf(line, sizeof(line) - 1, "%s", T(starred ? S_STAR_ON : S_STAR));
            break;
        case BB_SUBSCRIBE:
            if (!cuenta_signed_in())
                _snprintf(line, sizeof(line) - 1, "%s", T(S_SUB_NEED));
            else
                _snprintf(line, sizeof(line) - 1, T(cuenta_sub_state(g_infoVideo.channelId) == SUB_YES ? S_UNSUB_TIP : S_SUB_TIP),
                          g_infoChannel);
            break;
        default:
            _snprintf(line, sizeof(line) - 1, "%s", T(S_INFO_TIP));
            break;
        }
        line[sizeof(line) - 1] = 0;
        draw_tooltip(button_cx(g_col), OV_BOT_CY + OV_ICON * 0.5f + 4.0f, line);
    }

    if (g_menu != MENU_NONE)
        draw_menu();
}

void play_draw(void)
{
    GvStats st;
    char line[256];
    const char *state;
    unsigned int stateCol = COL_SOFT;
    float x, y, w, h, boxH;
    double mbps, total;

    memset(&st, 0, sizeof(st));
    st.firstPts = st.curPts = -1.0;
    if (g_decReady == 1)
        gv_get_stats(&st);

    /* resultado de recomendar (estrella) */
    {
        int cr = community_take_result();
        if (cr)
            show_toast(T(cr > 0 ? S_STAR_DONE : S_STAR_FAIL));
    }
    /* aviso de la cuenta de Google (no se pudo calificar, sesion vencida) */
    {
        const char *m = cuenta_msg();
        if (m[0])
        {
            show_toast(m);
            cuenta_clear_msg();
        }
    }

    /* video, ajustado a la pantalla sin deformarlo */
    if (st.width > 0 && st.height > 0)
    {
        w = (float)SCREEN_W;
        h = w * st.height / st.width;
        if (h > SCREEN_H)
        {
            h = (float)SCREEN_H;
            w = h * st.width / st.height;
        }
        x = (SCREEN_W - w) * 0.5f;
        y = (SCREEN_H - h) * 0.5f;
        gfx_flush();
        gv_draw(x, y, w, h);
        gfx_reset_state();
    }

    /* estado */
    if (g_hasErr)
    {
        state = g_err;
        stateCol = COL_ERR;
    }
    else if (g_ended)
    {
        state = "Fin del video";
        stateCol = COL_OK;
    }
    else if (g_initDone == 0)
        state = "Bajando encabezado...";
    else if (g_decReady == 0)
        state = "Abriendo decodificador...";
    else if (st.firstPts < 0.0)
        state = "Esperando el primer cuadro...";
    else if (st.width == 0)
    {
        state = "Recuperando el video (reinicio del decodificador)...";
        stateCol = COL_WARN;
    }
    else if (g_paused)
    {
        state = "En pausa";
        stateCol = COL_WARN;
    }
    else if (!g_clockOn)
    {
        state = g_rebuffers ? "Cargando (se acabaron los cuadros listos)..." : "Juntando cuadros...";
        stateCol = COL_WARN;
    }
    else
    {
        state = g_useAudio ? "Reproduciendo" : "Reproduciendo (reloj sin audio)";
        stateCol = COL_OK;
    }

    if (st.width == 0 && !g_showStats)
    {
        /* aun no hay imagen: mensaje al centro */
        gfx_text((SCREEN_W - gfx_text_width(1.0f, state)) * 0.5f, 340.0f, 1.0f, stateCol, state);
    }

    /* subtitulos (arriba del titulo cuando la capa esta a la vista) */
    if (g_capCur >= 0 && st.width > 0)
        subs_draw(g_clock >= 0.0 ? g_clock : st.curPts, osd_visible() ? 360.0f : 676.0f);

    draw_overlay(&st);
    draw_toast();

    if (!g_showStats)
        return;

    boxH = 12 * 28.0f + 20.0f;
    gfx_rect(30.0f, 30.0f, SCREEN_W - 60.0f, boxH, COL_BOX);
    y = 40.0f;

    stats_line(&y, COL_TEXT, g_title);

    _snprintf(line, sizeof(line) - 1, "itag %d  \xC2\xB7  %dx%d  \xC2\xB7  H.264 %s %d.%d  \xC2\xB7  %d fragmentos",
              g_fmt.itag, g_track.width, g_track.height, fmp_profile_name(g_track.profile),
              g_track.level / 10, g_track.level % 10, g_track.segmentCount);
    line[sizeof(line) - 1] = 0;
    stats_line(&y, COL_SOFT, line);

    _snprintf(line, sizeof(line) - 1,
              "Entregados %ld  Decodificados %ld  Mostrados %ld  Tirados %ld",
              st.fed, st.decoded, st.shown, st.dropped);
    line[sizeof(line) - 1] = 0;
    stats_line(&y, COL_SOFT, line);

    _snprintf(line, sizeof(line) - 1,
              "En vuelo %ld  Listos %d  Prestados %d  GPU %ld  Recargas %ld  Espera candado %ld ms",
              st.inFlight, st.queued, st.held, st.backlog, g_rebuffers, st.lockMiss);
    line[sizeof(line) - 1] = 0;
    stats_line(&y, COL_SOFT, line);

    _snprintf(line, sizeof(line) - 1,
              "Entrada %d  Leidos %ld%s  Sin salida %ld ms  Reinicios %ld",
              st.inQueue, st.reads, st.readWaiting ? "  (esperando datos)" : "",
              st.sinceOutMs, st.kicks);
    line[sizeof(line) - 1] = 0;
    stats_line(&y, st.sinceOutMs > 1000 ? COL_WARN : COL_DIM, line);

    mbps = g_dlMs > 0 ? (double)g_bytes * 8.0 / ((double)g_dlMs * 1000.0) : 0.0;
    total = g_fmt.contentLength > 0 ? g_fmt.contentLength / (1024.0 * 1024.0) : 0.0;
    _snprintf(line, sizeof(line) - 1,
              "Tiempo %.1f s  \xC2\xB7  Bajado %.1f de %.1f MB a %.1f Mbit/s  \xC2\xB7  Fragmento %ld/%ld",
              st.curPts < 0.0 ? 0.0 : st.curPts, g_bytes / (1024.0 * 1024.0), total, mbps,
              (long)g_segFed, (long)g_segLoaded);
    line[sizeof(line) - 1] = 0;
    stats_line(&y, COL_SOFT, line);

    if (!g_hasAudio)
        stats_line(&y, COL_WARN, "Audio: el video no trae formato AAC");
    else if (g_audState < 0)
        stats_line(&y, COL_ERR, g_audErr[0] ? g_audErr : "Audio: fallo");
    else if (g_audState == 0)
        stats_line(&y, COL_SOFT, "Audio: cargando...");
    else
    {
        _snprintf(line, sizeof(line) - 1,
                  "Audio %s%s%s  \xC2\xB7  en cola %.1f s  \xC2\xB7  fragmento %ld/%d%s",
                  g_trackCount > 0 ? g_trackNames[g_trackCur] : "",
                  g_trackCount > 0 ? "  \xC2\xB7  " : "",
                  g_audInfo, au_buffered(), (long)g_audFrag, g_atrack.segmentCount,
                  g_useAudio ? "  \xC2\xB7  lleva el reloj" : "");
        line[sizeof(line) - 1] = 0;
        stats_line(&y, COL_SOFT, line);
    }

    stats_line(&y, gv_motor_lock() ? COL_DIM : COL_WARN, gv_status());
    stats_line(&y, stateCol, state);

    /* diagnostico de atascos */
    {
        char l1[256], l2[256], l3[256];

        gv_diag_panel(l1, l2, l3, sizeof(l1));
        stats_line(&y, st.kicks > 0 ? COL_WARN : COL_SOFT, l1);
        stats_line(&y, COL_SOFT, l2);
        stats_line(&y, COL_DIM, l3);
    }
}

/* Espera a que termine un hilo SIN congelar la consola: sigue mandando
 * cuadros (mensaje + ruedita) cada 50 ms, asi la guia de la Xbox sigue
 * respondiendo. Las peticiones de red se cortan solas con g_stop
 * (net_set_cancel), asi que normalmente es menos de un segundo. Devuelve
 * 1 si el hilo termino. */
static int wait_thread(HANDLE h, const char *name)
{
    DWORD t0 = GetTickCount(), r;
    char line[96];
    int logged = 0;

    for (;;)
    {
        r = WaitForSingleObject(h, 50);
        if (r != WAIT_TIMEOUT)
            break;
        /* (no se abandona nunca: si el hilo siguiera vivo, el siguiente
         * video compartiria sus variables y se atoraria todo) */
        if (!logged && GetTickCount() - t0 > 10000)
        {
            _snprintf(line, sizeof(line) - 1, "Detener: el hilo de %s lleva 10 s sin terminar\n", name);
            line[sizeof(line) - 1] = 0;
            gv_log(line);
            logged = 1;
        }
        gfx_begin(0xFF000000);
        if (g_waitMsg[0])
            gfx_text((SCREEN_W - gfx_text_width(1.0f, g_waitMsg)) * 0.5f, 380.0f, 1.0f, COL_SOFT, g_waitMsg);
        ui_draw_spinner(SCREEN_W * 0.5f, 320.0f, 24.0f);
        gfx_end();
    }
    if (GetTickCount() - t0 > 1500)
    {
        _snprintf(line, sizeof(line) - 1, "Detener: el hilo de %s tardo %lu ms\n", name,
                  (unsigned long)(GetTickCount() - t0));
        line[sizeof(line) - 1] = 0;
        gv_log(line);
    }
    return r == WAIT_OBJECT_0;
}

void play_set_wait_msg(const char *msg)
{
    strncpy(g_waitMsg, msg ? msg : "", sizeof(g_waitMsg) - 1);
    g_waitMsg[sizeof(g_waitMsg) - 1] = 0;
}

void play_stop(void)
{
    int i;
    GvStats st;
    char line[200], a[16], b[16];

    if (!g_active)
        return;

    /* diagnostico: hasta donde se vio */
    if (g_decReady == 1)
    {
        gv_get_stats(&st);
        format_time(g_lengthSec > 0.0 && current_pos(st.curPts) > g_lengthSec
                    ? g_lengthSec : current_pos(st.curPts), a, sizeof(a));
        format_time(g_lengthSec, b, sizeof(b));
        _snprintf(line, sizeof(line) - 1, "Detenido en %s de %s%s; recargas %ld\n", a, b,
                  g_ended ? " (llego al final)" : "", g_rebuffers);
        line[sizeof(line) - 1] = 0;
        gv_log(line);
    }
    gv_set_diag_hook(NULL);

    g_stop = 1;

    /* 2026-11-23 (beta 1): el hilo principal vuelve a su prioridad y nucleo
     * normales ANTES de esperar. Durante el video corre en el nucleo 5 con
     * prioridad maxima; si se quedaba asi mientras esperaba (dibujando la
     * ruedita) no dejaba correr al sistema en ese nucleo y la guia de la
     * Xbox se colgaba. */
    if (g_movedThread)
    {
        SetThreadPriority(GetCurrentThread(), g_oldPrio);
        XSetThreadProcessor(GetCurrentThread(), g_oldCore);
        g_movedThread = 0;
    }

    /* 1. el alimentador primero (puede estar dentro de gv_feed) */
    if (g_feedThread)
    {
        wait_thread(g_feedThread, "alimentador");
        CloseHandle(g_feedThread);
        g_feedThread = NULL;
    }

    /* 2. el audio (la cola de XAudio2 se vacia al cerrar la voz) */
    if (g_audThread)
    {
        if (wait_thread(g_audThread, "audio"))
            fmp_free(&g_atrack);
        CloseHandle(g_audThread);
        g_audThread = NULL;
    }
    au_close();

    /* 3. cierra el decodificador de video */
    gv_close();

    /* 4. la descarga (si esta a media peticion, termina sola) */
    if (g_dlThread)
    {
        if (wait_thread(g_dlThread, "descarga"))
        {
            EnterCriticalSection(&g_fragLock);
            for (i = 0; i < g_fragCount; ++i)
                http_free(&g_frags[(g_fragHead + i) % FRAG_SLOTS].r);
            g_fragCount = 0;
            LeaveCriticalSection(&g_fragLock);
            fmp_free(&g_track);
        }
        CloseHandle(g_dlThread);
        g_dlThread = NULL;
    }

    g_waitMsg[0] = 0;
    g_active = 0;
}
