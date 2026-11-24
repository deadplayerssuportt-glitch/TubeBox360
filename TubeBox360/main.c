/* main.c - TubeBox360, fase 1: miniaturas (5a), pista de audio y
 * subtitulos (5d) e Inicio con recomendados.
 *
 * Pantallas:
 *   Lista  - al arrancar, el Inicio: videos recomendados ("Porque
 *            viste..." con el historial local, y categorias de YouTube).
 *            Despues, resultados de busqueda. Cada fila con miniatura
 *            (thumb.c), duracion encima y avatar del canal. Hasta 30 videos.
 *            Y: buscar, X: Inicio, A: abrir, Der: pagina del canal
 *   Canal  - banner, avatar, nombre, suscriptores, descripcion y sus
 *            videos mas recientes. A: abrir video, B: volver
 *   Video  - pide a YouTube los formatos del video con el cliente VISIONOS
 *            y los muestra. A hace una descarga de prueba del formato
 *            elegido; X prueba la lista HLS; Y analiza el video H.264
 *            elegido y su audio AAC (encabezado, indice y primer fragmento).
 *            Arriba de la tabla, dos filas: Audio (original, espanol,
 *            ingles, portugues; empieza en espanol si existe) y Subtitulos
 *            (apagados, espanol, ingles, portugues). Se llega con la
 *            cruceta hacia arriba y se cambian con Izq/Der.
 *            Start reproduce el formato H.264 elegido (play.c). Back
 *            cambia el modo de prueba del diagnostico de atascos (4f).
 *   Reproduccion - el video decodificado por la GPU de la consola
 *            (gpuvid.cpp), con audio AAC (ffaac + audio.cpp). B detiene.
 *
 * Todo lo que usa la red corre en un hilo de trabajo (una tarea a la vez)
 * para que la pantalla no se congele.
 *
 * Compila como C (VS2010 + XDK), estilo C89: variables al inicio de cada
 * bloque. Los textos con acentos van como bytes UTF-8 con \x, partidos en
 * trozos para que la letra siguiente no se lea como parte del hexadecimal. */

#include <xtl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "gfx.h"
#include "net.h"
#include "report.h"
#include "version.h"
#include "presence.h"
#include "cuenta.h"
#include "update.h"
#include "community.h"
#include "yt.h"
#include "text.h"
#include "media.h"
#include "gpuvid.h"
#include "play.h"
#include "audio.h"
#include "thumb.h"
#include "ui_assets.h"
#include "hist.h"
#include "subs.h"
#include "ui.h"
#include "search.h"
#include "lang.h"
#include "sfx.h"
#include "avatar.h"
#include "avatar3d.h"

#ifndef VKBD_DEFAULT
#define VKBD_DEFAULT 0
#endif

/* Colores y tema: ui.h */
#define CONFIG_PATH "game:\\config.ini"

/* Diseño */
#define SAFE_X        80.0f
#define LIST_Y        116.0f
#define ROW_H         102.0f
#define VISIBLE_ROWS  5
#define LINE_CHARS    88      /* caracteres que caben a lo ancho */
#define LIST_THUMB_W  160.0f  /* miniatura 16:9 en cada fila */
#define LIST_THUMB_H  90.0f
#define LIST_X        UI_CONTENT_X   /* listas a la derecha del menu lateral */
/* avatar junto al menu desplegado: centro, piso y alto */
#define AVATAR_CX     (UI_GUIDE_W + 310.0f)
#define AVATAR_FLOOR  492.0f
#define AVATAR_HGT      340.0f
#define ROW_TEXT_X    (LIST_X + 36.0f + LIST_THUMB_W)
#define ROW_CHARS     70      /* caracteres que caben a la derecha de la miniatura */
#define AVATAR_SIZE   28.0f   /* avatar del canal en cada fila */
#define PREFETCH_ROWS 4       /* filas fuera de la vista cuyas miniaturas se piden antes */
#define AUDIO_Y       216.0f  /* fila de la pista de audio */
#define SUBS_Y        246.0f  /* fila de los subtitulos */
#define FMT_HEAD_Y    282.0f
#define FMT_Y         312.0f
#define FMT_ROW_H     30.0f
#define FMT_ROWS      9

#define REPEAT_DELAY  400
#define REPEAT_RATE   90
#define STICK_LIMIT   16000

#define BTN_UP    XINPUT_GAMEPAD_DPAD_UP
#define BTN_DOWN  XINPUT_GAMEPAD_DPAD_DOWN

#define MAX_VIDEOS    40      /* busqueda y secciones: 8 filas de 5 (el canal usa YT_LIST_MAX) */
#define HOME_MAX      30      /* Inicio: 6 filas de 5 */
#define MAX_SECTIONS  8       /* secciones del Inicio */

#define DOT "  \xC2\xB7  "     /* separador " · " */

/* Corta el texto al ancho que ocupaban maxChars letras monoespaciadas a esa
 * escala (las medidas de la pantalla se pensaron asi). La letra de la
 * interfaz es proporcional: se mide en pixeles. */
static void fitw(const char *src, int maxChars, float scale, char *dst, int size)
{
    gfx_fit_text(src, maxChars * (float)GFX_CHAR_W * scale, scale, dst, size);
}

/* Columnas de la tabla de formatos (la fuente es monoespaciada) */
#define FMT_COLUMNS "%4d %-5s  %-13s  %-17s %6d  %7s"
#define FMT_HEADER  "%4s %-5s  %-13s  %-17s %6s  %7s"

enum { SCREEN_LIST, SCREEN_VIDEO, SCREEN_PLAY, SCREEN_CHANNEL, SCREEN_SEARCH, SCREEN_SETTINGS, SCREEN_MANUAL,
       SCREEN_GOOGLE };
enum { JOB_NONE, JOB_SEARCH, JOB_PLAYER, JOB_PROBE, JOB_HLS, JOB_ANALYZE, JOB_HOME, JOB_CHANNEL, JOB_RELATED,
       JOB_CATEGORY, JOB_NETTEST, JOB_SUBS, JOB_COMMUNITY };
enum { FOCUS_AUDIO, FOCUS_SUBS, FOCUS_FORMATS };   /* fila elegida en la pantalla del video */

/* ------------------------------------------------------------------ */
/* Estado                                                              */

static int g_screen = SCREEN_LIST;

/* Lista de resultados */
static YtVideo g_videos[MAX_VIDEOS];
static int     g_count = 0;
static char    g_query[256] = "";
static char    g_status[256] = "";
static int     g_isHome = 0;                    /* la lista es el Inicio */
static int     g_videoSec[MAX_VIDEOS];          /* seccion de cada video (Inicio) */
static char    g_secNames[MAX_SECTIONS][96];
static int     g_secCount = 0;
static int     g_listSel = 0;                   /* fila elegida (para el encabezado) */

/* Pagina de canal: la llena el hilo; leer solo si no esta ocupado */
static YtChannel g_channel;
static int       g_channelOk = 0;
static char      g_channelStatus[256] = "";
static int       g_chSel = 0;
static int       g_chTop = 0;
static char      g_chAvatarHint[320];   /* el avatar que ya se veia en la lista */

/* Pantalla de video */
static int      g_videoIdx = -1;      /* >= 0 si hay video abierto */
static YtVideo  g_curVideo;           /* copia del video abierto (de la lista o del canal) */
static int      g_videoFrom = SCREEN_LIST;   /* a donde vuelve con B */
static int      g_autoPlay = 0;       /* 1 = con A: empieza solo en 480p (sin la pantalla de formatos) */
static int      g_autoStart = 0;      /* ya llegaron los formatos: arrancar el video */
static YtPlayer g_player;              /* lo llena el hilo; leer solo si no esta ocupado */
static int      g_playerOk = 0;
static char     g_videoStatus[256] = "";
static char     g_probeText[512] = "";
static int      g_probeOk = 0;
static int      g_fmtSel = 0;    /* fila elegida en la lista de compatibles */
static int      g_fmtTop = 0;
static int      g_fmtList[48];   /* indices (en g_player.formats) de los compatibles */
static int      g_fmtCount = 0;
static int      g_trackSel = -1; /* pista de audio elegida (en g_player.tracks), -1 = unica */
static int      g_playTracks[YT_MAX_TRACKS];  /* pistas que se pasaron a play.c, en orden */
static int      g_capSel = -1;   /* subtitulos elegidos (en g_player.captions), -1 = apagados */
static int      g_capTouched = 0;/* el usuario ya los cambio (no seguir al audio) */
static int      g_focus = FOCUS_FORMATS;

/* Analisis (4b): lo llena el hilo; leer solo cuando termino */
static MediaProbe g_probe;
static int        g_analysisShown = 0;
static int        g_analysisOk = 0;
static char       g_analysisErr[256] = "";
static DWORD      g_analysisMs = 0;

/* Hilo de trabajo: el hilo escribe solo en g_job*, g_tmp y g_player.
 * El hilo principal lee esos datos cuando g_jobDone vale 1. */
static volatile LONG g_jobBusy = 0;
static volatile LONG g_jobDone = 0;
static int           g_jobType = JOB_NONE;
static char          g_jobArg[YT_URL_SIZE];
static int           g_jobInt;
static DWORD         g_jobMs;
static int           g_jobResult;
static char          g_jobErr[256];
static char          g_jobText[512];
static YtVideo       g_tmp[MAX_VIDEOS];
static int           g_tmpSec[MAX_VIDEOS];
static char          g_tmpSecNames[MAX_SECTIONS][96];
static int           g_tmpSecCount;
static YtVideo       g_scratch[YT_LIST_MAX];
static YtVideo       g_subChans[MAX_VIDEOS];   /* canales suscritos (los llena el hilo) */
static YtVideo       g_homeHist[2];      /* los 2 ultimos vistos (los copia el principal) */
static int           g_homeHistCount;

/* Selector de tema (arriba, junto al logo): se llega con la cruceta arriba
 * desde la primera fila de la lista */
enum { HDR_NONE, HDR_SEARCH, HDR_UPDATE };   /* arriba: la barra de busqueda / el icono de descarga */
static int g_headerFocus = HDR_NONE;

/* Pantalla de carga con el logo al arrancar, hasta que llega el Inicio */
#define SPLASH_MIN_MS 1500
static int   g_splash = 1;
static DWORD g_splashStart = 0;

/* Al terminar un video (abierto con A): cuenta regresiva de 5 s para el
 * siguiente. Sale de la lista de donde vino (Inicio, busqueda o canal); si
 * era el ultimo, el primer relacionado que YouTube sugiere. */
#define UPNEXT_MS 5000
enum { END_NONE, END_SEARCH, END_COUNT };
static int      g_endState = END_NONE;
static DWORD    g_upNextStart = 0;
static YtVideo  g_nextVideo;
static int      g_nextIdx = -1;      /* en la lista de donde vino (-1 = relacionado) */
static int      g_upSel = 0;         /* 0 = Reproducir ahora, 1 = Cancelar */
static int      g_curIdx = -1;       /* el video abierto en su lista (-1 = no esta) */
static int      g_listJump = -1;     /* al volver, la lista se para en esta fila */
static YtVideo  g_recent[10];        /* para no repetir los ultimos vistos */

/* Cuadro "Salir de TubeBox360?" (Back en la lista) */
static int g_exitDlg = 0;        /* 1 = a la vista */
static int g_updDlg = 0;         /* cuadro de actualizaciones a la vista */
static int g_updSel = 0;         /* boton elegido en ese cuadro */
static int g_exitSel = -1;       /* -1 ninguno (los dos grises), 0 = Si, 1 = No */
static int g_stickX = 0;         /* palanca izquierda: -1, 0, 1 */

/* Atenuar la pantalla por inactividad. El protector de pantalla de la
 * consola (el del dashboard) oscurece a los pocos minutos aunque se este
 * viendo un video; se apaga y la app usa el suyo: 30 min sin tocar el
 * mando. Cualquier boton la despierta (ese boton no hace nada mas). */
#define IDLE_DIM_MS   (30UL * 60UL * 1000UL)
#define IDLE_FADE_MS  2000UL
static DWORD g_lastInput = 0;    /* GetTickCount del ultimo uso del mando */
static int   g_dimmed = 0;
static DWORD g_dimStart = 0;
static int   g_wakeLock = 0;     /* se desperto: ignorar el mando hasta soltarlo */
static float g_rx = 0.0f, g_ry = 0.0f;   /* palanca derecha, -1..1 (para el avatar) */
static float g_lt = 0.0f, g_rt = 0.0f;   /* gatillos, 0..1 */
static WORD  g_padRaw = 0;               /* botones tal cual (sin la palanca sumada a la cruceta) */

/* Menu lateral: la seccion que se ve y, desplegado, la elegida */
static int g_section = UI_SEC_HOME;
static int g_menuFocus = 0;
static int g_menuSel = UI_SEC_HOME;
static int g_setSel = 0;            /* fila elegida en Configuracion */

/* Copia del Inicio, para volver a el sin pedirlo otra vez */
static YtVideo g_homeVideos[MAX_VIDEOS];
static int     g_homeSec[MAX_VIDEOS];
static char    g_homeSecNames[MAX_SECTIONS][96];
static int     g_homeSecCount = 0;
static int     g_homeCount = 0;

/* Canales de las categorias (los mismos del Inicio) */
static void grid_reset(void);   /* cuadricula, mas abajo */
static void channel_view_reset(void);   /* cuadricula del canal, mas abajo */
static char g_netReport[1200];  /* reporte de la prueba de red sin la PC */
static int  g_reportSaved = 0;  /* 1 = se guardo game:\TubeBox360_reporte.txt, -1 = fallo */

static const char *category_id(int sec)
{
    switch (sec)
    {
    case UI_SEC_GAMING: return "UCOpNcN46UbXVtpKMrmU4Abg";
    case UI_SEC_MUSIC:  return "UC-9-kyTW8ZkZNDHQJ6FgpwQ";
    case UI_SEC_NEWS:   return "UCYfdidRxbB8Qhf0Nx7ioOYw";
    case UI_SEC_SPORTS: return "UCEgdi0XIXXZ-qJOFPf4JSKw";
    }
    return NULL;
}

/* Busqueda de respaldo si la pagina de la categoria viene vacia */
static int category_query(int sec)
{
    switch (sec)
    {
    case UI_SEC_GAMING: return S_Q_GAMING;
    case UI_SEC_MUSIC:  return S_Q_MUSIC;
    case UI_SEC_NEWS:   return S_Q_NEWS;
    }
    return S_Q_SPORTS;
}

/* ------------------------------------------------------------------ */
/* Hilo de trabajo                                                     */

/* ------------------------------------------------------------------ */
/* Inicio (lo arma el hilo de trabajo)                                 */

/* Agrega a g_tmp los videos de g_scratch que no esten ya, como una seccion
 * nueva. Devuelve cuantos agrego. */
static int add_section(int *count, int n, int limit, const char *name)
{
    int i, k, added = 0, dup;

    if (n <= 0 || g_tmpSecCount >= MAX_SECTIONS)
        return 0;
    for (i = 0; i < n && added < limit && *count < HOME_MAX; ++i)
    {
        dup = 0;
        for (k = 0; k < *count && !dup; ++k)
            dup = strcmp(g_tmp[k].id, g_scratch[i].id) == 0;
        if (dup)
            continue;
        g_tmp[*count] = g_scratch[i];
        g_tmpSec[*count] = g_tmpSecCount;
        ++*count;
        ++added;
    }
    if (added > 0)
    {
        strncpy(g_tmpSecNames[g_tmpSecCount], name, sizeof(g_tmpSecNames[0]) - 1);
        g_tmpSecNames[g_tmpSecCount][sizeof(g_tmpSecNames[0]) - 1] = 0;
        ++g_tmpSecCount;
    }
    return added;
}

/* Sin sesion YouTube no da su Inicio, asi que se arma con:
 *   1. "Porque viste...": relacionados de los 2 ultimos videos vistos
 *      (10 del ultimo y 4 del penultimo).
 *   2. Las paginas de categorias de YouTube (Musica, Gaming, Noticias,
 *      Deportes). Si alguna ya no existe, simplemente no aporta videos.
 *   3. Si con eso salen muy pocos: una busqueda de "tendencias".
 * Devuelve cuantos videos junto, o -1 si todo fallo (motivo en err). */
static int build_home(char *err, int errSize)
{
    static const char *catIds[4] = {
        "UC-9-kyTW8ZkZNDHQJ6FgpwQ",   /* Musica */
        "UCOpNcN46UbXVtpKMrmU4Abg",   /* Gaming */
        "UCYfdidRxbB8Qhf0Nx7ioOYw",   /* Noticias */
        "UCEgdi0XIXXZ-qJOFPf4JSKw"    /* Deportes */
    };
    static const int catNames[4] = { S_SEC_MUSIC, S_SEC_GAMING, S_SEC_NEWS, S_SEC_SPORTS };
    char name[96];
    char title[64];
    char lastErr[160];
    int count = 0, n, i, failed = 0, tries = 0;

    g_tmpSecCount = 0;
    lastErr[0] = 0;

    for (i = 0; i < g_homeHistCount; ++i)
    {
        ++tries;
        n = yt_related(g_homeHist[i].id, g_scratch, i == 0 ? 16 : 10, lastErr, sizeof(lastErr));
        if (n < 0)
        {
            ++failed;
            continue;
        }
        fitw(g_homeHist[i].title, 40, 1.0f, title, sizeof(title));
        _snprintf(name, sizeof(name) - 1, T(S_BECAUSE), title);
        name[sizeof(name) - 1] = 0;
        add_section(&count, n, i == 0 ? 10 : 4, name);
    }

    /* lo que queda hasta 30 se reparte entre las categorias */
    for (i = 0; i < 4; ++i)
    {
        if (count >= HOME_MAX)
            break;   /* ya hay 30 */
        ++tries;
        n = yt_browse(catIds[i], g_scratch, 16, lastErr, sizeof(lastErr));
        if (n < 0)
            ++failed;
        else
            add_section(&count, n, (HOME_MAX - count + (4 - i) - 1) / (4 - i), T(catNames[i]));
    }

    if (count < 10)
    {
        ++tries;
        n = yt_search(T(S_TREND_QUERY), g_scratch, HOME_MAX, lastErr, sizeof(lastErr));
        if (n < 0)
            ++failed;
        else
            add_section(&count, n, HOME_MAX, T(S_TRENDING));
    }

    if (count == 0 && failed == tries)
    {
        _snprintf(err, errSize - 1, "%s", lastErr);
        err[errSize - 1] = 0;
        return -1;
    }
    return count;
}

/* Suscripciones (cuenta de Google): primero los videos nuevos (filas
 * completas de 5) y despues los canales. -1 = fallo (motivo en err). */
static int build_subs(char *err, int errSize)
{
    int nv = 0, nc = 0, i, count = 0;

    g_tmpSecCount = 0;
    if (!cuenta_subscriptions(g_scratch, 20, &nv, g_subChans, MAX_VIDEOS, &nc, err, errSize))
        return -1;
    if (nv > 5)
        nv -= nv % 5;   /* filas de 5: los canales empiezan en su propia fila */
    for (i = 0; i < nv; ++i, ++count)
    {
        g_tmp[count] = g_scratch[i];
        g_tmpSec[count] = 0;
    }
    if (nv > 0)
    {
        strcpy(g_tmpSecNames[0], T(S_SUBS_NEW));
        g_tmpSecCount = 1;
    }
    for (i = 0; i < nc && count < MAX_VIDEOS; ++i, ++count)
    {
        g_tmp[count] = g_subChans[i];
        g_tmpSec[count] = g_tmpSecCount;
    }
    if (nc > 0)
    {
        strcpy(g_tmpSecNames[g_tmpSecCount], T(S_SUBS_CHANNELS));
        ++g_tmpSecCount;
    }
    return count;
}

static DWORD WINAPI job_thread(LPVOID param)
{
    DWORD t0 = GetTickCount();

    (void)param;

    switch (g_jobType)
    {
    case JOB_SEARCH:
        g_jobResult = yt_search(g_jobArg, g_tmp, MAX_VIDEOS, g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_CHANNEL:
        g_jobResult = yt_channel(g_jobArg, &g_channel, g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_PLAYER:
        g_jobResult = yt_player(g_jobArg, &g_player, g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_PROBE:
        g_jobResult = yt_probe_range(g_jobArg, g_jobText, sizeof(g_jobText));
        break;
    case JOB_HLS:
        g_jobResult = yt_probe_hls(g_jobArg, g_jobText, sizeof(g_jobText));
        break;
    case JOB_ANALYZE:
        g_jobResult = media_probe(&g_player, g_jobInt, &g_probe, g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_HOME:
        g_jobResult = build_home(g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_SUBS:
        g_jobResult = build_subs(g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_COMMUNITY:
        g_jobResult = community_fetch(g_tmp, COMMUNITY_MAX, g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_RELATED:
        g_jobResult = yt_related(g_jobArg, g_scratch, 12, g_jobErr, sizeof(g_jobErr));
        break;
    case JOB_NETTEST:
        g_jobResult = net_selftest(g_netReport, sizeof(g_netReport));
        g_reportSaved = report_write(g_netReport) ? 1 : -1;   /* reporte para mandar */
        break;
    case JOB_CATEGORY:
        if (strcmp(g_jobArg, "shorts") == 0)
        {
            /* Shorts: por ahora una busqueda de Shorts (el feed propio de
             * Shorts y su reproductor vertical van en la siguiente etapa) */
            g_jobResult = yt_search(T(S_Q_SHORTS), g_tmp, MAX_VIDEOS, g_jobErr, sizeof(g_jobErr));
            break;
        }
        g_jobResult = yt_browse(g_jobArg, g_tmp, MAX_VIDEOS, g_jobErr, sizeof(g_jobErr));
        if (g_jobResult < 6)
        {
            /* la pagina de la categoria vino casi vacia (pasa con Gaming):
             * se completa con una busqueda del tema */
            int have = g_jobResult > 0 ? g_jobResult : 0;
            int n = yt_search(T(category_query(g_jobInt)), g_tmp + have, MAX_VIDEOS - have,
                              g_jobErr, sizeof(g_jobErr));
            if (n > 0)
                g_jobResult = have + n;
        }
        break;
    }
    g_jobMs = GetTickCount() - t0;

    /* La CPU de la Xbox puede reordenar escrituras: esta barrera asegura
     * que los resultados esten escritos antes de avisar que terminamos. */
    __lwsync();
    g_jobDone = 1;
    return 0;
}

static int start_job(int type, const char *arg)
{
    HANDLE h;

    if (g_jobBusy)
        return 0;

    strncpy(g_jobArg, arg, sizeof(g_jobArg) - 1);
    g_jobArg[sizeof(g_jobArg) - 1] = 0;
    g_jobType = type;
    g_jobErr[0] = 0;
    g_jobText[0] = 0;
    g_jobDone = 0;
    g_jobBusy = 1;

    /* 256 KB de pila: leer el JSON usa recursion */
    h = CreateThread(NULL, 256 * 1024, job_thread, NULL, 0, NULL);
    if (!h)
    {
        g_jobBusy = 0;
        strcpy(g_status, "No se pudo crear el hilo de trabajo");
        return 0;
    }
    CloseHandle(h);
    return 1;
}

static int is_busy(int type)
{
    return g_jobBusy && g_jobType == type;
}

/* Nivel H.264 de "avc1.PPCCLL" (LL en hexadecimal: 0x28 = 40 = 4.0).
 * 0 si no se puede leer. */
static int avc_level(const char *codecs)
{
    const char *p;
    int i, v = 0, c;

    if (strncmp(codecs, "avc1.", 5) != 0 || strlen(codecs) < 11)
        return 0;
    p = codecs + 9;
    for (i = 0; i < 2; ++i)
    {
        c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    return v;
}

/* 1 si el decodificador de la consola lo reproduce bien: H.264 hasta
 * nivel 4.1 (1080p a 30 o 720p a 60). 1080p60 es nivel 4.2: da tirones. */
static int format_playable(const YtFormat *f)
{
    int lv;

    if (!f->isVideo || strncmp(f->codecs, "avc1", 4) != 0 || f->indexEnd <= f->initStart)
        return 0;
    lv = avc_level(f->codecs);
    if (lv > 41)
        return 0;
    if (f->height > 720 && f->fps > 30)
        return 0;
    return 1;
}

/* Lista de formatos que se muestran: solo video H.264 que la consola
 * reproduce bien (ver format_playable), de mejor a peor. El primero queda
 * elegido. El audio (140) se elige solo al reproducir. */
static void build_format_list(void)
{
    int i, j, t;
    const YtFormat *a, *b;

    g_fmtCount = 0;
    for (i = 0; i < g_player.formatCount && g_fmtCount < 48; ++i)
        if (format_playable(&g_player.formats[i]))
            g_fmtList[g_fmtCount++] = i;

    /* orden: mas alto primero; a igual altura, mas cuadros por segundo */
    for (i = 1; i < g_fmtCount; ++i)
    {
        t = g_fmtList[i];
        a = &g_player.formats[t];
        j = i;
        while (j > 0)
        {
            b = &g_player.formats[g_fmtList[j - 1]];
            if (b->height > a->height || (b->height == a->height && b->fps >= a->fps))
                break;
            g_fmtList[j] = g_fmtList[j - 1];
            --j;
        }
        g_fmtList[j] = t;
    }
    g_fmtSel = 0;
    g_fmtTop = 0;
}

/* Formato de la fila elegida, o NULL si no hay compatibles */
static const YtFormat *selected_format(void)
{
    if (g_fmtSel < 0 || g_fmtSel >= g_fmtCount)
        return NULL;
    return &g_player.formats[g_fmtList[g_fmtSel]];
}

/* Calidad predeterminada (Configuracion): alto en pixeles */
static const int kQualities[] = { 360, 480, 720, 1080 };
#define QUALITY_COUNT 4
static int g_prefQuality = 1;     /* indice en kQualities: 480p */
static int g_avatarOn = 1;        /* avatar junto al menu lateral */
static int g_tlsVerifyCfg = 1;    /* config.ini certificados= (1 = verificar HTTPS) */

/* Calidad con la que empieza cada video: la predeterminada (480p si no se
 * cambio) o la mas alta por debajo (de 30 cuadros si hay de dos tipos).
 * Posicion en g_fmtList. */
static int default_quality(void)
{
    int i, first = -1;
    const YtFormat *f;

    for (i = 0; i < g_fmtCount; ++i)
    {
        f = &g_player.formats[g_fmtList[i]];
        if (f->height > kQualities[g_prefQuality])
            continue;
        if (first < 0)
            first = i;
        if (f->fps <= 30 && g_player.formats[g_fmtList[first]].height == f->height)
            return i;
        if (f->height < g_player.formats[g_fmtList[first]].height)
            break;
    }
    if (first >= 0)
        return first;
    return g_fmtCount > 0 ? g_fmtCount - 1 : 0;   /* todas son mayores: la mas baja */
}

/* "1080p", "720p60"... */
static void quality_name(const YtFormat *f, char *out, int size)
{
    if (f->fps > 30)
        _snprintf(out, size - 1, "%dp%d", f->height, f->fps);
    else
        _snprintf(out, size - 1, "%dp", f->height);
    out[size - 1] = 0;
}

/* Suscripciones guardadas (se vuelven a pedir si tienen mas de 10 min) */
static YtVideo g_subsVideos[MAX_VIDEOS];
static int     g_subsSec[MAX_VIDEOS];
static char    g_subsSecNames[MAX_SECTIONS][96];
static int     g_subsSecCount = 0, g_subsCount = 0, g_subsVer = -1;
static DWORD   g_subsTick = 0;
#define SUBS_KEEP_MS (10UL * 60UL * 1000UL)

/* Recomendados por la comunidad (se vuelven a pedir si tienen mas de 2 min) */
static YtVideo g_commVideos[COMMUNITY_MAX];
static int     g_commCount = 0, g_commLoaded = 0, g_commVer = -1;
static DWORD   g_commTick = 0;
#define COMM_KEEP_MS (2UL * 60UL * 1000UL)

static void apply_community(void)
{
    int i;
    memcpy(g_videos, g_commVideos, sizeof(YtVideo) * g_commCount);
    for (i = 0; i < g_commCount; ++i)
        g_videoSec[i] = -1;
    g_secCount = 0;
    g_count = g_commCount;
    g_query[0] = 0;
    g_isHome = 0;
    g_status[0] = 0;
    g_listJump = 0;
    grid_reset();
}

static void apply_subs(void)
{
    memcpy(g_videos, g_subsVideos, sizeof(YtVideo) * g_subsCount);
    memcpy(g_videoSec, g_subsSec, sizeof(int) * g_subsCount);
    memcpy(g_secNames, g_subsSecNames, sizeof(g_secNames));
    g_secCount = g_subsSecCount;
    g_count = g_subsCount;
    g_query[0] = 0;
    g_isHome = 0;
    g_status[0] = 0;
    g_listJump = 0;
    grid_reset();
}

/* Pone en la lista el Inicio guardado */
static void apply_home(void)
{
    memcpy(g_videos, g_homeVideos, sizeof(YtVideo) * g_homeCount);
    memcpy(g_videoSec, g_homeSec, sizeof(int) * g_homeCount);
    memcpy(g_secNames, g_homeSecNames, sizeof(g_secNames));
    g_secCount = g_homeSecCount;
    g_count = g_homeCount;
    g_query[0] = 0;
    g_isHome = 1;
    g_status[0] = 0;
    g_listJump = 0;
    grid_reset();   /* lista nueva: cada fila desde su primera tarjeta */
}

/* Revisa si termino la tarea y aplica el resultado. Devuelve el tipo de
 * tarea que termino, o JOB_NONE. */
static int poll_job(void)
{
    int type;

    if (!g_jobBusy || !g_jobDone)
        return JOB_NONE;

    __lwsync();
    type = g_jobType;
    g_jobBusy = 0;

    /* cada tarea que fallo queda en game:\errores.txt (para el reporte) */
    if (type != JOB_NETTEST && !g_jobResult && g_jobErr[0])
    {
        static const char *names[] = { "-", "busqueda", "video", "prueba", "hls", "analisis", "inicio", "canal",
                                       "relacionados", "seccion", "red", "suscripciones",
                                       "recomendados" };
        report_error(type >= 0 && type < (int)(sizeof(names) / sizeof(names[0])) ? names[type] : "tarea",
                     "%s (%.80s)", g_jobErr, g_jobArg);
    }

    switch (type)
    {
    case JOB_CHANNEL:
        g_channelOk = g_jobResult;
        g_chSel = g_chTop = 0;
        channel_view_reset();
        /* si la pagina no trajo avatar, el de la lista (mas grande) */
        if (g_channelOk && !g_channel.avatar[0] && g_chAvatarHint[0])
        {
            char *sz = strstr(g_chAvatarHint, "=s68");
            strcpy(g_channel.avatar, g_chAvatarHint);
            if (sz)
            {
                g_channel.avatar[sz - g_chAvatarHint + 2] = 0;
                strcat(g_channel.avatar, "176");
                strcat(g_channel.avatar, sz + 4);
            }
        }
        if (!g_channelOk)
        {
            _snprintf(g_channelStatus, sizeof(g_channelStatus) - 1, T(S_ERROR), g_jobErr);
            g_channelStatus[sizeof(g_channelStatus) - 1] = 0;
        }
        break;

    case JOB_HOME:
        if (g_jobResult > 0)
        {
            /* copia para volver al Inicio desde el menu sin pedirlo otra vez */
            memcpy(g_homeVideos, g_tmp, sizeof(YtVideo) * g_jobResult);
            memcpy(g_homeSec, g_tmpSec, sizeof(int) * g_jobResult);
            memcpy(g_homeSecNames, g_tmpSecNames, sizeof(g_homeSecNames));
            g_homeSecCount = g_tmpSecCount;
            g_homeCount = g_jobResult;
            if (g_section == UI_SEC_HOME)
                apply_home();   /* si mientras tanto se fue a otra seccion, no la pisa */
        }
        else if (g_section != UI_SEC_HOME)
            ;   /* el error del Inicio no importa en otra seccion */
        else if (g_jobResult == 0)
            strcpy(g_status, T(S_NO_HOME));
        else
            _snprintf(g_status, sizeof(g_status) - 1, T(S_ERROR), g_jobErr);
        g_status[sizeof(g_status) - 1] = 0;
        break;

    case JOB_SUBS:
        if (g_jobResult > 0)
        {
            memcpy(g_subsVideos, g_tmp, sizeof(YtVideo) * g_jobResult);
            memcpy(g_subsSec, g_tmpSec, sizeof(int) * g_jobResult);
            memcpy(g_subsSecNames, g_tmpSecNames, sizeof(g_subsSecNames));
            g_subsSecCount = g_tmpSecCount;
            g_subsCount = g_jobResult;
            g_subsTick = GetTickCount();
            g_subsVer = cuenta_sub_version();
            if (g_section == UI_SEC_SUBS)
                apply_subs();
        }
        else if (g_section != UI_SEC_SUBS)
            ;
        else if (g_jobResult == 0)
            strcpy(g_status, T(S_SUBS_EMPTY));
        else
            _snprintf(g_status, sizeof(g_status) - 1, T(S_ERROR), g_jobErr);
        g_status[sizeof(g_status) - 1] = 0;
        break;

    case JOB_COMMUNITY:
        if (g_jobResult >= 0)
        {
            memcpy(g_commVideos, g_tmp, sizeof(YtVideo) * g_jobResult);
            g_commCount = g_jobResult;
            g_commTick = GetTickCount();
            g_commLoaded = 1;
            g_commVer = community_version();
        }
        if (g_section != UI_SEC_COMMUNITY)
            break;
        if (g_jobResult > 0)
            apply_community();
        else if (g_jobResult == 0)
            strcpy(g_status, T(S_COMMUNITY_EMPTY));
        else
        {
            _snprintf(g_status, sizeof(g_status) - 1, T(S_ERROR), g_jobErr);
            g_status[sizeof(g_status) - 1] = 0;
        }
        break;

    case JOB_CATEGORY:
        if (!category_id(g_section) || strcmp(category_id(g_section), g_jobArg) != 0)
            break;   /* ya se fue a otra seccion */
        if (g_jobResult > 0)
        {
            g_listJump = 0;
            grid_reset();   /* lista nueva: cada fila desde su primera tarjeta */
            memcpy(g_videos, g_tmp, sizeof(YtVideo) * g_jobResult);
            g_count = g_jobResult;
            g_isHome = 0;
            g_secCount = 0;
            g_status[0] = 0;
        }
        else if (g_jobResult == 0)
            _snprintf(g_status, sizeof(g_status) - 1, T(S_NO_CAT), ui_section_name(g_section));
        else
            _snprintf(g_status, sizeof(g_status) - 1, T(S_ERROR), g_jobErr);
        g_status[sizeof(g_status) - 1] = 0;
        break;

    case JOB_SEARCH:
        if (g_section != UI_SEC_SEARCH)
            break;   /* ya se fue a otra seccion */
        if (g_jobResult > 0)
        {
            g_listJump = 0;
            grid_reset();   /* lista nueva: cada fila desde su primera tarjeta */
            memcpy(g_videos, g_tmp, sizeof(YtVideo) * g_jobResult);
            g_count = g_jobResult;
            strcpy(g_query, g_jobArg);
            g_isHome = 0;
            g_secCount = 0;
            g_status[0] = 0;
            sfx_play(SFX_RESULTS);   /* sonido de "resultado de busqueda" */
        }
        else if (g_jobResult == 0)
        {
            _snprintf(g_status, sizeof(g_status) - 1, T(S_NO_RESULTS), g_jobArg);
        }
        else
        {
            _snprintf(g_status, sizeof(g_status) - 1, T(S_ERROR), g_jobErr);
        }
        g_status[sizeof(g_status) - 1] = 0;
        break;

    case JOB_PLAYER:
        g_playerOk = g_jobResult;
        if (g_playerOk)
        {
            g_videoStatus[0] = 0;
            build_format_list();
            g_trackSel = media_default_track(&g_player);
            g_capSel = media_default_caption(&g_player, g_trackSel);
            g_capTouched = 0;
            g_focus = FOCUS_FORMATS;
            g_fmtSel = default_quality();
            if (g_fmtSel >= FMT_ROWS)
                g_fmtTop = g_fmtSel - FMT_ROWS + 1;
            g_autoStart = g_autoPlay && g_screen == SCREEN_VIDEO;   /* lo arranca el ciclo principal */
        }
        else
        {
            _snprintf(g_videoStatus, sizeof(g_videoStatus) - 1, T(S_ERROR), g_jobErr);
            g_videoStatus[sizeof(g_videoStatus) - 1] = 0;
        }
        break;

    case JOB_PROBE:
    case JOB_HLS:
        g_probeOk = g_jobResult;
        strcpy(g_probeText, g_jobText);
        break;

    case JOB_ANALYZE:
        g_analysisOk = g_jobResult;
        g_analysisMs = g_jobMs;
        strcpy(g_analysisErr, g_jobErr);
        break;

    case JOB_RELATED:
        /* el siguiente video: el primer relacionado que no se haya visto */
        if (g_endState == END_SEARCH && g_screen == SCREEN_PLAY)
        {
            int i, j, n = hist_copy(g_recent, 10), seen;

            g_endState = END_NONE;
            for (i = 0; i < g_jobResult; ++i)
            {
                seen = !g_scratch[i].id[0] || strcmp(g_scratch[i].id, g_curVideo.id) == 0;
                for (j = 0; j < n && !seen; ++j)
                    seen = strcmp(g_scratch[i].id, g_recent[j].id) == 0;
                if (!seen)
                {
                    g_nextVideo = g_scratch[i];
                    g_nextIdx = -1;
                    g_endState = END_COUNT;
                    g_upNextStart = GetTickCount();
                    g_upSel = 0;
                    break;
                }
            }
        }
        break;
    }

    OutputDebugStringA("[main] tarea terminada\n");
    return type;
}

/* Pide el Inicio (con los ultimos vistos del historial) */
static void start_home(void)
{
    if (g_jobBusy)
        return;
    g_homeHistCount = hist_copy(g_homeHist, 2);
    g_status[0] = 0;
    start_job(JOB_HOME, "");
}

/* ------------------------------------------------------------------ */
/* Secciones del menu lateral                                          */

/* Inicio: el guardado, o pedirlo si todavia no hay */
static void show_home(void)
{
    g_section = UI_SEC_HOME;
    g_screen = SCREEN_LIST;
    if (g_homeCount > 0)
        apply_home();
    else
        start_home();
}

/* Historial: los ultimos vistos (game:\historial.txt), el mas nuevo arriba */
static void show_history(void)
{
    g_section = UI_SEC_HISTORY;
    g_screen = SCREEN_LIST;
    g_count = hist_copy(g_videos, MAX_VIDEOS);
    g_isHome = 0;
    g_secCount = 0;
    g_status[0] = 0;
    if (g_count == 0)
        strcpy(g_status, T(S_NO_HISTORY));
    g_listJump = 0;
    grid_reset();   /* lista nueva: cada fila desde su primera tarjeta */
}

static void open_google(void);

/* Recomendados: los guardados, o pedirlos (force = X, recargar) */
static void show_community(int force)
{
    g_section = UI_SEC_COMMUNITY;
    g_screen = SCREEN_LIST;
    g_count = 0;
    g_secCount = 0;
    g_isHome = 0;
    g_status[0] = 0;
    g_listJump = 0;
    grid_reset();
    if (!community_enabled())
    {
        strcpy(g_status, T(S_COMMUNITY_OFF));
        return;
    }
    if (!force && g_commLoaded && GetTickCount() - g_commTick < COMM_KEEP_MS &&
        g_commVer == community_version())
    {
        if (g_commCount > 0)
            apply_community();
        else
            strcpy(g_status, T(S_COMMUNITY_EMPTY));
        return;
    }
    if (!start_job(JOB_COMMUNITY, "") && g_commCount > 0)
        apply_community();
}

/* Suscripciones: las guardadas, o pedirlas (force = X, recargar) */
static void show_subs(int force)
{
    g_section = UI_SEC_SUBS;
    g_screen = SCREEN_LIST;
    g_count = 0;
    g_secCount = 0;
    g_isHome = 0;
    g_status[0] = 0;
    g_listJump = 0;
    grid_reset();
    if (cuenta_state() == CUENTA_NOCLIENT)
    {
        _snprintf(g_status, sizeof(g_status) - 1, T(S_GACCOUNT_X), T(S_G_NOCLIENT));
        g_status[sizeof(g_status) - 1] = 0;
        return;
    }
    if (!cuenta_signed_in())
    {
        strcpy(g_status, T(S_SUBS_NEED));
        return;
    }
    if (!force && g_subsCount > 0 && GetTickCount() - g_subsTick < SUBS_KEEP_MS &&
        g_subsVer == cuenta_sub_version())
    {
        apply_subs();
        return;
    }
    if (!start_job(JOB_SUBS, "") && g_subsCount > 0)
        apply_subs();   /* habia otra tarea: por ahora las de antes */
}

static void open_category(int sec)
{
    if (g_jobBusy)
        return;
    g_section = sec;
    g_screen = SCREEN_LIST;
    g_count = 0;
    g_status[0] = 0;
    g_listJump = 0;
    grid_reset();   /* lista nueva: cada fila desde su primera tarjeta */
    g_jobInt = sec;
    start_job(JOB_CATEGORY, category_id(sec));
}

static void open_search(void)
{
    g_screen = SCREEN_SEARCH;
    g_headerFocus = 0;
    g_menuFocus = 0;
    search_open();
}

/* A sobre un renglon del menu lateral */
static void activate_section(int sec)
{
    if (sec == UI_SEC_PROFILE)
    {
        ui_profile_signin();   /* el menu sigue abierto */
        return;
    }
    g_menuFocus = 0;
    switch (sec)
    {
    case UI_SEC_SEARCH:
        open_search();
        break;
    case UI_SEC_HOME:
        if (!g_jobBusy || g_homeCount > 0)
            show_home();
        break;
    case UI_SEC_HISTORY:
        show_history();
        break;
    case UI_SEC_SUBS:
        show_subs(0);
        break;
    case UI_SEC_COMMUNITY:
        show_community(0);
        break;
    case UI_SEC_SETTINGS:
        g_screen = SCREEN_SETTINGS;
        g_setSel = 0;
        break;
    default:
        open_category(sec);
        break;
    }
}

/* Botones con el menu desplegado */
static void menu_input(WORD pressed, int move)
{
    if (move < 0 && g_menuSel > 0)
        --g_menuSel;
    if (move > 0 && g_menuSel < UI_SEC_COUNT - 1)
        ++g_menuSel;
    if (pressed & XINPUT_GAMEPAD_A)
        activate_section(g_menuSel);
    else if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_BACK))
        g_menuFocus = 0;
}

/* Start: el cuadro de "Salir" (ninguno de los dos botones elegido) */
/* Salir al dashboard (cuadro de Salir o "Salir ahora" de actualizaciones) */
static void app_exit(void)
{
    play_stop();
    gv_close();
    presence_stop();    /* borra el registro de esta consola */
    update_stop();
    cuenta_shutdown();
    XEnableScreenSaver(TRUE);
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
}

static void open_exit(void)
{
    g_exitDlg = 1;
    g_exitSel = -1;
}

static void open_menu(int current)
{
    avatar_reset();     /* al abrir, el avatar mira de frente */
    g_menuFocus = 1;
    g_menuSel = current;
    g_headerFocus = 0;
}

/* ------------------------------------------------------------------ */
/* Control                                                             */

#ifndef XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
#define XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE 8689
#endif
#ifndef XINPUT_GAMEPAD_TRIGGER_THRESHOLD
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30
#endif

/* Eje de palanca a -1..1 con zona muerta (la de XInput para la derecha) */
static float stick_axis(SHORT v)
{
    const float dz = (float)XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    float f = (float)v;

    if (f > dz)
        return (f - dz) / (32767.0f - dz);
    if (f < -dz)
        return (f + dz) / (32768.0f - dz);
    return 0.0f;
}

static WORD read_buttons(void)
{
    XINPUT_STATE st;
    WORD b;

    ZeroMemory(&st, sizeof(st));
    if (XInputGetState(0, &st) != ERROR_SUCCESS)
        return 0;

    b = st.Gamepad.wButtons;
    g_padRaw = b;
    if (st.Gamepad.sThumbLY >  STICK_LIMIT) b |= BTN_UP;
    if (st.Gamepad.sThumbLY < -STICK_LIMIT) b |= BTN_DOWN;
    /* izq/der de la palanca: solo lo usa el cuadro de "Salir" */
    g_stickX = st.Gamepad.sThumbLX > STICK_LIMIT ? 1 : (st.Gamepad.sThumbLX < -STICK_LIMIT ? -1 : 0);
    /* palanca derecha y gatillos: los usa el avatar del menu lateral */
    g_rx = stick_axis(st.Gamepad.sThumbRX);
    g_ry = stick_axis(st.Gamepad.sThumbRY);
    g_lt = st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? st.Gamepad.bLeftTrigger / 255.0f : 0.0f;
    g_rt = st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? st.Gamepad.bRightTrigger / 255.0f : 0.0f;
    /* uso del mando (para la atenuacion por inactividad) */
    if (b || g_stickX || g_rx != 0.0f || g_ry != 0.0f || g_lt > 0.0f || g_rt > 0.0f)
        g_lastInput = GetTickCount();
    return b;
}

/* Capa oscura encima de todo cuando esta atenuada (entra suave en 2 s) */
static void draw_idle_dim(void)
{
    DWORD t;
    unsigned int a;

    if (!g_dimmed)
        return;
    t = GetTickCount() - g_dimStart;
    a = t >= IDLE_FADE_MS ? 0xC0 : (unsigned int)(0xC0 * t / IDLE_FADE_MS);
    gfx_rect(0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, a << 24);
}

/* Mueve sel dentro de [0, count-1] y ajusta top para que se vea */
static void apply_move(int *sel, int *top, int move, int count, int visible)
{
    *sel += move;
    if (*sel > count - 1) *sel = count - 1;
    if (*sel < 0) *sel = 0;
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + visible) *top = *sel - visible + 1;
}

/* ------------------------------------------------------------------ */
/* Texto para mostrar                                                  */

/* canal · duracion · vistas · fecha (sin la duracion si withLength es 0:
 * en la lista ya va encima de la miniatura) */
static void join_meta(const YtVideo *v, char *out, int size, int withLength)
{
    const char *parts[4];
    int i;

    parts[0] = v->channel;
    parts[1] = withLength ? v->length : "";
    parts[2] = v->views;
    parts[3] = v->published;

    out[0] = 0;
    for (i = 0; i < 4; ++i)
    {
        if (!parts[i][0])
            continue;
        if (out[0] && (int)strlen(out) + 7 < size)
            strcat(out, DOT);
        if ((int)(strlen(out) + strlen(parts[i])) < size)
            strcat(out, parts[i]);
    }
}

static void format_duration(int secs, char *out, int size)
{
    if (secs >= 3600)
        _snprintf(out, size - 1, "%d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
    else
        _snprintf(out, size - 1, "%d:%02d", secs / 60, secs % 60);
    out[size - 1] = 0;
}

/* Una fila de la tabla de formatos, en columnas (la fuente es monoespaciada) */
static void format_row(const YtFormat *f, char *out, int size)
{
    char detail[32];
    char codecs[16];
    char mb[16];

    if (f->isVideo)
        _snprintf(detail, sizeof(detail) - 1, "%dx%d %dfps", f->width, f->height, f->fps);
    else if (f->isAudio)
        _snprintf(detail, sizeof(detail) - 1, "%d Hz", f->sampleRate);
    else
        strcpy(detail, "-");
    detail[sizeof(detail) - 1] = 0;

    fit_text(f->codecs[0] ? f->codecs : "?", 13, codecs, sizeof(codecs));

    if (f->contentLength > 0.0)
        _snprintf(mb, sizeof(mb) - 1, "%.1f", f->contentLength / 1048576.0);
    else
        strcpy(mb, "-");
    mb[sizeof(mb) - 1] = 0;

    _snprintf(out, size - 1, FMT_COLUMNS "%s",
              f->itag, f->isVideo ? "video" : (f->isAudio ? "audio" : "?"),
              codecs, detail, f->bitrate / 1000, mb, f->url[0] ? "" : "  (cifrada)");
    out[size - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* Dibujo                                                              */

/* Renglon de la seccion, arriba de la lista */
static float g_hdrTextW = 700.0f;

static void header_text(float y, unsigned int color, const char *s)
{
    char fitted[320];

    gfx_fit_text(s, g_hdrTextW, 1.0f, fitted, sizeof(fitted));
    gfx_text(LIST_X, y, 1.0f, color, fitted);
}

#define LOGO_H   56.0f   /* el PNG ya trae 3 px de borde alrededor */
#define HDR_Y    86.0f   /* renglon de la seccion */
#define PILL_Y   26.0f   /* barra de busqueda */
#define PILL_H   48.0f

/* Pantallas con menu lateral y barra de busqueda */
static int has_rail(void)
{
    return g_screen == SCREEN_LIST || g_screen == SCREEN_CHANNEL ||
           g_screen == SCREEN_SEARCH || g_screen == SCREEN_SETTINGS || g_screen == SCREEN_MANUAL ||
           g_screen == SCREEN_GOOGLE;
}

/* Seccion que se marca en el menu */
static int rail_current(void)
{
    if (g_screen == SCREEN_SEARCH)
        return UI_SEC_SEARCH;
    if (g_screen == SCREEN_SETTINGS || g_screen == SCREEN_MANUAL || g_screen == SCREEN_GOOGLE)
        return UI_SEC_SETTINGS;
    return g_section;
}

/* Barra de busqueda: "Buscar", o lo que se escribe en el teclado */
static void draw_search_pill(float x, float w)
{
    char line[256];
    const char *t = g_screen == SCREEN_SEARCH ? search_text() : "";
    int focused = g_headerFocus == HDR_SEARCH || g_screen == SCREEN_SEARCH;
    float tx = x + 58.0f, cw;

    if (g_headerFocus == HDR_SEARCH)
        ui_round_rect(x - 3.0f, PILL_Y - 3.0f, w + 6.0f, PILL_H + 6.0f, PILL_H * 0.5f + 3.0f, COL_ACCENT);
    ui_round_rect(x, PILL_Y, w, PILL_H, PILL_H * 0.5f, g_th->pill);
    gfx_icon(UI_ICON_M_SEARCH, x + 18.0f, PILL_Y + 12.0f, 24.0f, focused ? COL_TEXT : COL_DIM);
    if (t[0])
    {
        /* si no cabe, se ve el final (lo ultimo que se escribio) */
        cw = w - 90.0f;
        while (*t && gfx_text_width(1.0f, t) > cw)
        {
            ++t;
            while (((unsigned char)*t & 0xC0) == 0x80)
                ++t;
        }
        gfx_text(tx, PILL_Y + 12.0f, 1.0f, COL_TEXT, t);
        tx += gfx_text_width(1.0f, t) + 2.0f;
    }
    else
    {
        gfx_fit_text(T(S_SEC_SEARCH), w - 90.0f, 1.0f, line, sizeof(line));
        gfx_text(tx, PILL_Y + 12.0f, 1.0f, COL_DIM, line);
    }
    if (g_screen == SCREEN_SEARCH && (GetTickCount() / 500) % 2 == 0)
        gfx_rect(t[0] ? tx : x + 56.0f, PILL_Y + 11.0f, 2.0f, 26.0f, COL_ACCENT);   /* cursor */
}

static void draw_header(void)
{
    char buf[320];
    char fitted[128];
    int dots = (int)((GetTickCount() / 400) % 4);
    float logoW, logoX, leftEnd;

    /* logo arriba a la derecha (si no cargo, el nombre en texto) */
    logoW = gfx_logo_width(0, LOGO_H);
    if (logoW > 0.0f)
    {
        logoX = SCREEN_W - SAFE_X - logoW + 3.0f;
        gfx_logo(logoX, 50.0f - LOGO_H * 0.5f, LOGO_H);
    }
    else
    {
        logoW = gfx_text_width(1.5f, "TubeBox360");
        logoX = SCREEN_W - SAFE_X - logoW;
        gfx_text(logoX, 32.0f, 1.5f, COL_TEXT, "TubeBox360");
        gfx_rect(logoX - 20.0f, 36.0f, 8.0f, 30.0f, COL_ACCENT);
        logoX -= 20.0f;
    }
    if (!has_rail())
        return;   /* formatos y carga: solo el logo */

    /* la barra de busqueda hasta el logo (el tema esta en Configuracion) */
    leftEnd = logoX;
    if (update_enabled())
    {
        /* icono de actualizaciones entre la barra y el logo: gris; elegido,
         * blanco sobre rojo; punto amarillo si hay version nueva */
        float ix, iy = 32.0f;
        leftEnd -= 52.0f;
        ix = leftEnd - 14.0f;
        if (g_headerFocus == HDR_UPDATE && g_screen == SCREEN_LIST && !g_menuFocus)
        {
            gfx_circle(ix - 5.0f, iy - 5.0f, 46.0f, COL_ACCENT);
            gfx_icon(UI_ICON_UPDATE, ix, iy, 36.0f, 0xFFFFFFFF);
        }
        else
            gfx_icon(UI_ICON_UPDATE, ix, iy, 36.0f, g_lightTheme ? 0xFF9A9A9A : 0xFF7A7A7A);
        if (update_available())
        {
            gfx_circle(ix + 23.0f, iy - 5.0f, 16.0f, g_lightTheme ? 0xFFFFFFFF : 0xFF202020);
            gfx_circle(ix + 25.0f, iy - 3.0f, 12.0f, 0xFFFFC400);
        }
    }
    draw_search_pill(LIST_X, leftEnd - 30.0f - LIST_X);
    g_hdrTextW = SCREEN_W - SAFE_X - LIST_X;

    if (g_screen != SCREEN_LIST)
        return;   /* el canal, el teclado y la configuracion dicen lo suyo */
    if (is_busy(JOB_SEARCH))
    {
        fitw(g_jobArg, 40, 1.0f, fitted, sizeof(fitted));
        _snprintf(buf, sizeof(buf) - 1, T(S_SEARCHING_Q), fitted);
        buf[sizeof(buf) - 1] = 0;
        _snprintf(buf + strlen(buf), sizeof(buf) - 1 - strlen(buf), "%.*s", dots, "...");
        buf[sizeof(buf) - 1] = 0;
        header_text(HDR_Y, COL_SOFT, buf);
    }
    else if (is_busy(JOB_HOME))
    {
        _snprintf(buf, sizeof(buf) - 1, "%s%.*s", T(S_LOADING_REC), dots, "...");
        buf[sizeof(buf) - 1] = 0;
        header_text(HDR_Y, COL_SOFT, buf);
    }
    else if (is_busy(JOB_CATEGORY) || (is_busy(JOB_SUBS) && g_section == UI_SEC_SUBS) ||
             (is_busy(JOB_COMMUNITY) && g_section == UI_SEC_COMMUNITY))
    {
        _snprintf(buf, sizeof(buf) - 1, T(S_LOADING_X), ui_section_name(g_section));
        buf[sizeof(buf) - 1] = 0;
        _snprintf(buf + strlen(buf), sizeof(buf) - 1 - strlen(buf), "%.*s", dots, "...");
        buf[sizeof(buf) - 1] = 0;
        header_text(HDR_Y, COL_SOFT, buf);
    }
    else if (g_status[0])
        header_text(HDR_Y, COL_ACCENT, g_status);
    else if (g_count > 0 && g_isHome)
    {
        /* Inicio (cada fila de la cuadricula ya dice su seccion) */
        header_text(HDR_Y, COL_DIM, T(S_SEC_HOME));
    }
    else if (g_count > 0 && g_section == UI_SEC_SEARCH)
    {
        fitw(g_query, 50, 1.0f, fitted, sizeof(fitted));
        _snprintf(buf, sizeof(buf) - 1, "\"%s\"" DOT, fitted);
        _snprintf(buf + strlen(buf), sizeof(buf) - 1 - strlen(buf), T(S_N_VIDEOS), g_count);
        buf[sizeof(buf) - 1] = 0;
        header_text(HDR_Y, COL_DIM, buf);
    }
    else if (g_count > 0)
    {
        _snprintf(buf, sizeof(buf) - 1, "%s" DOT, ui_section_name(g_section));
        _snprintf(buf + strlen(buf), sizeof(buf) - 1 - strlen(buf), T(S_N_VIDEOS), g_count);
        buf[sizeof(buf) - 1] = 0;
        header_text(HDR_Y, COL_DIM, buf);
    }
}

/* Ayudas de los botones abajo, con el icono de cada boton */
static void draw_footer(const char *spec)
{
    if (has_rail() && g_menuFocus)
        return;   /* menu abierto: abajo van los usuarios activos (sin recuadro) */
    ui_footer(has_rail() ? LIST_X : SAFE_X, spec);
}

/* Ayudas con los textos del idioma: hints("A=%s|B=%s", T(H_WATCH), T(H_BACK)) */
static const char *hints(const char *fmt, ...)
{
    static char buf[640];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    return buf;
}

/* --- Tema y configuracion (game:\config.ini) --- */

static void set_theme(int light)
{
    ui_set_theme(light);
    thumb_set_colors(g_th->holder, g_lightTheme ? 0xFFC8C8C8 : 0xFF3A3A3A,
                     g_lightTheme ? 0xFF555555 : 0xFFD0D0D0);
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void config_save(void)
{
    FILE *f = fopen(CONFIG_PATH, "wb");

    if (!f)
        return;
    fputs("; TubeBox360\r\n", f);
    fputs(g_lightTheme ? "tema=claro\r\n" : "tema=oscuro\r\n", f);
    fprintf(f, "idioma=%s\r\n", lang_code());
    fprintf(f, "region=%s\r\n", lang_region_code(lang_region()));   /* auto = segun el idioma */
    fputs(sfx_enabled() ? "sonidos=si\r\n" : "sonidos=no\r\n", f);
    fputs(g_avatarOn ? "avatar=si\r\n" : "avatar=no\r\n", f);
    /* predeterminados: audio 0=app 1=original 2=es 3=en 4=pt;
     * subtitulos 0=automaticos 1=apagados 2=es 3=en 4=pt; calidad en p */
    fprintf(f, "audio=%d\r\nsubtitulos=%d\r\ncalidad=%d\r\n", g_prefAudio, g_prefSubs, kQualities[g_prefQuality]);
    /* red: conexion=directa (HTTPS desde la consola) o proxy (la PC con ProtoHTTPS) */
    {
        int port;
        const char *ip = net_get_proxy(&port);
        fputs(net_is_direct() ? "conexion=directa\r\n" : "conexion=proxy\r\n", f);
        if (!g_tlsVerifyCfg)
            fputs("certificados=0\r\n", f);   /* (solo si se quito a mano: por omision se verifican) */
        fprintf(f, "proxy=%s:%d\r\n", ip, port);
    }
    fclose(f);
}

static void config_load(void)
{
    char line[128];
    FILE *f = fopen(CONFIG_PATH, "rb");

    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "tema=", 5) == 0)
            set_theme(strncmp(line + 5, "claro", 5) == 0);
        else if (strncmp(line, "idioma=", 7) == 0)
            lang_set(strncmp(line + 7, "en", 2) == 0 ? LANG_EN : strncmp(line + 7, "pt", 2) == 0 ? LANG_PT :
                     strncmp(line + 7, "ja", 2) == 0 ? LANG_JA : strncmp(line + 7, "ar", 2) == 0 ? LANG_AR : LANG_ES);
        else if (strncmp(line, "region=", 7) == 0)
            lang_set_region(lang_region_from_code(line + 7));
        else if (strncmp(line, "sonidos=", 8) == 0)
            sfx_enable(strncmp(line + 8, "no", 2) != 0);   /* sonidos=no los apaga */
        else if (strncmp(line, "proxy=", 6) == 0)
        {
            /* proxy=192.168.1.70:8080 (la PC con ProtoHTTPS) */
            char ip[32];
            int k = 0, port = 0;
            const char *c = line + 6;
            while (*c && *c != ':' && *c != '\r' && *c != '\n' && k < (int)sizeof(ip) - 1)
                ip[k++] = *c++;
            ip[k] = 0;
            if (*c == ':')
                port = atoi(c + 1);
            net_set_proxy(ip, port);
        }
        else if (strncmp(line, "conexion=", 9) == 0)
            net_set_direct(strncmp(line + 9, "proxy", 5) != 0);   /* conexion=proxy usa la PC */
        else if (strncmp(line, "certificados=", 13) == 0)
        {
            /* certificados=0: no verificar HTTPS (solo para diagnosticar) */
            g_tlsVerifyCfg = line[13] != '0';
            net_set_verify(g_tlsVerifyCfg);
        }
        else if (strncmp(line, "avatar=", 7) == 0)
            g_avatarOn = strncmp(line + 7, "no", 2) != 0;
        else if (strncmp(line, "audio=", 6) == 0)
            g_prefAudio = clamp_int(atoi(line + 6), 0, PREF_AUDIO_COUNT - 1);
        else if (strncmp(line, "subtitulos=", 11) == 0)
            g_prefSubs = clamp_int(atoi(line + 11), 0, PREF_SUBS_COUNT - 1);
        else if (strncmp(line, "calidad=", 8) == 0)
        {
            int q = atoi(line + 8), i;
            for (i = 0; i < QUALITY_COUNT; ++i)
                if (kQualities[i] == q)
                    g_prefQuality = i;
        }
    }
    fclose(f);
}

/* Idioma de la app (Configuracion): textos, y lo que se le pide a YouTube */
static void set_language(int lang)
{
    lang_set(lang);
    config_save();
    g_homeCount = 0;   /* el Inicio guardado venia en el otro idioma */
    if (g_section == UI_SEC_HOME && !g_jobBusy)
        start_home();
}

/* Duracion sobre la esquina de la miniatura, como en YouTube */
static void draw_badge(float right, float bottom, const char *text)
{
    float w = gfx_text_width(0.75f, text) + 10.0f;

    gfx_rect(right - w - 4.0f, bottom - 24.0f, w, 20.0f, 0xD0000000);
    gfx_text(right - w + 1.0f, bottom - 23.0f, 0.75f, COL_WHITE, text);
}

/* Filas de videos con miniatura (lista principal y pagina del canal).
 * inChannel: sin avatar ni nombre del canal (ya estan arriba). */
static void draw_rows(const YtVideo *vids, int count, int sel, int top, float y0, int rows,
                      float rowH, int inChannel)
{
    int row, i;
    float y, tx, ty, mx, trackH, thumbH, thumbY;
    char line[512];
    char meta[512];
    const char *rest;
    const YtVideo *v;
    unsigned int color;

    for (row = 0; row < rows; ++row)
    {
        i = top + row;
        if (i >= count)
            break;
        v = &vids[i];
        color = (i == sel) ? COL_TEXT : COL_SOFT;

        y = y0 + row * rowH;
        if (i == sel)
        {
            gfx_rect(LIST_X, y, SCREEN_W - SAFE_X - LIST_X, rowH - 6.0f, COL_SEL);
            gfx_rect(LIST_X, y, 6.0f, rowH - 6.0f, COL_ACCENT);
        }

        /* Inicio: el nombre de la seccion en la primera fila de cada una
         * (abajo a la derecha, donde la linea de datos casi nunca llega) */
        if (!inChannel && g_secCount > 0 && (i == 0 || g_videoSec[i] != g_videoSec[i - 1]) &&
            g_videoSec[i] >= 0 && g_videoSec[i] < g_secCount)
        {
            fitw(g_secNames[g_videoSec[i]], 50, 0.75f, line, sizeof(line));
            gfx_text(SCREEN_W - SAFE_X - 12.0f - gfx_text_width(0.75f, line), y + 70.0f, 0.75f,
                     COL_ACCENT, line);
        }

        /* miniatura con la duracion encima */
        tx = LIST_X + 16.0f;
        ty = y + 3.0f;
        thumb_draw(v->id, tx, ty, LIST_THUMB_W, LIST_THUMB_H);
        if (v->length[0])
            draw_badge(tx + LIST_THUMB_W, ty + LIST_THUMB_H, v->length);

        /* titulo en hasta 2 lineas y abajo [avatar] canal · vistas · fecha;
         * con titulo de una linea, el bloque va centrado junto a la miniatura */
        rest = gfx_wrap_text(v->title, ROW_CHARS * (float)GFX_CHAR_W, 1.0f, line, sizeof(line));
        ty = *rest ? y + 6.0f : y + 19.0f;
        gfx_text(ROW_TEXT_X, ty, 1.0f, color, line);
        if (*rest)
        {
            fitw(rest, ROW_CHARS, 1.0f, line, sizeof(line));
            ty += 26.0f;
            gfx_text(ROW_TEXT_X, ty, 1.0f, color, line);
        }

        mx = ROW_TEXT_X;
        if (inChannel)
        {
            meta[0] = 0;
            if (v->views[0])
                strcat(meta, v->views);
            if (v->views[0] && v->published[0])
                strcat(meta, DOT);
            if ((int)(strlen(meta) + strlen(v->published)) < (int)sizeof(meta))
                strcat(meta, v->published);
        }
        else
        {
            thumb_draw_avatar(v->avatar, mx, ty + 30.0f, AVATAR_SIZE, v->channel);
            mx += AVATAR_SIZE + 8.0f;
            join_meta(v, meta, sizeof(meta), 0);
        }
        fitw(meta, ROW_CHARS - (inChannel ? 0 : 3), 1.0f, line, sizeof(line));
        gfx_text(mx, ty + 32.0f, 1.0f, COL_DIM, line);
    }

    /* pedir antes las miniaturas de las filas que estan por aparecer */
    for (i = top + rows; i < top + rows + PREFETCH_ROWS && i < count; ++i)
    {
        thumb_prefetch(vids[i].id);
        if (!inChannel)
            thumb_prefetch_avatar(vids[i].avatar);
    }
    for (i = top - 1; i >= 0 && i >= top - 2; --i)
        thumb_prefetch(vids[i].id);

    if (count > rows)
    {
        trackH = rows * rowH - 6.0f;
        thumbH = trackH * rows / count;
        thumbY = y0 + (trackH - thumbH) * top / (count - rows);
        gfx_rect(SCREEN_W - SAFE_X + 12.0f, y0, 4.0f, trackH, COL_BAR);
        gfx_rect(SCREEN_W - SAFE_X + 12.0f, thumbY, 4.0f, thumbH, COL_DIM);
    }
}

/* ---- Cuadricula de tarjetas (Inicio, busqueda, categorias, historial) ----
 * Como YouTube TV clasico, en grande: filas de 5 tarjetas (miniatura y
 * debajo titulo, canal, vistas y fecha), hasta 6 filas (30 videos). A lo
 * ancho se ven 3 tarjetas (y la orilla de la cuarta); izq/der desliza la
 * fila para ver las otras 2. A lo alto se ve la fila elegida completa y la
 * mitad de la miniatura de la siguiente, bajo una sombra degradada que
 * indica que hay mas abajo (los botones de ayuda van encima de la sombra).
 * En el Inicio cada fila lleva arriba el nombre de su seccion. */
#define GRID_COLS    5
#define GRID_ROWS_MAX ((YT_LIST_MAX + GRID_COLS - 1) / GRID_COLS)   /* (el canal trae hasta 50) */
#define GRID_SHOWN   3           /* tarjetas completas a lo ancho */
#define GRID_GAP     24.0f
#define GRID_Y0      118.0f
#define GRID_LABEL   32.0f       /* nombre de la seccion arriba de la fila */
#define GRID_STEP    400.0f      /* de una fila a la otra (de la siguiente se ve media miniatura) */
#define GRID_CLIP_T  110.0f      /* zona visible de la cuadricula */
#define GRID_CLIP_B  646.0f
#define GRID_TITLE_S 1.0f
#define GRID_META_S  0.85f
#define GRID_SHADOW_Y 404.0f     /* donde empieza la sombra de abajo */

/* Estado de una cuadricula: Inicio/busqueda y la pagina del canal tienen
 * cada una la suya */
typedef struct
{
    int   top;                         /* fila elegida (arriba) */
    float scroll;                      /* la misma, con decimales (para deslizar) */
    int   rowLeft[GRID_ROWS_MAX];      /* primera tarjeta a la vista en cada fila */
    float rowScroll[GRID_ROWS_MAX];
} GridState;

static GridState g_grid;               /* Inicio y busqueda */
static GridState g_chGrid;             /* pagina del canal */
static void *g_shadowTex = NULL;
static int   g_shadowTried = 0;

static float grid_card_w(void)
{
    return (SCREEN_W - SAFE_X - LIST_X - (GRID_SHOWN - 1) * GRID_GAP) / GRID_SHOWN;
}

/* Lista nueva: todo al principio */
static void grid_state_reset(GridState *g)
{
    ZeroMemory(g, sizeof(*g));
}

static void grid_reset(void)
{
    grid_state_reset(&g_grid);
}

/* Mueve la tarjeta elegida: dx dentro de la fila, dy entre filas */
static void grid_move(int *sel, int dx, int dy, int count)
{
    int col = *sel % GRID_COLS, row = *sel / GRID_COLS;
    int rows = (count + GRID_COLS - 1) / GRID_COLS, s;

    if (count <= 0)
        return;
    if (dx)
    {
        col += dx;
        if (col >= 0 && col < GRID_COLS && row * GRID_COLS + col < count)
            *sel = row * GRID_COLS + col;
    }
    if (dy)
    {
        row += dy;
        if (row < 0)
            row = 0;
        if (row > rows - 1)
            row = rows - 1;
        s = row * GRID_COLS;     /* otra fila: desde su primera tarjeta */
        *sel = s < count ? s : count - 1;
    }
}

static float slide(float cur, float target)
{
    float d = target - cur;

    if (d > -0.01f && d < 0.01f)
        return target;
    return cur + d * 0.30f;
}

/* y0 = donde va la fila elegida (GRID_Y0 en Inicio; mas abajo en el canal,
 * bajo su encabezado). sections = poner el nombre de la seccion de Inicio. */
static int g_gridHideSel = 0;   /* 1 = no marcar la tarjeta elegida (el foco esta en otro lado) */

static void draw_grid(const YtVideo *vids, int count, int sel, GridState *g, float y0, int sections)
{
    float cw = grid_card_w(), th = cw * 9.0f / 16.0f, x, y, ty, my;
    float shadowY = GRID_SHADOW_Y + (y0 - GRID_Y0);   /* la sombra va con la fila elegida */
    int rows = (count + GRID_COLS - 1) / GRID_COLS, selRow = sel / GRID_COLS, selCol = sel % GRID_COLS;
    int r, c, i, first, on;
    char line[512], meta[512];
    const char *rest;
    const YtVideo *v;
    unsigned int color;
    long hr;

    if (rows > GRID_ROWS_MAX)
        rows = GRID_ROWS_MAX;
    /* la fila elegida va arriba; en ella, la tarjeta elegida a la vista */
    g->top = selRow;
    if (selRow < GRID_ROWS_MAX)
    {
        if (selCol < g->rowLeft[selRow])
            g->rowLeft[selRow] = selCol;
        if (selCol > g->rowLeft[selRow] + GRID_SHOWN - 1)
            g->rowLeft[selRow] = selCol - (GRID_SHOWN - 1);
    }
    g->scroll = slide(g->scroll, (float)g->top);
    for (r = 0; r < rows; ++r)
        g->rowScroll[r] = slide(g->rowScroll[r], (float)g->rowLeft[r]);

    gfx_clip(LIST_X - 12.0f, GRID_CLIP_T, SCREEN_W - LIST_X + 12.0f, GRID_CLIP_B - GRID_CLIP_T);
    first = (int)g->scroll - 1;
    if (first < 0)
        first = 0;
    for (r = first; r < rows && r <= (int)g->scroll + 2; ++r)
    {
        y = y0 + (r - g->scroll) * GRID_STEP;
        if (y > GRID_CLIP_B || y + GRID_STEP < GRID_CLIP_T)
            continue;

        /* Inicio: la seccion de la fila (la del primer video) */
        i = r * GRID_COLS;
        if (sections && g_secCount > 0 && g_videoSec[i] >= 0 && g_videoSec[i] < g_secCount)
        {
            fitw(g_secNames[g_videoSec[i]], 70, 1.0f, line, sizeof(line));
            gfx_font_bold(1);
            gfx_text(LIST_X, y, 1.0f, COL_TEXT, line);
            gfx_font_bold(0);
        }

        for (c = 0; c < GRID_COLS; ++c)
        {
            i = r * GRID_COLS + c;
            if (i >= count)
                break;
            x = LIST_X + (c - g->rowScroll[r]) * (cw + GRID_GAP);
            if (x > SCREEN_W || x + cw < LIST_X - 12.0f)
                continue;
            v = &vids[i];
            on = i == sel && !g_gridHideSel;
            color = on ? COL_TEXT : COL_SOFT;
            ty = y + GRID_LABEL;
            if (on)
                gfx_rect(x - 8.0f, ty - 8.0f, cw + 16.0f, th + 128.0f, COL_SEL);

            /* imagen: miniatura con su duracion, o el avatar si es un canal */
            if (v->kind == YT_KIND_CHANNEL)
            {
                gfx_rect(x, ty, cw, th, COL_CHIP);
                thumb_draw_avatar(v->avatar, x + (cw - 132.0f) * 0.5f, ty + (th - 132.0f) * 0.5f, 132.0f, v->channel);
            }
            else
            {
                thumb_draw(v->id, x, ty, cw, th);
                if (v->length[0])
                    draw_badge(x + cw, ty + th, v->length);
            }
            if (on)
            {
                /* marco blanco en la elegida */
                gfx_rect(x - 4.0f, ty - 4.0f, cw + 8.0f, 4.0f, COL_WHITE);
                gfx_rect(x - 4.0f, ty + th, cw + 8.0f, 4.0f, COL_WHITE);
                gfx_rect(x - 4.0f, ty, 4.0f, th, COL_WHITE);
                gfx_rect(x + cw, ty, 4.0f, th, COL_WHITE);
            }

            /* titulo en 2 lineas */
            my = ty + th + 10.0f;
            rest = gfx_wrap_text(v->title, cw, GRID_TITLE_S, line, sizeof(line));
            gfx_text(x, my, GRID_TITLE_S, color, line);
            if (*rest)
            {
                gfx_fit_text(rest, cw, GRID_TITLE_S, line, sizeof(line));
                gfx_text(x, my + 26.0f, GRID_TITLE_S, color, line);
            }

            /* canal y "vistas � fecha" (en un canal: @canal y suscriptores) */
            my += 58.0f;
            gfx_fit_text(v->kind == YT_KIND_CHANNEL ? v->published : v->channel, cw, GRID_META_S, line, sizeof(line));
            gfx_text(x, my, GRID_META_S, COL_DIM, line);
            meta[0] = 0;
            strncpy(meta, v->views, sizeof(meta) - 1);
            meta[sizeof(meta) - 1] = 0;
            if (v->kind != YT_KIND_CHANNEL)
            {
                if (v->views[0] && v->published[0])
                    strcat(meta, DOT);
                if (strlen(meta) + strlen(v->published) < sizeof(meta))
                    strcat(meta, v->published);
            }
            gfx_fit_text(meta, cw, GRID_META_S, line, sizeof(line));
            gfx_text(x, my + 23.0f, GRID_META_S, COL_DIM, line);
        }
    }
    gfx_clip_off();

    /* sombra degradada abajo (los botones de ayuda se dibujan despues, encima) */
    if (!g_shadowTex && !g_shadowTried)
    {
        g_shadowTried = 1;
        g_shadowTex = gfx_texture_load(g_shadow_png, g_shadow_png_len, g_shadow_w, g_shadow_h, &hr);
    }
    if (g_lightTheme)
    {
        /* tema de dia: sombra blanca. La textura es negra con alfa (tenirla no
           sirve), asi que se dibuja un degradado blanco en dos tramos para que
           entre suave: transparente -> medio -> casi opaco. */
        float shH  = SCREEN_H - shadowY;
        float half = shH * 0.45f;
        gfx_rect_vgrad(UI_RAIL_W, shadowY, SCREEN_W - UI_RAIL_W, half,
                       0x00FFFFFF, 0x99FFFFFF);
        gfx_rect_vgrad(UI_RAIL_W, shadowY + half, SCREEN_W - UI_RAIL_W, shH - half,
                       0x99FFFFFF, 0xF2FFFFFF);
    }
    else if (g_shadowTex)
        gfx_image(g_shadowTex, LIST_X - 12.0f, shadowY, SCREEN_W - LIST_X + 12.0f,
                  SCREEN_H - shadowY, 0xFFFFFFFF);
    else
        gfx_rect_vgrad(LIST_X - 12.0f, shadowY, SCREEN_W - LIST_X + 12.0f, SCREEN_H - shadowY,
                       0x00000000, 0xC0000000);

    /* pedir antes las miniaturas: la fila elegida completa y las 2 de abajo */
    for (i = g->top * GRID_COLS; i < (g->top + 3) * GRID_COLS && i < count; ++i)
    {
        if (vids[i].kind == YT_KIND_CHANNEL)
            thumb_prefetch_avatar(vids[i].avatar);
        else
            thumb_prefetch(vids[i].id);
    }
}

static void draw_list(int sel, int top)
{
    char line[512];
    const char *msg;

    int busy = is_busy(JOB_SEARCH) || is_busy(JOB_HOME) || is_busy(JOB_CATEGORY);
    float mid = (LIST_X + SCREEN_W - SAFE_X) * 0.5f;

    if (g_count == 0)
    {
        msg = is_busy(JOB_SEARCH) ? T(S_SEARCHING) :
              (is_busy(JOB_HOME) || is_busy(JOB_CATEGORY)) ? T(S_LOADING) :
              (g_status[0] ? g_status : T(S_PRESS_Y));
        fitw(msg, 80, 1.0f, line, sizeof(line));
        if (busy)
            ui_draw_spinner(mid, 300.0f, 24.0f);
        gfx_text(mid - gfx_text_width(1.0f, line) * 0.5f, busy ? 350.0f : 330.0f, 1.0f, COL_SOFT, line);
        return;
    }
    (void)top;
    draw_grid(g_videos, g_count, sel, &g_grid, GRID_Y0, 1);
    if (busy)
    {
        /* recargando con la lista vieja a la vista: velo y ruedita encima */
        gfx_rect(LIST_X - 12.0f, GRID_CLIP_T, SCREEN_W - LIST_X + 12.0f, GRID_CLIP_B - GRID_CLIP_T,
                 g_lightTheme ? 0x80FFFFFF : 0x90000000);
        ui_draw_spinner(mid, 360.0f, 24.0f);
    }
}

/* Pagina del canal: banner, avatar, nombre, datos, descripcion y los
 * videos en tarjetas como en Inicio. Al bajar de la primera fila el
 * encabezado se desliza hacia arriba y la fila elegida queda donde en Inicio. */
#define CH_GRID_Y0   290.0f      /* primera fila, bajo el encabezado */
static float g_chHead = 0.0f;    /* cuanto se subio el encabezado */

static int   g_chSubFocus = 0;   /* 1 = elegido el boton Suscribirse del canal */
static DWORD g_chUnsubArm = 0;   /* primer A sobre "Suscrito" (cancelar pide dos) */
static int   g_chSubMsg = 0;     /* texto bajo el boton (S_...) */
static DWORD g_chSubMsgAt = 0;

static void channel_view_reset(void)
{
    grid_state_reset(&g_chGrid);
    g_chHead = 0.0f;
    g_chSubFocus = 0;
    g_chUnsubArm = 0;
    g_chSubMsg = 0;
}

static int channel_can_sub(void)
{
    return g_channelOk && strncmp(g_channel.id, "UC", 2) == 0 && cuenta_state() != CUENTA_NOCLIENT;
}

/* A sobre el boton Suscribirse de la pagina del canal */
static void channel_sub_press(void)
{
    int st;

    if (!cuenta_signed_in())
    {
        open_google();
        return;
    }
    st = cuenta_sub_state(g_channel.id);
    if (st == SUB_UNKNOWN)
        return;
    if (st == SUB_YES)
    {
        if (!g_chUnsubArm || GetTickCount() - g_chUnsubArm > 3000)
        {
            g_chUnsubArm = GetTickCount();
            g_chSubMsg = S_UNSUB_CONFIRM;
            g_chSubMsgAt = GetTickCount();
            return;
        }
        g_chUnsubArm = 0;
        cuenta_sub_set(g_channel.id, 0);
        g_chSubMsg = S_UNSUB_DONE;
    }
    else
    {
        cuenta_sub_set(g_channel.id, 1);
        g_chSubMsg = S_SUB_DONE;
    }
    g_chSubMsgAt = GetTickCount();
}

static void draw_channel(void)
{
    char line[640];
    char buf[640];
    const char *msg;
    int i, dots = (int)((GetTickCount() / 400) % 4);
    float x, off, target;

    if (is_busy(JOB_CHANNEL))
    {
        _snprintf(buf, sizeof(buf) - 1, "%s%.*s", T(S_LOADING_CHANNEL), dots, "...");
        buf[sizeof(buf) - 1] = 0;
        ui_draw_spinner((LIST_X + SCREEN_W - SAFE_X) * 0.5f, 300.0f, 24.0f);
        gfx_text((LIST_X + SCREEN_W - SAFE_X - gfx_text_width(1.0f, buf)) * 0.5f, 350.0f, 1.0f, COL_SOFT, buf);
        draw_footer(hints("B=%s", T(H_BACK)));
        return;
    }
    if (!g_channelOk)
    {
        msg = g_channelStatus[0] ? g_channelStatus : T(S_NO_CHANNEL);
        fitw(msg, LINE_CHARS, 1.0f, line, sizeof(line));
        gfx_text((LIST_X + SCREEN_W - SAFE_X - gfx_text_width(1.0f, line)) * 0.5f, 330.0f, 1.0f, COL_ACCENT, line);
        draw_footer(hints("B=%s", T(H_BACK)));
        return;
    }

    target = (g_chSel / GRID_COLS == 0) ? 0.0f : CH_GRID_Y0 - GRID_Y0;
    g_chHead = slide(g_chHead, target);
    off = -g_chHead;

    /* encabezado (se va hacia arriba al bajar; no tapa la barra de busqueda) */
    if (g_chHead < 150.0f)
    {
        gfx_clip(LIST_X - 12.0f, 92.0f, SCREEN_W - LIST_X + 12.0f, GRID_CLIP_B - 92.0f);
        thumb_draw_banner(g_channel.banner, LIST_X, 96.0f + off, SCREEN_W - SAFE_X - LIST_X, 92.0f);
        gfx_circle(LIST_X + 12.0f, 142.0f + off, 112.0f, COL_BG);
        thumb_draw_avatar(g_channel.avatar, LIST_X + 16.0f, 146.0f + off, 104.0f, g_channel.title);

        /* nombre, @canal, suscriptores, videos, y la primera linea de la descripcion */
        x = LIST_X + 140.0f;
        fitw(g_channel.title, 46, 1.3f, line, sizeof(line));
        gfx_font_bold(1);
        gfx_text(x, 196.0f + off, 1.3f, COL_TEXT, line);
        gfx_font_bold(0);

        buf[0] = 0;
        if (g_channel.handle[0])
            strcat(buf, g_channel.handle);
        if (g_channel.subs[0])
        {
            if (buf[0])
                strcat(buf, DOT);
            strcat(buf, g_channel.subs);
        }
        if (g_channel.videosText[0])
        {
            if (buf[0])
                strcat(buf, DOT);
            strcat(buf, g_channel.videosText);
        }
        fitw(buf, 74, 0.9f, line, sizeof(line));
        gfx_text(x, 232.0f + off, 0.9f, COL_DIM, line);

        for (i = 0; g_channel.description[i] && g_channel.description[i] != '\n' && i < (int)sizeof(buf) - 1; ++i)
            buf[i] = g_channel.description[i];
        buf[i] = 0;
        fitw(buf, 80, 0.9f, line, sizeof(line));
        gfx_text(x, 256.0f + off, 0.9f, COL_SOFT, line);

        /* boton Suscribirse, a la derecha */
        if (channel_can_sub())
        {
            float bx = SCREEN_W - SAFE_X - UI_SUB_W - 8.0f, by = 214.0f + off;
            int ss;
            cuenta_sub_request(g_channel.id);
            ss = cuenta_sub_state(g_channel.id);
            ui_draw_sub_pill(bx, by, ss == SUB_YES, g_chSubFocus,
                             !cuenta_signed_in() || ss != SUB_UNKNOWN);
            if (g_chSubFocus && !cuenta_signed_in())
                g_chSubMsg = S_SUB_NEED, g_chSubMsgAt = GetTickCount();
            if (g_chSubMsg && GetTickCount() - g_chSubMsgAt < 3000)
            {
                const char *m = T(g_chSubMsg);
                if (strstr(m, "%s"))
                {
                    _snprintf(buf, sizeof(buf) - 1, m, g_channel.title);
                    buf[sizeof(buf) - 1] = 0;
                    m = buf;
                }
                fitw(m, 60, 0.75f, line, sizeof(line));
                gfx_text(SCREEN_W - SAFE_X - 8.0f - gfx_text_width(0.75f, line), by + UI_SUB_H + 12.0f, 0.75f,
                         COL_SOFT, line);
            }
        }
        gfx_clip_off();
    }

    if (g_channel.videoCount == 0)
        gfx_text(LIST_X + 24.0f, CH_GRID_Y0 + 40.0f, 1.0f, COL_SOFT, T(S_CHANNEL_EMPTY));
    else
    {
        g_gridHideSel = g_chSubFocus;
        draw_grid(g_channel.videos, g_channel.videoCount, g_chSel, &g_chGrid, CH_GRID_Y0 + off, 0);
        g_gridHideSel = 0;
    }

    if (g_chSubFocus)
        draw_footer(hints("A=%s|UD=%s|B=%s", T(cuenta_sub_state(g_channel.id) == SUB_YES ? S_SUBSCRIBED : S_SUBSCRIBE),
                          T(H_TO_LIST), T(H_BACK)));
    else
        draw_footer(hints("A=%s|BACK=%s|B=%s", T(H_WATCH), T(H_MENU), T(H_BACK)));
}

static void draw_analysis(void);   /* definida mas abajo */

/* Filas de la pista de audio y los subtitulos (5d) */
static int focus_selectable(int focus)
{
    if (focus == FOCUS_AUDIO)
        return g_player.trackCount > 1;
    if (focus == FOCUS_SUBS)
        return g_player.captionCount > 0;
    return 1;
}

static void caption_name(int cap, char *out, int size)
{
    if (cap < 0 || cap >= g_player.captionCount)
        _snprintf(out, size - 1, g_player.captionCount > 0 ? "Apagados"
                  : "No hay en espa\xC3\xB1ol, ingl\xC3\xA9s ni portugu\xC3\xA9s");
    else
        _snprintf(out, size - 1, "%s", g_player.captions[cap].name);
    out[size - 1] = 0;
}

/* "Audio:      « Español (US) original »   Izq/Der: cambiar (1 de 3)" */
static void draw_setting(int focus, float y, const char *label, const char *value, int pos, int count)
{
    char buf[256];
    char line[256];
    int on = g_focus == focus && focus_selectable(focus);
    float x;

    if (on)
    {
        gfx_rect(SAFE_X, y - 3.0f, SCREEN_W - 2.0f * SAFE_X, FMT_ROW_H, COL_SEL);
        gfx_rect(SAFE_X, y - 3.0f, 6.0f, FMT_ROW_H, COL_ACCENT);
    }
    gfx_text(SAFE_X + 24.0f, y, 1.0f, COL_DIM, label);
    x = SAFE_X + 24.0f + 13.0f * 12.0f;   /* los valores alineados */
    if (focus_selectable(focus))
    {
        _snprintf(buf, sizeof(buf) - 1, "\xC2\xAB %s \xC2\xBB", value);
        buf[sizeof(buf) - 1] = 0;
        fitw(buf, 52, 1.0f, line, sizeof(line));
        x = gfx_text(x, y, 1.0f, on ? COL_TEXT : COL_SOFT, line);
        if (on)
        {
            _snprintf(buf, sizeof(buf) - 1, "   Izq/Der: cambiar (%d de %d)", pos, count);
            buf[sizeof(buf) - 1] = 0;
            gfx_text(x, y, 1.0f, COL_DIM, buf);
        }
        else if (g_focus == FOCUS_FORMATS)
            gfx_text(x, y, 1.0f, COL_DIM, "   (cruceta arriba para cambiar)");
    }
    else
    {
        fitw(value, 60, 1.0f, line, sizeof(line));
        gfx_text(x, y, 1.0f, COL_DIM, line);
    }
}

static void draw_settings(void)
{
    char name[128];

    media_track_name(&g_player, g_trackSel, name, sizeof(name));
    draw_setting(FOCUS_AUDIO, AUDIO_Y, "Audio:", name, g_trackSel + 1, g_player.trackCount);
    caption_name(g_capSel, name, sizeof(name));
    draw_setting(FOCUS_SUBS, SUBS_Y, "Subt\xC3\xADtulos:", name, g_capSel + 2, g_player.captionCount + 1);
}

/* Cruceta en la pantalla del video: arriba/abajo pasa entre las filas de
 * audio y subtitulos y la tabla; izq/der cambia la fila elegida. */
static void video_nav(int move, WORD pressed)
{
    int f;

    if (move == -1 && g_focus == FOCUS_FORMATS && g_fmtSel == 0)
    {
        /* subir de la tabla a la fila de arriba que se pueda elegir */
        for (f = FOCUS_SUBS; f >= FOCUS_AUDIO; --f)
            if (focus_selectable(f))
            {
                g_focus = f;
                break;
            }
    }
    else if (move == -1 && g_focus == FOCUS_SUBS)
    {
        if (focus_selectable(FOCUS_AUDIO))
            g_focus = FOCUS_AUDIO;
    }
    else if (move == 1 && g_focus != FOCUS_FORMATS)
    {
        g_focus = (g_focus == FOCUS_AUDIO && focus_selectable(FOCUS_SUBS)) ? FOCUS_SUBS : FOCUS_FORMATS;
    }
    else if (g_focus == FOCUS_FORMATS)
        apply_move(&g_fmtSel, &g_fmtTop, move, g_fmtCount, FMT_ROWS);

    if (g_focus == FOCUS_AUDIO && g_player.trackCount > 1)
    {
        if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)
            g_trackSel = (g_trackSel + 1) % g_player.trackCount;
        if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)
            g_trackSel = (g_trackSel + g_player.trackCount - 1) % g_player.trackCount;
        /* si no se han tocado los subtitulos, siguen la regla del audio */
        if ((pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) && !g_capTouched)
            g_capSel = media_default_caption(&g_player, g_trackSel);
    }
    if (g_focus == FOCUS_SUBS && g_player.captionCount > 0)
    {
        /* -1 (apagados), 0, 1, 2 */
        if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)
            g_capSel = g_capSel + 1 >= g_player.captionCount ? -1 : g_capSel + 1;
        if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)
            g_capSel = g_capSel <= -1 ? g_player.captionCount - 1 : g_capSel - 1;
        if (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))
            g_capTouched = 1;
    }
}

/* Video abierto con A mientras llegan sus formatos: miniatura grande,
 * titulo y "Cargando..." (o el error) */
static void draw_loading(void)
{
    const YtVideo *v = &g_curVideo;
    char line[512];
    char buf[512];
    const char *msg;
    int dots = (int)((GetTickCount() / 400) % 4), err = 0;
    float tw = 576.0f, th = 324.0f, tx = (SCREEN_W - tw) * 0.5f, ty = 128.0f;

    thumb_draw(v->id, tx, ty, tw, th);
    if (v->length[0])
        draw_badge(tx + tw, ty + th, v->length);

    gfx_fit_text(v->title, 1100.0f, 1.25f, line, sizeof(line));
    gfx_text((SCREEN_W - gfx_text_width(1.25f, line)) * 0.5f, 470.0f, 1.25f, COL_TEXT, line);
    join_meta(v, buf, sizeof(buf), 0);
    gfx_fit_text(buf, 1100.0f, 1.0f, line, sizeof(line));
    gfx_text((SCREEN_W - gfx_text_width(1.0f, line)) * 0.5f, 512.0f, 1.0f, COL_DIM, line);

    if (is_busy(JOB_PLAYER) || g_autoStart)
    {
        _snprintf(buf, sizeof(buf) - 1, "%s%.*s", T(S_LOADING), dots, "...");
        msg = buf;
    }
    else if (!g_playerOk)
    {
        msg = g_videoStatus[0] ? g_videoStatus : T(S_LOAD_FAIL);
        err = 1;
    }
    else if (g_fmtCount == 0)
    {
        msg = T(S_NO_QUALITY);
        err = 1;
    }
    else
    {
        _snprintf(buf, sizeof(buf) - 1, "%s%.*s", T(S_LOADING), dots, "...");
        msg = buf;
    }
    buf[sizeof(buf) - 1] = 0;
    gfx_fit_text(msg, 1100.0f, 1.0f, line, sizeof(line));
    gfx_text((SCREEN_W - gfx_text_width(1.0f, line)) * 0.5f, 560.0f, 1.0f, err ? COL_ACCENT : COL_SOFT, line);
    draw_footer(hints("B=%s", T(err ? H_BACK : H_CANCEL)));
}

static void draw_video(void)
{
    const YtVideo *v = &g_curVideo;
    char line[512];
    char buf[512];
    char dur[24];
    int row, i, dots, hls, def;
    float y;
    const YtFormat *f;

    if (g_autoPlay)
    {
        draw_loading();
        return;
    }
    dots = (int)((GetTickCount() / 400) % 4);

    /* titulo y canal */
    fitw(v->title, LINE_CHARS, 1.0f, line, sizeof(line));
    gfx_text(SAFE_X, 116.0f, 1.0f, COL_TEXT, line);
    join_meta(v, buf, sizeof(buf), 1);
    fitw(buf, LINE_CHARS, 1.0f, line, sizeof(line));
    gfx_text(SAFE_X, 146.0f, 1.0f, COL_DIM, line);

    /* estado de la consulta */
    if (is_busy(JOB_PLAYER))
    {
        _snprintf(buf, sizeof(buf) - 1, "Consultando formatos (cliente VISIONOS)%.*s", dots, "...");
        buf[sizeof(buf) - 1] = 0;
        gfx_text(SAFE_X, 186.0f, 1.0f, COL_SOFT, buf);
        draw_footer(hints("B=%s", T(H_BACK)));
        return;
    }
    if (!g_playerOk)
    {
        fitw(g_videoStatus[0] ? g_videoStatus : "Sin datos", LINE_CHARS, 1.0f, line, sizeof(line));
        gfx_text(SAFE_X, 186.0f, 1.0f, COL_ACCENT, line);
        draw_footer(hints("B=%s", T(H_BACK)));
        return;
    }

    hls = g_player.hlsUrl[0] != 0;
    format_duration(g_player.lengthSeconds, dur, sizeof(dur));
    _snprintf(buf, sizeof(buf) - 1,
              "OK" DOT "%s" DOT "%d compatibles (de %d)" DOT "HLS: %s" DOT "URLs v\xC3\xA1" "lidas %d h",
              dur, g_fmtCount, g_player.formatCount, hls ? "s\xC3\xAD" : "no", g_player.expiresIn / 3600);
    buf[sizeof(buf) - 1] = 0;
    gfx_text(SAFE_X, 186.0f, 1.0f, COL_OK, buf);

    /* pista de audio y subtitulos */
    draw_settings();

    /* tabla de formatos */
    _snprintf(buf, sizeof(buf) - 1, FMT_HEADER, "itag", "tipo", "codec", "detalle", "kbps", "MB");
    buf[sizeof(buf) - 1] = 0;
    gfx_text_mono(SAFE_X + 24.0f, FMT_HEAD_Y, 1.0f, COL_DIM, buf);

    if (g_fmtCount == 0)
        gfx_text(SAFE_X + 24.0f, FMT_Y, 1.0f, COL_ACCENT,
                 "Este video no tiene formatos H.264 que la consola pueda reproducir");
    def = default_quality();   /* la predeterminada (480p o menos) va en rojo */
    for (row = 0; row < FMT_ROWS; ++row)
    {
        i = g_fmtTop + row;
        if (i >= g_fmtCount)
            break;
        f = &g_player.formats[g_fmtList[i]];
        y = FMT_Y + row * FMT_ROW_H;

        /* el elegido siempre lleva la marca roja (es el que se ve con
         * Start); el fondo solo cuando la tabla tiene la seleccion */
        if (i == g_fmtSel)
        {
            if (g_focus == FOCUS_FORMATS)
                gfx_rect(SAFE_X, y - 3.0f, SCREEN_W - 2.0f * SAFE_X, FMT_ROW_H, COL_SEL);
            gfx_rect(SAFE_X, y - 3.0f, 6.0f, FMT_ROW_H, COL_ACCENT);
        }
        format_row(f, line, sizeof(line));
        gfx_text_mono(SAFE_X + 24.0f, y, 1.0f,
                      i == def ? COL_ACCENT : (i == g_fmtSel && g_focus == FOCUS_FORMATS) ? COL_TEXT : COL_SOFT, line);
    }

    /* resultado de la prueba */
    if (is_busy(JOB_PROBE) || is_busy(JOB_HLS))
    {
        _snprintf(buf, sizeof(buf) - 1, "Descargando prueba%.*s", dots, "...");
        buf[sizeof(buf) - 1] = 0;
        gfx_text(SAFE_X, 596.0f, 1.0f, COL_SOFT, buf);
    }
    else if (g_probeText[0])
    {
        fitw(g_probeText, LINE_CHARS, 1.0f, line, sizeof(line));
        gfx_text(SAFE_X, 596.0f, 1.0f, g_probeOk ? COL_OK : COL_ACCENT, line);
    }

    if (g_analysisShown)
    {
        draw_analysis();
        draw_footer("B=Cerrar an\xC3\xA1" "lisis");
    }
    else
    {
#if TB_BETA
        (void)hls;
        draw_footer(hints("START=%s|B=%s", T(H_WATCH), T(H_BACK)));
#else
        /* Back: modo de prueba del diagnostico de atascos (ver gpuvid.h) */
        _snprintf(buf, sizeof(buf) - 1, hls
                  ? "START=Ver|A=Probar|X=HLS|Y=Analizar|BACK=Prueba %d (%s)|B=Volver"
                  : "START=Ver|A=Probar|Y=Analizar|BACK=Prueba %d (%s)|B=Volver",
                  gv_test_mode(), gv_test_mode_name(gv_test_mode()));
        buf[sizeof(buf) - 1] = 0;
        draw_footer(buf);
#endif
    }
}

static void aot_name(int aot, char *out, int size)
{
    if (aot == 2)
        _snprintf(out, size - 1, "AAC LC");
    else if (aot == 5)
        _snprintf(out, size - 1, "HE-AAC");
    else if (aot == 29)
        _snprintf(out, size - 1, "HE-AAC v2");
    else
        _snprintf(out, size - 1, "AAC (tipo %d)", aot);
    out[size - 1] = 0;
}

/* Duracion total y promedio de los fragmentos segun el indice */
static void index_line(const FmpTrack *t, char *out, int size)
{
    double total = 0.0;
    char dur[24];
    int i;

    for (i = 0; i < t->segmentCount; ++i)
        total += t->segments[i].duration;
    if (t->sidxTimescale)
        total /= t->sidxTimescale;

    format_duration((int)(total + 0.5), dur, sizeof(dur));
    _snprintf(out, size - 1, "       \xC3\x8D" "ndice: %d fragmentos" DOT "%.1f s c/u" DOT "total %s",
              t->segmentCount, t->segmentCount ? total / t->segmentCount : 0.0, dur);
    out[size - 1] = 0;
}

static void panel_line(float *y, unsigned int color, const char *text)
{
    char line[512];

    fitw(text, LINE_CHARS - 2, 1.0f, line, sizeof(line));
    gfx_text(SAFE_X + 20.0f, *y, 1.0f, color, line);
    *y += 30.0f;
}

static void draw_analysis(void)
{
    const MediaStream *v = &g_probe.video;
    const MediaStream *a = &g_probe.audio;
    char buf[512];
    char nals[64];
    char aac[32];
    float y = 222.0f;
    int k, n, dots;
    double secs, mbit, need;

    gfx_rect(SAFE_X, 206.0f, SCREEN_W - 2.0f * SAFE_X, 414.0f, COL_PANEL);
    gfx_rect(SAFE_X, 206.0f, SCREEN_W - 2.0f * SAFE_X, 4.0f, COL_ACCENT);
    panel_line(&y, COL_TEXT, "An\xC3\xA1" "lisis de reproducci\xC3\xB3" "n (paso 4b)");
    y += 10.0f;

    if (is_busy(JOB_ANALYZE))
    {
        dots = (int)((GetTickCount() / 400) % 4);
        _snprintf(buf, sizeof(buf) - 1,
                  "Descargando encabezado, \xC3\xAD" "ndice y primer fragmento%.*s", dots, "...");
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, COL_SOFT, buf);
        return;
    }

    /* video */
    if (v->ok)
    {
        _snprintf(buf, sizeof(buf) - 1,
                  "Video  itag %d" DOT "H.264 %s %d.%d" DOT "%dx%d" DOT "NAL %d B" DOT "SPS %d B" DOT "PPS %d B",
                  v->itag, fmp_profile_name(v->track.profile), v->track.level / 10, v->track.level % 10,
                  v->track.width, v->track.height, v->track.nalLengthSize, v->track.spsLen, v->track.ppsLen);
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, COL_TEXT, buf);

        index_line(&v->track, buf, sizeof(buf));
        panel_line(&y, COL_SOFT, buf);

        n = 0;
        nals[0] = 0;
        for (k = 0; k < v->frag.nalCount && n < (int)sizeof(nals) - 4; ++k)
            n += _snprintf(nals + n, sizeof(nals) - 1 - n, " %d", v->frag.nalTypes[k]);
        _snprintf(buf, sizeof(buf) - 1,
                  "       Fragmento 1: %u cuadros" DOT "%.1f KB" DOT "%.2f s" DOT "%d clave" DOT "NAL:%s",
                  v->frag.sampleCount, v->frag.totalBytes / 1024.0, v->frag.durationSec,
                  v->frag.keyframes, nals);
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, COL_SOFT, buf);
    }
    else
    {
        _snprintf(buf, sizeof(buf) - 1, "Video  itag %d" DOT "Error: %s", v->itag, v->err);
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, COL_ACCENT, buf);
        y += 60.0f;
    }
    y += 10.0f;

    /* audio */
    if (a->ok)
    {
        aot_name(a->track.audioObjectType, aac, sizeof(aac));
        _snprintf(buf, sizeof(buf) - 1, "Audio  itag %d" DOT "%s" DOT "%d Hz" DOT "%d canales",
                  a->itag, aac, a->track.sampleRate, a->track.channels);
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, COL_TEXT, buf);

        index_line(&a->track, buf, sizeof(buf));
        panel_line(&y, COL_SOFT, buf);

        _snprintf(buf, sizeof(buf) - 1, "       Fragmento 1: %u bloques" DOT "%.1f KB" DOT "%.2f s",
                  a->frag.sampleCount, a->frag.totalBytes / 1024.0, a->frag.durationSec);
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, COL_SOFT, buf);
    }
    else
    {
        _snprintf(buf, sizeof(buf) - 1, "Audio  itag %d" DOT "Error: %s", a->itag, a->err);
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, COL_ACCENT, buf);
        y += 60.0f;
    }
    y += 10.0f;

    /* velocidad: ¿alcanza para reproducir sin cortes? */
    secs = g_analysisMs / 1000.0;
    mbit = secs > 0.0 ? g_probe.totalBytes * 8.0 / secs / 1000000.0 : 0.0;
    need = 0.0;
    if (v->fmtIndex >= 0) need += g_player.formats[v->fmtIndex].bitrate;
    if (a->fmtIndex >= 0) need += g_player.formats[a->fmtIndex].bitrate;
    need /= 1000000.0;

    if (g_probe.totalBytes > 0)
    {
        _snprintf(buf, sizeof(buf) - 1, "Descarga: %.0f KB en %.1f s (%.1f Mbit/s)" DOT "el video necesita %.1f Mbit/s",
                  g_probe.totalBytes / 1024.0, secs, mbit, need);
        buf[sizeof(buf) - 1] = 0;
        panel_line(&y, (mbit >= need * 1.5) ? COL_OK : COL_ACCENT, buf);
    }

    if (g_analysisOk)
        panel_line(&y, COL_OK, "Todo listo para el paso 4c: decodificar el video");
    else if (g_analysisErr[0])
        panel_line(&y, COL_ACCENT, g_analysisErr);
}

/* Cuadro de confirmacion para salir: oscurece la pantalla y pone dos
 * botones; gris el que no esta elegido y rojo el elegido. */
static void draw_exit_dialog(void)
{
    const char *labels[2];
    const char *title = T(S_EXIT_Q);
    const char *h = hints("A=%s|B=%s", T(H_ACCEPT), T(H_CANCEL));
    float bw = 580.0f, bh = 250.0f;
    float bx = (SCREEN_W - bw) * 0.5f, by = (SCREEN_H - bh) * 0.5f;
    float btnW = 190.0f, btnH = 56.0f, gap = 40.0f;
    float x0 = (SCREEN_W - (2.0f * btnW + gap)) * 0.5f, y0 = by + 104.0f;
    float x;
    int i;

    labels[0] = T(S_YES);
    labels[1] = T(S_NO);
    gfx_rect(0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0xB0000000);
    gfx_rect(bx, by, bw, bh, COL_BAR);
    /* encabezado con el rojo con brillo del menu de calidades */
    ui_red_gloss(bx, by, bw, 72.0f);
    gfx_text((SCREEN_W - gfx_text_width(1.25f, title)) * 0.5f, by + 21.0f, 1.25f, COL_WHITE, title);

    for (i = 0; i < 2; ++i)
    {
        x = x0 + i * (btnW + gap);
        if (i == g_exitSel)
            ui_red_gloss(x, y0, btnW, btnH);
        else
            gfx_rect(x, y0, btnW, btnH, g_lightTheme ? 0xFFD6D6D6 : 0xFF4A4A4A);
        gfx_text(x + (btnW - gfx_text_width(1.25f, labels[i])) * 0.5f, y0 + 13.0f, 1.25f,
                 i == g_exitSel ? COL_WHITE : COL_TEXT, labels[i]);
    }
    ui_hints((SCREEN_W - ui_hints_width(h)) * 0.5f, by + bh - 44.0f, h, COL_DIM);
}

/* ---- Cuadro de actualizaciones (mismo estilo que el de Salir) ---- */

/* botones que tiene el cuadro ahora; devuelve cuantos (1 o 2) */
static int update_buttons(const char **labels)
{
    int st = update_dl_state();

    if (st == UPD_DL_RUNNING)
    {
        labels[0] = T(H_CANCEL);
        return 1;
    }
    if (st == UPD_DL_DONE)
    {
        labels[0] = T(S_UPD_EXIT_NOW);
        labels[1] = T(S_UPD_LATER);
        return 2;
    }
    if (st == UPD_DL_FAIL)
    {
        labels[0] = T(S_UPD_RETRY);
        labels[1] = T(H_CANCEL);
        return 2;
    }
    if (update_can_download())
    {
        labels[0] = T(S_UPD_DOWNLOAD);
        labels[1] = T(H_CANCEL);
        return 2;
    }
    labels[0] = T(H_ACCEPT);
    return 1;
}

static float dlg_text(float x, float y, float w, float scale, unsigned int color, const char *s, float lineH, int center)
{
    char line[256];
    while (s && *s)
    {
        s = gfx_wrap_text(s, w, scale, line, sizeof(line));
        gfx_text(center ? x + (w - gfx_text_width(scale, line)) * 0.5f : x, y, scale, color, line);
        y += lineH;
    }
    return y;
}

static void draw_update_dialog(void)
{
    const char *labels[2];
    char title[128], buf[400];
    float bw = 780.0f, bh = 440.0f;
    float bx = (SCREEN_W - bw) * 0.5f, by = (SCREEN_H - bh) * 0.5f;
    float tx = bx + 40.0f, tw = bw - 80.0f, y = by + 96.0f;
    float btnW = 220.0f, btnH = 56.0f, gap = 40.0f, x0, yb = by + bh - 128.0f;
    int n = update_buttons(labels), st = update_dl_state(), i;
    const char *h = hints("A=%s|B=%s", T(H_ACCEPT), T(H_CANCEL));

    if (update_available())
        _snprintf(title, sizeof(title) - 1, T(S_UPD_NEW), update_version());
    else
        _snprintf(title, sizeof(title) - 1, "%s", T(S_UPD_TITLE));
    title[sizeof(title) - 1] = 0;

    gfx_rect(0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0xB0000000);
    gfx_rect(bx, by, bw, bh, COL_BAR);
    ui_red_gloss(bx, by, bw, 72.0f);
    gfx_icon(UI_ICON_UPDATE, bx + 24.0f, by + 18.0f, 36.0f, COL_WHITE);
    gfx_font_bold(1);
    gfx_fit_text(title, bw - 120.0f, 1.2f, buf, sizeof(buf));
    gfx_text(bx + 74.0f, by + 21.0f, 1.2f, COL_WHITE, buf);
    gfx_font_bold(0);

    if (!update_available())
    {
        if (update_checked() == 0)
            strcpy(buf, T(S_UPD_CHECKING));
        else if (update_checked() < 0)
            strcpy(buf, T(S_UPD_NOCHECK));
        else
            _snprintf(buf, sizeof(buf) - 1, T(S_UPD_LATEST), TB_VERSION);
        buf[sizeof(buf) - 1] = 0;
        dlg_text(tx, y + 50.0f, tw, 1.0f, COL_TEXT, buf, 32.0f, 1);
    }
    else if (st == UPD_DL_RUNNING)
    {
        double done = 0.0, total = 0.0;
        float frac;
        update_dl_progress(&done, &total);
        _snprintf(buf, sizeof(buf) - 1, T(S_UPD_DOWNLOADING), update_version());
        buf[sizeof(buf) - 1] = 0;
        dlg_text(tx, y + 20.0f, tw, 1.0f, COL_TEXT, buf, 32.0f, 1);
        frac = total > 0.0 ? (float)(done / total) : 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;
        ui_round_rect(tx, y + 80.0f, tw, 18.0f, 9.0f, g_lightTheme ? 0xFFD0D0D0 : 0xFF3A3A3A);
        if (total > 0.0)
            ui_round_rect(tx, y + 80.0f, tw * frac < 18.0f ? 18.0f : tw * frac, 18.0f, 9.0f, COL_ACCENT);
        else
        {
            float k = (float)((GetTickCount() / 8) % 1000) / 1000.0f;   /* sin tamano: barrita que corre */
            ui_round_rect(tx + (tw - 160.0f) * k, y + 80.0f, 160.0f, 18.0f, 9.0f, COL_ACCENT);
        }
        if (total > 0.0)
            _snprintf(buf, sizeof(buf) - 1, T(S_UPD_MB), done / 1048576.0, total / 1048576.0);
        else
            _snprintf(buf, sizeof(buf) - 1, T(S_UPD_MB1), done / 1048576.0);
        buf[sizeof(buf) - 1] = 0;
        dlg_text(tx, y + 112.0f, tw, 0.9f, COL_SOFT, buf, 28.0f, 1);
    }
    else if (st == UPD_DL_DONE)
    {
        gfx_font_bold(1);
        y = dlg_text(tx, y + 10.0f, tw, 1.0f, COL_OK, T(S_UPD_DONE), 32.0f, 1);
        gfx_font_bold(0);
        dlg_text(tx, y + 14.0f, tw, 0.9f, COL_TEXT, T(S_UPD_DONE_HOW), 28.0f, 1);
    }
    else
    {
        if (st == UPD_DL_FAIL)
        {
            _snprintf(buf, sizeof(buf) - 1, T(S_UPD_FAIL), update_dl_error());
            buf[sizeof(buf) - 1] = 0;
            y = dlg_text(tx, y, tw, 0.9f, COL_ACCENT, buf, 28.0f, 1) + 8.0f;
        }
        else if (update_notes()[0])
            y = dlg_text(tx, y, tw, 0.9f, COL_SOFT, update_notes(), 27.0f, 1) + 10.0f;
        if (update_can_download())
            dlg_text(tx, y, tw, 0.85f, COL_TEXT, T(S_UPD_HOWTO), 26.0f, 1);
        else if (update_url()[0])
        {
            _snprintf(buf, sizeof(buf) - 1, T(S_UPD_GET), update_url());
            buf[sizeof(buf) - 1] = 0;
            dlg_text(tx, y, tw, 0.9f, COL_TEXT, buf, 28.0f, 1);
        }
    }

    if (g_updSel >= n)
        g_updSel = n - 1;
    x0 = (SCREEN_W - (n * btnW + (n - 1) * gap)) * 0.5f;
    for (i = 0; i < n; ++i)
    {
        float x = x0 + i * (btnW + gap);
        if (i == g_updSel)
            ui_red_gloss(x, yb, btnW, btnH);
        else
            gfx_rect(x, yb, btnW, btnH, g_lightTheme ? 0xFFD6D6D6 : 0xFF4A4A4A);
        gfx_text(x + (btnW - gfx_text_width(1.1f, labels[i])) * 0.5f, yb + 15.0f, 1.1f,
                 i == g_updSel ? COL_WHITE : COL_TEXT, labels[i]);
    }
    ui_hints((SCREEN_W - ui_hints_width(h)) * 0.5f, by + bh - 48.0f, h, COL_DIM);
}

/* Pantalla de carga: el logo grande al centro y tres puntos rojos que
 * laten mientras llega el Inicio */
static void draw_splash(void)
{
    const char *msg;
    char buf[160];
    float h = 168.0f, w = gfx_logo_width(1, h), cx = SCREEN_W * 0.5f;
    float cy = 432.0f, rad = 24.0f;

    gfx_rect_vgrad(0.0f, 360.0f, (float)SCREEN_W, 360.0f, 0x00000000, g_lightTheme ? 0x10000000 : 0x40000000);
    if (w > 0.0f)
        gfx_logo_big(cx - w * 0.5f, 190.0f, h);
    else
    {
        gfx_font_bold(1);
        gfx_text(cx - gfx_text_width(2.0f, "TubeBox360") * 0.5f, 240.0f, 2.0f, COL_TEXT, "TubeBox360");
        gfx_font_bold(0);
    }

    /* ruedita de carga estilo Xbox 360 (dos arcos) */
    ui_draw_spinner(cx, cy, rad);

    if (g_status[0] && !g_jobBusy)
        msg = g_status;   /* error al arrancar */
    else if (is_busy(JOB_HOME))
        msg = T(S_LOADING_CATALOG);
    else
        msg = g_status[0] ? g_status : T(S_STARTING);
    gfx_fit_text(msg, 1000.0f, 1.0f, buf, sizeof(buf));
    gfx_text(cx - gfx_text_width(1.0f, buf) * 0.5f, 478.0f, 1.0f, COL_SOFT, buf);
}

/* Cuenta regresiva del siguiente video, encima del ultimo cuadro */
static void draw_up_next(void)
{
    const char *labels[2];
    const YtVideo *v = &g_nextVideo;
    char line[320];
    char buf[320];
    const char *rest;
    float tw = 480.0f, th = 270.0f, tx = (SCREEN_W - tw) * 0.5f, ty = 132.0f, y, x;
    float btnW = 250.0f, btnH = 54.0f, gap = 30.0f, frac;
    DWORD el = GetTickCount() - g_upNextStart;
    int left, i;

    labels[0] = T(S_PLAY_NOW);
    labels[1] = T(H_CANCEL);
    gfx_rect(0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0xD8000000);
    if (g_endState == END_SEARCH)
    {
        const char *m = T(S_FINDING_NEXT);
        gfx_text((SCREEN_W - gfx_text_width(1.25f, m)) * 0.5f, 330.0f, 1.25f, COL_WHITE, m);
        m = hints("B=%s", T(H_EXIT));
        ui_hints((SCREEN_W - ui_hints_width(m)) * 0.5f, 660.0f, m, 0xFF9A9A9A);
        return;
    }

    left = el >= UPNEXT_MS ? 0 : (int)((UPNEXT_MS - el + 999) / 1000);
    frac = el >= UPNEXT_MS ? 1.0f : (float)el / UPNEXT_MS;

    /* "Siguiente video en 5" */
    _snprintf(line, sizeof(line) - 1, T(S_NEXT_IN), left);
    line[sizeof(line) - 1] = 0;
    gfx_font_bold(1);
    gfx_text((SCREEN_W - gfx_text_width(1.58f, line)) * 0.5f, 64.0f, 1.58f, COL_WHITE, line);
    gfx_font_bold(0);

    /* miniatura con el boton de reproducir encima y la barra del tiempo */
    thumb_draw(v->id, tx, ty, tw, th);
    if (v->length[0])
        draw_badge(tx + tw, ty + th, v->length);
    gfx_icon(UI_ICON_ORB, SCREEN_W * 0.5f - 40.0f, ty + th * 0.5f - 40.0f, 80.0f, COL_WHITE);
    gfx_icon(UI_ICON_PLAY, SCREEN_W * 0.5f - 40.0f, ty + th * 0.5f - 40.0f, 80.0f, COL_WHITE);
    gfx_rect(tx, ty + th, tw, 6.0f, 0xFF555555);
    gfx_rect(tx, ty + th, tw * frac, 6.0f, COL_ACCENT);

    /* titulo (hasta 2 lineas) y canal */
    y = ty + th + 22.0f;
    rest = gfx_wrap_text(v->title, 900.0f, 1.25f, line, sizeof(line));
    gfx_text((SCREEN_W - gfx_text_width(1.25f, line)) * 0.5f, y, 1.25f, COL_WHITE, line);
    if (rest[0])
    {
        y += 34.0f;
        gfx_fit_text(rest, 900.0f, 1.25f, line, sizeof(line));
        gfx_text((SCREEN_W - gfx_text_width(1.25f, line)) * 0.5f, y, 1.25f, COL_WHITE, line);
    }
    y += 38.0f;
    join_meta(v, buf, sizeof(buf), 0);
    gfx_fit_text(buf, 900.0f, 1.0f, line, sizeof(line));
    gfx_text((SCREEN_W - gfx_text_width(1.0f, line)) * 0.5f, y, 1.0f, 0xFFAAAAAA, line);

    /* botones: gris, y rojo el elegido */
    y = 590.0f;
    for (i = 0; i < 2; ++i)
    {
        x = (SCREEN_W - (2.0f * btnW + gap)) * 0.5f + i * (btnW + gap);
        gfx_rect(x, y, btnW, btnH, i == g_upSel ? COL_ACCENT : 0xFF4A4A4A);
        gfx_text(x + (btnW - gfx_text_width(1.0f, labels[i])) * 0.5f, y + 14.0f, 1.0f, COL_WHITE, labels[i]);
    }
    {
        const char *h = hints("A=%s|LR=%s|B=%s", T(H_CHOOSE), T(H_CHANGE), T(H_CANCEL));
        ui_hints((SCREEN_W - ui_hints_width(h)) * 0.5f, 664.0f, h, 0xFF9A9A9A);
    }
}

/* Configuracion: perfil, tema, idioma, predeterminados (audio, subtitulos,
 * calidad), sonidos, avatar y el manual de controles. A la derecha, la
 * descripcion con el logo de DeadPIayers. */
enum { SET_PROFILE, SET_GOOGLE, SET_THEME, SET_LANG, SET_REGION, SET_AUDIO, SET_SUBS, SET_QUALITY, SET_SOUNDS,
       SET_AVATAR, SET_NETTEST, SET_MANUAL, SET_ROWS };
#define SET_FIRST_DEF SET_REGION   /* primer renglon bajo "Configuraciones predeterminadas" */
#define SET_Y0      136.0f
#define SET_STEP    39.0f      /* 12 renglones: el ultimo termina en 632 */
#define SET_ROW_H   37.0f
#define SET_HDR_H   30.0f     /* titulo "Configuraciones predeterminadas" */
#define SET_W       660.0f
#define SET_SCALE   0.92f
#define SET_INFO_X  (LIST_X + SET_W + 34.0f)

static int   g_manPage = 0;     /* pagina del manual de controles */
static void *g_dpTex = NULL;    /* logo de DeadPIayers */
static int   g_dpTried = 0;

static float set_row_y(int i)
{
    return SET_Y0 + i * SET_STEP + (i >= SET_FIRST_DEF ? SET_HDR_H : 0.0f);
}

static const char *pref_audio_name(int v)
{
    static const int n[PREF_AUDIO_COUNT] = { S_V_APPLANG, S_V_ORIGINAL, S_V_ES, S_V_EN, S_V_PT, S_V_JA, S_V_AR };
    return T(n[v]);
}

static const char *pref_subs_name(int v)
{
    static const int n[PREF_SUBS_COUNT] = { S_V_AUTO, S_V_OFF, S_V_ES, S_V_EN, S_V_PT, S_V_JA, S_V_AR };
    return T(n[v]);
}

/* A o Der = siguiente valor, Izq = anterior */
static void settings_change(int row, int dir)
{
    switch (row)
    {
    case SET_PROFILE:
        ui_profile_signin();
        return;
    case SET_GOOGLE:
        if (dir > 0)
            open_google();
        return;
    case SET_THEME:
        set_theme(!g_lightTheme);
        break;
    case SET_LANG:
        set_language((lang_get() + LANG_COUNT + dir) % LANG_COUNT);   /* es -> en -> pt */
        return;   /* set_language ya guarda */
    case SET_REGION:
        lang_set_region((lang_region() + REGION_COUNT + dir) % REGION_COUNT);
        config_save();
        g_homeCount = 0;   /* el Inicio guardado era de la otra region */
        if (g_section == UI_SEC_HOME && !g_jobBusy)
            start_home();
        return;
    case SET_AUDIO:
        g_prefAudio = (g_prefAudio + PREF_AUDIO_COUNT + dir) % PREF_AUDIO_COUNT;
        break;
    case SET_SUBS:
        g_prefSubs = (g_prefSubs + PREF_SUBS_COUNT + dir) % PREF_SUBS_COUNT;
        break;
    case SET_QUALITY:
        g_prefQuality = (g_prefQuality + QUALITY_COUNT + dir) % QUALITY_COUNT;
        break;
    case SET_SOUNDS:
        sfx_enable(!sfx_enabled());
        if (sfx_enabled())
            sfx_play(SFX_SELECT);   /* que se oiga que volvieron */
        break;
    case SET_AVATAR:
        g_avatarOn = !g_avatarOn;
        break;
    case SET_NETTEST:
        if (dir > 0 && !g_jobBusy)
        {
            g_netReport[0] = 0;
            start_job(JOB_NETTEST, "");
        }
        return;
    default:
        if (dir > 0)
        {
            g_screen = SCREEN_MANUAL;
            g_manPage = 0;
        }
        return;
    }
    config_save();
}

static void draw_settings_screen(void)
{
    static const int info[3] = { S_INFO1, S_INFO2, S_INFO3 };
    char line[256], fit[256], q[16], q2[96];
    const char *act;
    unsigned int ic = g_lightTheme ? 0xFF606060 : COL_WHITE;
    float y, iy, w;
    int i;

    gfx_font_bold(1);
    gfx_text(LIST_X, 96.0f, 1.25f, COL_TEXT, T(S_SEC_SETTINGS));
    gfx_font_bold(0);

    for (i = 0; i < SET_ROWS; ++i)
    {
        y = set_row_y(i);
        if (i == SET_FIRST_DEF)
        {
            gfx_font_bold(1);
            gfx_text(LIST_X + 6.0f, y - SET_HDR_H + 2.0f, 0.82f, COL_DIM, T(S_DEFAULTS_HDR));
            gfx_font_bold(0);
        }
        if (i == g_setSel)
        {
            gfx_rect(LIST_X, y, SET_W, SET_ROW_H, COL_SEL);
            gfx_rect(LIST_X, y, 6.0f, SET_ROW_H, COL_ACCENT);
        }
        act = T(H_CHANGE);
        iy = y + (SET_ROW_H - 32.0f) * 0.5f;
        switch (i)
        {
        case SET_PROFILE:
            ui_profile_draw(LIST_X + 24.0f, y + 3.0f, 34.0f);
            _snprintf(line, sizeof(line) - 1, T(S_PROFILE_X),
                      ui_profile_name()[0] ? ui_profile_name() : T(S_NO_SESSION));
            break;
        case SET_GOOGLE:
            if (cuenta_signed_in())
            {
                thumb_draw_avatar(cuenta_avatar(), LIST_X + 24.0f, y + 3.0f, 34.0f, cuenta_name());
                _snprintf(line, sizeof(line) - 1, T(S_GACCOUNT_X), cuenta_name());
                act = T(H_SIGNOUT);
            }
            else
            {
                gfx_icon(UI_ICON_M_USER, LIST_X + 25.0f, iy, 32.0f, ic);
                _snprintf(line, sizeof(line) - 1, T(S_GACCOUNT_X),
                          T(cuenta_state() == CUENTA_NOCLIENT ? S_G_NOCLIENT : S_NO_SESSION));
                act = cuenta_state() == CUENTA_NOCLIENT ? "" : T(H_SIGNIN);
            }
            break;
        case SET_THEME:
            gfx_icon(g_lightTheme ? UI_ICON_SUN : UI_ICON_MOON, LIST_X + 25.0f, iy, 32.0f, ic);
            _snprintf(line, sizeof(line) - 1, "%s", T(g_lightTheme ? S_THEME_LIGHT : S_THEME_DARK));
            break;
        case SET_LANG:
            gfx_icon(UI_ICON_GLOBE, LIST_X + 23.0f, y + 5.0f, 36.0f, COL_WHITE);
            _snprintf(line, sizeof(line) - 1, "%s", T(S_LANGUAGE));
            break;
        case SET_REGION:
            gfx_icon(UI_ICON_GLOBE, LIST_X + 23.0f, y + 3.0f, 34.0f, COL_WHITE);
            if (lang_region() == 0)
            {
                _snprintf(q2, sizeof(q2) - 1, T(S_REGION_AUTO), lang_region_name(0));
                q2[sizeof(q2) - 1] = 0;
                _snprintf(line, sizeof(line) - 1, T(S_REGION_X), q2);
            }
            else
                _snprintf(line, sizeof(line) - 1, T(S_REGION_X), lang_region_name(lang_region()));
            break;
        case SET_AUDIO:
            gfx_icon(UI_ICON_M_MUSIC, LIST_X + 25.0f, iy, 32.0f, ic);
            _snprintf(line, sizeof(line) - 1, T(S_DEF_AUDIO), pref_audio_name(g_prefAudio));
            break;
        case SET_SUBS:
            gfx_icon(UI_ICON_CC, LIST_X + 23.0f, y + 5.0f, 36.0f, COL_WHITE);
            _snprintf(line, sizeof(line) - 1, T(S_DEF_SUBS), pref_subs_name(g_prefSubs));
            break;
        case SET_QUALITY:
            gfx_icon(UI_ICON_HQ, LIST_X + 23.0f, y + 5.0f, 36.0f, COL_WHITE);
            _snprintf(q, sizeof(q) - 1, "%dp", kQualities[g_prefQuality]);
            q[sizeof(q) - 1] = 0;
            _snprintf(line, sizeof(line) - 1, T(S_DEF_QUALITY), q);
            break;
        case SET_SOUNDS:
            gfx_icon(UI_ICON_M_SOUND, LIST_X + 25.0f, iy, 32.0f, ic);
            _snprintf(line, sizeof(line) - 1, T(S_SOUNDS_X), T(sfx_enabled() ? S_ON_LC : S_OFF_LC2));
            break;
        case SET_AVATAR:
            gfx_icon(UI_ICON_M_USER, LIST_X + 25.0f, iy, 32.0f, ic);
            _snprintf(line, sizeof(line) - 1, T(S_AVATAR_X), T(g_avatarOn ? S_ON_LC : S_OFF_LC2));
            break;
        case SET_NETTEST:
            gfx_icon(UI_ICON_GLOBE, LIST_X + 25.0f, y + 5.0f, 32.0f, COL_WHITE);
            _snprintf(line, sizeof(line) - 1, "%s", T(is_busy(JOB_NETTEST) ? S_NETTEST_RUN : S_NETTEST));
            act = T(H_OPEN);
            break;
        default:
            gfx_icon(UI_ICON_M_GAMING, LIST_X + 25.0f, iy, 32.0f, ic);
            _snprintf(line, sizeof(line) - 1, "%s", T(S_CONTROLS));
            act = T(H_OPEN);
            break;
        }
        line[sizeof(line) - 1] = 0;
        w = SET_W - 100.0f - gfx_text_width(SET_SCALE, act) - 24.0f;
        gfx_fit_text(line, w, SET_SCALE, fit, sizeof(fit));
        gfx_text(LIST_X + 74.0f, y + (SET_ROW_H - 22.0f * SET_SCALE) * 0.5f - 2.0f, SET_SCALE, COL_TEXT, fit);
        gfx_text(LIST_X + SET_W - 18.0f - gfx_text_width(SET_SCALE, act), y + (SET_ROW_H - 22.0f * SET_SCALE) * 0.5f - 2.0f,
                 SET_SCALE, COL_DIM, act);
    }

    /* a la derecha: el reporte de la prueba de red (si se hizo) o el logo
     * de DeadPIayers con la descripcion */
    if (g_netReport[0] && !is_busy(JOB_NETTEST))
    {
        const char *p = g_netReport;
        w = SCREEN_W - SAFE_X - SET_INFO_X;
        y = SET_Y0;
        gfx_font_bold(1);
        gfx_text(SET_INFO_X, y, 0.9f, COL_TEXT, T(S_NETTEST));
        gfx_font_bold(0);
        y += 34.0f;
        while (*p)
        {
            const char *e = strchr(p, '\n');
            int len = e ? (int)(e - p) : (int)strlen(p);
            char one[300];
            const char *rest;
            if (len > (int)sizeof(one) - 1)
                len = sizeof(one) - 1;
            memcpy(one, p, len);
            one[len] = 0;
            rest = one;
            while (*rest)
            {
                rest = gfx_wrap_text(rest, w, 0.74f, line, sizeof(line));
                gfx_text(SET_INFO_X, y, 0.74f, strstr(one, "FALLO") ? COL_ACCENT : COL_SOFT, line);
                y += 21.0f;
            }
            y += 4.0f;
            p = e ? e + 1 : p + len;
        }
        {
            /* donde quedo el reporte para mandar */
            const char *rest;
            char saved[200];
            y += 6.0f;
            if (g_reportSaved > 0)
                _snprintf(saved, sizeof(saved) - 1, T(S_REPORT_SAVED), REPORT_PATH);
            else
                _snprintf(saved, sizeof(saved) - 1, "%s", T(S_REPORT_FAIL));
            saved[sizeof(saved) - 1] = 0;
            rest = saved;
            while (*rest)
            {
                rest = gfx_wrap_text(rest, w, 0.74f, line, sizeof(line));
                gfx_text(SET_INFO_X, y, 0.74f, g_reportSaved > 0 ? COL_TEXT : COL_ACCENT, line);
                y += 21.0f;
            }
        }
        draw_footer(hints("A=%s|UD=%s|BACK=%s|B=%s", T(H_CHANGE), T(H_CHOOSE), T(H_MENU), T(H_BACK)));
        return;
    }
    if (!g_dpTex && !g_dpTried)
    {
        long hr;
        g_dpTried = 1;
        g_dpTex = gfx_texture_load(g_dp_png, g_dp_png_len, g_dp_w, g_dp_h, &hr);
    }
    w = SCREEN_W - SAFE_X - SET_INFO_X;
    y = SET_Y0;
    if (g_dpTex)
    {
        gfx_image(g_dpTex, SET_INFO_X + (w - g_dp_w) * 0.5f, y, (float)g_dp_w, (float)g_dp_h, 0xFFFFFFFF);
        y += g_dp_h + 16.0f;
    }
    for (i = 0; i < 3; ++i)
    {
        const char *rest = T(info[i]);
        while (*rest)
        {
            rest = gfx_wrap_text(rest, w, 0.78f, line, sizeof(line));
            gfx_text(SET_INFO_X, y, 0.78f, i == 0 ? COL_TEXT : COL_SOFT, line);
            y += 22.0f;
        }
        y += 8.0f;
    }
    /* contacto y aviso de app no oficial */
    gfx_font_bold(1);
    gfx_text(SET_INFO_X, y, 0.78f, COL_TEXT, T(S_CONTACT));
    gfx_font_bold(0);
    y += 28.0f;
    {
        const char *rest = T(S_DISCLAIMER);
        while (*rest)
        {
            rest = gfx_wrap_text(rest, w, 0.72f, line, sizeof(line));
            gfx_text(SET_INFO_X, y, 0.72f, COL_DIM, line);
            y += 20.0f;
        }
    }
    gfx_text(SET_INFO_X, y + 4.0f, 0.74f, COL_DIM, "TubeBox360 v" TB_VERSION);
    if (update_available())
    {
        y += 34.0f;
        gfx_icon(UI_ICON_UPDATE, SET_INFO_X, y, 26.0f, COL_ACCENT);
        _snprintf(line, sizeof(line) - 1, T(S_UPD_NEW), update_version());
        line[sizeof(line) - 1] = 0;
        gfx_font_bold(1);
        gfx_text(SET_INFO_X + 34.0f, y + 1.0f, 0.78f, COL_ACCENT, line);
        gfx_font_bold(0);
        y += 30.0f;
        if (update_notes()[0])
        {
            const char *rest = update_notes();
            while (*rest && y < 610.0f)
            {
                rest = gfx_wrap_text(rest, w, 0.74f, line, sizeof(line));
                gfx_text(SET_INFO_X, y, 0.74f, COL_SOFT, line);
                y += 21.0f;
            }
        }
        if (update_url()[0] && y < 620.0f)
        {
            char u[256];
            _snprintf(u, sizeof(u) - 1, T(S_UPD_GET), update_url());
            u[sizeof(u) - 1] = 0;
            gfx_fit_text(u, w, 0.74f, line, sizeof(line));
            gfx_text(SET_INFO_X, y + 2.0f, 0.74f, COL_TEXT, line);
        }
    }
    draw_footer(hints("A=%s|UD=%s|BACK=%s|B=%s", T(H_CHANGE), T(H_CHOOSE), T(H_MENU), T(H_BACK)));
}

/* ---- Manual de controles: el mando con el nombre de cada boton ---- */

typedef struct { int pt, icon, label; } ManItem;

static const ManItem kManHome[] = {
    { UI_PADPT_LS, UI_ICON_PAD_LS, M_MOVE }, { UI_PADPT_DPAD, UI_ICON_PAD_DPAD, M_HOME_DPAD },
    { UI_PADPT_BACK, UI_ICON_PAD_BACK, M_OPEN_MENU },
    { UI_PADPT_RB, UI_ICON_PAD_RB, M_GO_CHANNEL }, { UI_PADPT_Y, UI_ICON_PAD_Y, M_SEARCH },
    { UI_PADPT_X, UI_ICON_PAD_X, M_RELOAD }, { UI_PADPT_A, UI_ICON_PAD_A, M_WATCH },
    { UI_PADPT_START, UI_ICON_PAD_START, M_EXIT }
};
static const ManItem kManKb[] = {
    { UI_PADPT_DPAD, UI_ICON_PAD_DPAD, M_KB_DPAD }, { UI_PADPT_Y, UI_ICON_PAD_Y, M_KB_Y },
    { UI_PADPT_X, UI_ICON_PAD_X, M_KB_X }, { UI_PADPT_B, UI_ICON_PAD_B, M_BACK },
    { UI_PADPT_A, UI_ICON_PAD_A, M_KB_A }, { UI_PADPT_START, UI_ICON_PAD_START, M_KB_START },
    { UI_PADPT_BACK, UI_ICON_PAD_BACK, M_KB_BACK }
};
static const ManItem kManPlayer[] = {
    { UI_PADPT_LB, UI_ICON_PAD_LB, M_PL_LB }, { UI_PADPT_DPAD, UI_ICON_PAD_DPAD, M_PL_DPAD },
    { UI_PADPT_RB, UI_ICON_PAD_RB, M_PL_RB }, { UI_PADPT_Y, UI_ICON_PAD_Y, M_PL_Y },
    { UI_PADPT_X, UI_ICON_PAD_X, M_PL_X }, { UI_PADPT_B, UI_ICON_PAD_B, M_PL_B },
    { UI_PADPT_A, UI_ICON_PAD_A, M_PL_A }
};
static const ManItem kManMenu[] = {
    { UI_PADPT_DPAD, UI_ICON_PAD_DPAD, M_MN_DPAD }, { UI_PADPT_X, UI_ICON_PAD_X, M_MN_X },
    { UI_PADPT_BACK, UI_ICON_PAD_BACK, M_MN_START },
    { UI_PADPT_START, UI_ICON_PAD_START, M_EXIT },
    { UI_PADPT_B, UI_ICON_PAD_B, M_MN_B }, { UI_PADPT_A, UI_ICON_PAD_A, M_MN_A },
    { UI_PADPT_RS, UI_ICON_PAD_RS, M_MN_RS }, { UI_PADPT_LT, UI_ICON_PAD_LT, M_MN_LT },
    { UI_PADPT_RT, UI_ICON_PAD_RT, M_MN_RT }
};

#define MAN_PAGES  4
#define MAN_K      1.0f     /* el dibujo ya viene al tamano en que se ve */
#define MAN_CX     660.0f     /* centro del dibujo del mando */
#define MAN_PAD_Y  232.0f     /* centrado entre las pestanas y las ayudas */
#define MAN_TEXT_W 214.0f
#define MAN_SCALE  0.9f
#define MAN_LINE_H 22.0f

static void *g_padTex = NULL;
static int   g_padTried = 0;

/* Dibuja la etiqueta de un boton (icono + texto en hasta 2 lineas) y
 * devuelve su alto. right = 1: alineada a la derecha terminando en x. */
static float man_label(const ManItem *it, float x, float y, int right, int draw, float *lineY)
{
    char l1[160], l2[160], full[200];
    const char *rest;
    float w1, w2, tw, icon = it->icon >= 0 ? 34.0f : 0.0f, h, tx;

    if (it->icon < 0)
        _snprintf(full, sizeof(full) - 1, "%s: %s", T(M_STICK), T(it->label));
    else
        _snprintf(full, sizeof(full) - 1, "%s", T(it->label));
    full[sizeof(full) - 1] = 0;
    rest = gfx_wrap_text(full, MAN_TEXT_W, MAN_SCALE, l1, sizeof(l1));
    l2[0] = 0;
    if (rest[0])
        gfx_fit_text(rest, MAN_TEXT_W, MAN_SCALE, l2, sizeof(l2));
    w1 = gfx_text_width(MAN_SCALE, l1);
    w2 = l2[0] ? gfx_text_width(MAN_SCALE, l2) : 0.0f;
    tw = w1 > w2 ? w1 : w2;
    h = l2[0] ? 2.0f * MAN_LINE_H + 4.0f : 30.0f;
    *lineY = y + h * 0.5f;
    if (!draw)
        return h;
    tx = right ? x - tw : x + icon;
    if (it->icon >= 0)
        gfx_icon(it->icon, right ? x - tw - 34.0f : x, *lineY - 14.0f, 28.0f, COL_WHITE);
    if (l2[0])
    {
        gfx_text(right ? x - w1 : tx, y, MAN_SCALE, COL_TEXT, l1);
        gfx_text(right ? x - w2 : tx, y + MAN_LINE_H + 2.0f, MAN_SCALE, COL_TEXT, l2);
    }
    else
        gfx_text(right ? x - w1 : tx, *lineY - 11.0f, MAN_SCALE, COL_TEXT, l1);
    return h;
}

/* ------------------------------------------------------------------ */
/* Cuenta de Google: iniciar / cerrar sesion                            */

static void open_google(void)
{
    g_screen = SCREEN_GOOGLE;
    g_menuFocus = 0;
    cuenta_clear_msg();
    if (cuenta_state() == CUENTA_OFF)
        cuenta_login_start();   /* de una vez pide el codigo */
}

static void google_input(WORD pressed)
{
    int st = cuenta_state();

    if (pressed & XINPUT_GAMEPAD_A)
    {
        if (st == CUENTA_ON)
        {
            cuenta_logout();
            g_subsCount = 0;   /* lo guardado era de esa cuenta */
        }
        else if (st == CUENTA_OFF)
            cuenta_login_start();
    }
    if (pressed & XINPUT_GAMEPAD_B)
    {
        if (st == CUENTA_ASKING || st == CUENTA_CODE)
            cuenta_login_cancel();
        cuenta_clear_msg();
        g_screen = SCREEN_SETTINGS;
    }
    if (pressed & XINPUT_GAMEPAD_BACK)
    {
        cuenta_clear_msg();
        open_menu(UI_SEC_SETTINGS);
    }
    if (pressed & XINPUT_GAMEPAD_START)
        open_exit();
}

/* Texto largo en varios renglones; devuelve la y de abajo */
static float text_block(float x, float y, float w, float scale, unsigned int color, const char *s, float lineH)
{
    char line[256];
    while (s && *s)
    {
        s = gfx_wrap_text(s, w, scale, line, sizeof(line));
        gfx_text(x, y, scale, color, line);
        y += lineH;
    }
    return y;
}

static void draw_google_screen(void)
{
    char code[32], url[128], buf[320], t[16];
    const char *msg = cuenta_msg();
    float w = SCREEN_W - SAFE_X - LIST_X, x = LIST_X, y = 150.0f, cw;
    int st = cuenta_state(), secs = 0;

    gfx_font_bold(1);
    gfx_text(LIST_X, 96.0f, 1.25f, COL_TEXT, T(S_G_TITLE));
    gfx_font_bold(0);

    switch (st)
    {
    case CUENTA_ON:
        gfx_text(x, y, 1.0f, COL_DIM, T(S_G_SIGNED_AS));
        y += 44.0f;
        thumb_draw_avatar(cuenta_avatar(), x, y, 96.0f, cuenta_name());
        gfx_font_bold(1);
        gfx_fit_text(cuenta_name(), w - 130.0f, 1.3f, buf, sizeof(buf));
        gfx_text(x + 120.0f, y + 30.0f, 1.3f, COL_TEXT, buf);
        gfx_font_bold(0);
        y += 130.0f;
        if (msg[0])
        {
            y = text_block(x, y, w, 0.95f, COL_SOFT, msg, 30.0f);
            y += 10.0f;
        }
        y = text_block(x, y, w, 0.95f, COL_TEXT, T(S_G_OUT_HINT), 30.0f);
        text_block(x, y + 24.0f, w * 0.8f, 0.8f, COL_DIM, T(S_G_NOTE), 25.0f);
        draw_footer(hints("A=%s|BACK=%s|B=%s", T(H_SIGNOUT), T(H_MENU), T(H_BACK)));
        break;

    case CUENTA_CODE:
        cuenta_code(code, sizeof(code), url, sizeof(url), &secs);
        gfx_text(x, y, 1.0f, COL_SOFT, T(S_G_STEP1));
        y += 40.0f;
        gfx_font_bold(1);
        gfx_text(x + 24.0f, y, 1.6f, COL_TEXT, url);
        gfx_font_bold(0);
        y += 70.0f;
        gfx_text(x, y, 1.0f, COL_SOFT, T(S_G_STEP2));
        y += 44.0f;
        gfx_font_bold(1);
        cw = gfx_text_width(2.2f, code);
        gfx_rect(x + 24.0f, y, cw + 64.0f, 84.0f, COL_SEL);
        gfx_rect(x + 24.0f, y, 6.0f, 84.0f, COL_ACCENT);
        gfx_text(x + 24.0f + 34.0f, y + 12.0f, 2.2f, COL_TEXT, code);
        gfx_font_bold(0);
        y += 112.0f;
        _snprintf(t, sizeof(t) - 1, "%d:%02d", secs / 60, secs % 60);
        t[sizeof(t) - 1] = 0;
        _snprintf(buf, sizeof(buf) - 1, T(S_G_WAIT), t);
        buf[sizeof(buf) - 1] = 0;
        ui_draw_spinner(x + 16.0f, y + 12.0f, 14.0f);
        y = text_block(x + 44.0f, y, w - 44.0f, 0.9f, COL_SOFT, buf, 28.0f);
        text_block(x, y + 22.0f, w * 0.8f, 0.8f, COL_DIM, T(S_G_NOTE), 25.0f);
        draw_footer(hints("BACK=%s|B=%s", T(H_MENU), T(H_CANCEL)));
        break;

    case CUENTA_ASKING:
    case CUENTA_FINISH:
        ui_draw_spinner(x + 22.0f, y + 14.0f, 20.0f);
        gfx_text(x + 58.0f, y, 1.0f, COL_SOFT, T(st == CUENTA_ASKING ? S_G_ASKING : S_G_FINISH));
        draw_footer(hints("BACK=%s|B=%s", T(H_MENU), T(st == CUENTA_ASKING ? H_CANCEL : H_BACK)));
        break;

    case CUENTA_NOCLIENT:
        _snprintf(buf, sizeof(buf) - 1, T(S_GACCOUNT_X), T(S_G_NOCLIENT));
        buf[sizeof(buf) - 1] = 0;
        text_block(x, y, w, 1.0f, COL_SOFT, buf, 32.0f);
        draw_footer(hints("BACK=%s|B=%s", T(H_MENU), T(H_BACK)));
        break;

    default:   /* CUENTA_OFF: se cancelo, vencio o fallo */
        if (msg[0])
            y = text_block(x, y, w, 1.0f, COL_ACCENT, msg, 32.0f) + 16.0f;
        text_block(x, y, w * 0.8f, 0.8f, COL_DIM, T(S_G_NOTE), 25.0f);
        draw_footer(hints("A=%s|BACK=%s|B=%s", T(H_SIGNIN), T(H_MENU), T(H_BACK)));
        break;
    }
}

static void draw_manual(void)
{
    static const int pageNames[MAN_PAGES] = { M_P_HOME, M_P_KB, M_P_PLAYER, M_P_MENU };
    const ManItem *items;
    int n, i, k, side, order[16], cnt, tmp;
    float x, w, padX, padW, padH, ax, ay, ly, nextY, h, lineY;
    long hr;

    gfx_font_bold(1);
    gfx_text(LIST_X, 96.0f, 1.25f, COL_TEXT, T(M_TITLE));
    gfx_font_bold(0);

    /* pestanas de las 4 pantallas */
    x = LIST_X;
    for (i = 0; i < MAN_PAGES; ++i)
    {
        w = gfx_text_width(0.95f, T(pageNames[i])) + 36.0f;
        if (i == g_manPage)
            ui_red_gloss(x, 140.0f, w, 38.0f);
        else
            gfx_rect(x, 140.0f, w, 38.0f, COL_CHIP);
        gfx_text(x + 18.0f, 148.0f, 0.95f, i == g_manPage ? COL_WHITE : COL_TEXT, T(pageNames[i]));
        x += w + 10.0f;
    }

    switch (g_manPage)
    {
    case 0:  items = kManHome;   n = sizeof(kManHome) / sizeof(kManHome[0]);     break;
    case 1:  items = kManKb;     n = sizeof(kManKb) / sizeof(kManKb[0]);         break;
    case 2:  items = kManPlayer; n = sizeof(kManPlayer) / sizeof(kManPlayer[0]); break;
    default: items = kManMenu;   n = sizeof(kManMenu) / sizeof(kManMenu[0]);     break;
    }

    /* el mando al centro del contenido */
    if (!g_padTex && !g_padTried)
    {
        g_padTried = 1;
        g_padTex = gfx_texture_load(g_pad_png, g_pad_png_len, g_pad_w, g_pad_h, &hr);
    }
    padW = g_pad_w * MAN_K;
    padH = g_pad_h * MAN_K;
    padX = MAN_CX - padW * 0.5f;   /* un poco mas hacia el centro de la pantalla */
    if (g_padTex)
        gfx_image(g_padTex, padX, MAN_PAD_Y, padW, padH, 0xFFFFFFFF);

    /* nombres: los botones de la mitad izquierda del mando a la izquierda y
     * los demas a la derecha, de arriba a abajo sin encimarse; una linea va
     * del nombre al boton */
    for (side = 0; side < 2; ++side)
    {
        cnt = 0;
        for (i = 0; i < n && cnt < 16; ++i)
            if ((g_pad_points[items[i].pt].x < g_pad_w / 2) == (side == 0))
                order[cnt++] = i;
        for (i = 1; i < cnt; ++i)   /* ordenar por altura del boton */
            for (k = i; k > 0 && g_pad_points[items[order[k]].pt].y < g_pad_points[items[order[k - 1]].pt].y; --k)
            {
                tmp = order[k];
                order[k] = order[k - 1];
                order[k - 1] = tmp;
            }
        nextY = 190.0f;
        for (k = 0; k < cnt; ++k)
        {
            const ManItem *it = &items[order[k]];
            const UiPoint *p = &g_pad_points[it->pt];
            ax = padX + p->x * MAN_K;
            ay = MAN_PAD_Y + p->y * MAN_K;
            h = man_label(it, 0.0f, 0.0f, 0, 0, &lineY);
            ly = ay - h * 0.5f;
            if (ly < nextY)
                ly = nextY;
            if (side == 0)
            {
                man_label(it, padX - 30.0f, ly, 1, 1, &lineY);
                ui_line(padX - 22.0f, lineY, ax, ay, 2.0f, 0xFF9A9A9A);
            }
            else
            {
                man_label(it, padX + padW + 30.0f, ly, 0, 1, &lineY);
                ui_line(padX + padW + 22.0f, lineY, ax, ay, 2.0f, 0xFF9A9A9A);
            }
            gfx_circle(ax - 5.0f, ay - 5.0f, 10.0f, COL_ACCENT);
            nextY = ly + h + 12.0f;
        }
    }
    draw_footer(hints("LR+LB+RB=%s|B=%s", T(H_CHANGE_PAGE), T(H_BACK)));
}

static void draw_screen(int sel, int top)
{
    char info[160];
    char line[160];

    thumb_frame();

    if (g_screen == SCREEN_PLAY)
    {
        gfx_begin(0xFF000000);
        play_draw();
        if (g_endState != END_NONE)
            draw_up_next();
        draw_idle_dim();
        gfx_end();
        return;
    }

    gfx_begin(COL_BG);
    if (g_splash)
    {
        draw_splash();
        draw_idle_dim();
        gfx_end();
        return;
    }
    if (has_rail())
        ui_draw_rail(rail_current(), -1);   /* plegado; desplegado va encima de todo */
    draw_header();

    if (g_screen == SCREEN_VIDEO && g_videoIdx >= 0)
    {
        draw_video();
    }
    else if (g_screen == SCREEN_CHANNEL)
    {
        draw_channel();
    }
    else if (g_screen == SCREEN_SEARCH)
    {
        search_draw(LIST_X);
        draw_footer(hints("A=%s|X=%s|Y=%s|START=%s|BACK=%s|B=%s", T(H_TYPE), T(H_DELETE), T(H_SPACE),
                          T(H_SEARCH), T(H_MENU), T(H_BACK)));
    }
    else if (g_screen == SCREEN_SETTINGS)
    {
        draw_settings_screen();
    }
    else if (g_screen == SCREEN_MANUAL)
    {
        draw_manual();
    }
    else if (g_screen == SCREEN_GOOGLE)
    {
        draw_google_screen();
    }
    else
    {
        draw_list(sel, top);
        if (g_headerFocus == HDR_SEARCH)
            draw_footer(hints("A=%s|BACK=%s|UD=%s|START=%s", T(H_OPEN_KB), T(H_MENU), T(H_TO_LIST), T(H_EXIT)));
        else if (g_headerFocus == HDR_UPDATE)
            draw_footer(hints("A=%s|BACK=%s|UD=%s|START=%s", T(S_UPD_TITLE), T(H_MENU), T(H_TO_LIST), T(H_EXIT)));
        else
            draw_footer(hints("A=%s|RB=%s|Y=%s|X=%s|BACK=%s|START=%s", T(H_WATCH), T(H_CHANNEL),
                              T(H_SEARCH), T(H_RELOAD), T(H_MENU), T(H_EXIT)));
    }

    /* si la letra/iconos/logo o alguna miniatura fallaron, el motivo
     * (para el reporte de pruebas), debajo de las ayudas */
    if (has_rail())
    {
        if (gfx_assets_error()[0])
        {
            fitw(gfx_assets_error(), 60, 0.75f, line, sizeof(line));
            gfx_text(SCREEN_W - SAFE_X - gfx_text_width(0.75f, line), 688.0f, 0.75f, COL_ACCENT, line);
        }
        else if (thumb_status(info, sizeof(info)))
        {
            fitw(info, 60, 0.75f, line, sizeof(line));
            gfx_text(SCREEN_W - SAFE_X - gfx_text_width(0.75f, line), 688.0f, 0.75f, COL_DIM, line);
        }
    }
    if (has_rail() && g_menuFocus)
    {
        ui_draw_rail(rail_current(), g_menuSel);
        if (g_avatarOn)   /* Configuracion -> Avatar en el menu lateral */
            avatar_draw(AVATAR_CX, AVATAR_FLOOR, AVATAR_HGT);   /* el avatar del jugador, junto al menu */
        else
            ui_draw_active_users(AVATAR_CX, 300.0f);   /* sin avatar: los usuarios activos solos */
    }
    if (g_exitDlg)
        draw_exit_dialog();
    if (g_updDlg)
        draw_update_dialog();

    draw_idle_dim();
    gfx_end();
}

/* ------------------------------------------------------------------ */

/* autoplay: 1 = A (empieza solo en 480p), 0 = Start (pantalla de formatos).
 * idx = su lugar en la lista de donde viene (-1 si no esta en ninguna). */
static void open_video_from(const YtVideo *v, int from, int autoplay, int idx)
{
    g_curVideo = *v;
    g_curIdx = idx;
    g_endState = END_NONE;
    g_videoFrom = from;
    g_autoPlay = autoplay;
    g_autoStart = 0;
    g_videoIdx = 0;
    g_screen = SCREEN_VIDEO;
    g_playerOk = 0;
    g_trackSel = -1;
    g_capSel = -1;
    g_capTouched = 0;
    g_focus = FOCUS_FORMATS;
    g_videoStatus[0] = 0;
    g_probeText[0] = 0;
    g_fmtSel = 0;
    g_fmtTop = 0;
    g_analysisShown = 0;
    if (!start_job(JOB_PLAYER, g_curVideo.id))
        strcpy(g_videoStatus, T(S_WAIT_TASK));
}

/* Abre la pagina del canal (desde la lista) */
static void open_channel(const YtVideo *v)
{
    if (!v->channelId[0] || g_jobBusy)
        return;
    g_screen = SCREEN_CHANNEL;
    if (g_channelOk && strcmp(g_channel.id, v->channelId) == 0)
        return;   /* ya lo tenemos */
    g_channelOk = 0;
    g_channelStatus[0] = 0;
    g_chSel = g_chTop = 0;
    channel_view_reset();
    strncpy(g_chAvatarHint, v->avatar, sizeof(g_chAvatarHint) - 1);
    g_chAvatarHint[sizeof(g_chAvatarHint) - 1] = 0;
    start_job(JOB_CHANNEL, v->channelId);
}

/* Le pasa a play.c las pistas de audio para cambiarlas con Y (5d) */
static void set_play_tracks(void)
{
    const YtFormat *fmts[YT_MAX_TRACKS];
    char names[YT_MAX_TRACKS][96];
    const char *namePtrs[YT_MAX_TRACKS];
    int i, idx, n = 0, current = 0;

    for (i = 0; i < g_player.trackCount && n < YT_MAX_TRACKS; ++i)
    {
        idx = media_pick_audio(&g_player, i);
        if (idx < 0)
            continue;   /* esa pista no trae AAC */
        if (i == g_trackSel)
            current = n;
        fmts[n] = &g_player.formats[idx];
        media_track_name(&g_player, i, names[n], sizeof(names[n]));
        namePtrs[n] = names[n];
        g_playTracks[n] = i;
        ++n;
    }
    play_set_tracks(fmts, namePtrs, n, current);
}

/* Start en la pantalla de formatos: reproduce el formato elegido */
static void start_playback(void)
{
    const YtFormat *f;
    int a;

    f = selected_format();
    if (!g_playerOk || g_jobBusy || !f)
        return;
    if (!f->isVideo || strncmp(f->codecs, "avc1", 4) != 0 || f->indexEnd <= f->initStart)
    {
        g_probeOk = 0;
        strcpy(g_probeText, "Para ver, elige un formato de video H.264 (avc1)");
        return;
    }
    a = media_pick_audio(&g_player, g_trackSel);
    set_play_tracks();
    play_set_captions(g_player.captions, g_player.captionCount, g_capSel);
    play_set_info(&g_curVideo);
    {
        /* todas las calidades compatibles, para el boton HQ */
        const YtFormat *q[12];
        char names[12][24];
        const char *namePtrs[12];
        int i, n = g_fmtCount < 12 ? g_fmtCount : 12;

        for (i = 0; i < n; ++i)
        {
            q[i] = &g_player.formats[g_fmtList[i]];
            quality_name(q[i], names[i], sizeof(names[i]));
            namePtrs[i] = names[i];
        }
        play_set_qualities(q, namePtrs, n, g_fmtSel, default_quality());
    }
    hist_add(&g_curVideo);
    thumb_set_active(0);   /* sin descargas de miniaturas durante el video */
    play_start(f, a >= 0 ? &g_player.formats[a] : NULL, g_curVideo.title,
               (double)g_player.lengthSeconds);
    g_screen = SCREEN_PLAY;
}

static void stop_playback(int sel, int top)
{
    /* cerrar el decodificador puede tardar: avisa en pantalla */
    gfx_begin(0xFF000000);
    gfx_text((SCREEN_W - gfx_text_width(1.0f, T(S_STOPPING))) * 0.5f, 380.0f, 1.0f, COL_SOFT, T(S_STOPPING));
    ui_draw_spinner(SCREEN_W * 0.5f, 320.0f, 24.0f);
    gfx_end();
    play_set_wait_msg(T(S_STOPPING));
    play_stop();
    subs_clear();
    thumb_set_active(1);
    g_endState = END_NONE;
    if (play_current_caption() != g_capSel)
    {
        g_capSel = play_current_caption();   /* los que se eligieron durante el video */
        g_capTouched = 1;
    }
    if (g_player.trackCount > 1)
    {
        int k = play_current_track();
        if (k >= 0 && k < YT_MAX_TRACKS)
            g_trackSel = g_playTracks[k];   /* la que se eligio con Y */
    }
    /* con A se vuelve a la lista (o al canal); con Start, a los formatos */
    g_screen = g_autoPlay ? g_videoFrom : SCREEN_VIDEO;
    (void)sel;
    (void)top;
}

/* Siguiente de la lista de donde vino el video (0 si era el ultimo) */
static int next_in_list(void)
{
    if (g_curIdx < 0)
        return 0;
    if (g_videoFrom == SCREEN_LIST)
    {
        int k = g_curIdx + 1;
        while (k < g_count && g_videos[k].kind == YT_KIND_CHANNEL)
            ++k;   /* los canales de la busqueda no se "reproducen" */
        if (k < g_count)
        {
            g_nextVideo = g_videos[k];
            g_nextIdx = k;
            return 1;
        }
    }
    if (g_videoFrom == SCREEN_CHANNEL && g_channelOk && g_curIdx + 1 < g_channel.videoCount)
    {
        g_nextVideo = g_channel.videos[g_curIdx + 1];
        g_nextIdx = g_curIdx + 1;
        return 1;
    }
    return 0;
}

/* El video llego al final: cuenta regresiva (o primero buscar un
 * relacionado). Si no hay de donde, de regreso a la lista. */
static void start_up_next(int sel, int top)
{
    play_hide_overlay();
    thumb_set_active(1);   /* para que baje la miniatura del siguiente */
    g_upSel = 0;
    if (next_in_list())
    {
        g_endState = END_COUNT;
        g_upNextStart = GetTickCount();
    }
    else if (start_job(JOB_RELATED, g_curVideo.id))
        g_endState = END_SEARCH;
    else
        stop_playback(sel, top);
}

static void play_next(int sel, int top)
{
    YtVideo v = g_nextVideo;
    int from = g_videoFrom, idx = g_nextIdx;

    stop_playback(sel, top);
    if (idx >= 0 && from == SCREEN_LIST)
        g_listJump = idx;
    else if (idx >= 0 && from == SCREEN_CHANNEL)
    {
        g_chSel = idx;      /* la cuadricula pone la tarjeta a la vista sola */
    }
    open_video_from(&v, from, 1, idx);
}

/* Botones durante la cuenta regresiva */
static void up_next_input(WORD pressed, int stickDir, int sel, int top)
{
    if (pressed & XINPUT_GAMEPAD_B)
    {
        stop_playback(sel, top);
        return;
    }
    if (g_endState != END_COUNT)
        return;
    if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) || stickDir < 0)
        g_upSel = 0;
    if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || stickDir > 0)
        g_upSel = 1;
    if (pressed & XINPUT_GAMEPAD_A)
    {
        if (g_upSel == 0)
            play_next(sel, top);
        else
            stop_playback(sel, top);
    }
    else if (GetTickCount() - g_upNextStart >= UPNEXT_MS)
        play_next(sel, top);
}

/* Lo que se mira para decidir el sonido de la interfaz en cada vuelta */
typedef struct
{
    int screen, menu, menuSel, sel, chSel, setSel, manPage, header, exitDlg, exitSel;
} UiSnap;

static void ui_snap(UiSnap *u, int sel)
{
    u->screen = g_screen;
    u->menu = g_menuFocus;
    u->menuSel = g_menuSel;
    u->sel = sel;
    u->chSel = g_chSel;
    u->setSel = g_setSel;
    u->manPage = g_manPage;
    u->header = g_headerFocus;
    u->exitDlg = g_exitDlg;
    u->exitSel = g_exitSel;
}

/* Sonidos estilo NXE segun lo que cambio en esta vuelta. El teclado pone
 * los suyos (tecla mecanica, clic al buscar); en el reproductor no suena
 * nada para no tapar el video. */
static void ui_sounds(const UiSnap *b, WORD pressed, int sel)
{
    if (sfx_played() || g_splash || g_screen == SCREEN_PLAY)
        return;
    if (b->screen == SCREEN_PLAY)
    {
        if (g_screen != SCREEN_VIDEO)               /* (al VIDEO va el siguiente: sin sonido) */
            sfx_play(SFX_BACK);                     /* se cerro el video */
    }
    else if (!b->menu && g_menuFocus)
        sfx_play(SFX_MENU_OPEN);
    else if (b->menu && !g_menuFocus)
        sfx_play((pressed & XINPUT_GAMEPAD_A) ? SFX_SELECT : SFX_MENU_CLOSE);
    else if (b->exitDlg != g_exitDlg)
        sfx_play(g_exitDlg ? SFX_EXIT : SFX_BACK);
    else if (b->screen != g_screen)
        sfx_play((pressed & XINPUT_GAMEPAD_B) ? SFX_BACK : SFX_SELECT);
    else if (g_screen == SCREEN_SEARCH)
        return;                                     /* search.c ya sono lo suyo */
    else if (g_screen == SCREEN_LIST && !g_menuFocus && b->sel != sel && b->sel / GRID_COLS != sel / GRID_COLS)
        sfx_play(SFX_ROW);                          /* otra fila de la cuadricula */
    else if (g_screen == SCREEN_CHANNEL && !g_menuFocus && b->chSel != g_chSel &&
             b->chSel / GRID_COLS != g_chSel / GRID_COLS)
        sfx_play(SFX_ROW);                          /* otra fila en el canal */
    else if (b->menuSel != g_menuSel || b->sel != sel || b->chSel != g_chSel || b->setSel != g_setSel ||
             b->manPage != g_manPage || b->header != g_headerFocus || b->exitSel != g_exitSel)
        sfx_play(SFX_MOVE);
    else if (pressed & XINPUT_GAMEPAD_A)
        sfx_play(SFX_SELECT);
    else if (pressed & XINPUT_GAMEPAD_B)
        sfx_play(SFX_BACK);
}

void __cdecl main(void)
{
    int sel = 0;
    int top = 0;
    int move;
    WORD now;
    WORD prev = 0;
    WORD pressed;
    DWORD t;
    DWORD holdStart = 0;
    DWORD lastRepeat = 0;
    int prevStickX = 0;
    DWORD prevT = 0;
    DWORD holdStartX = 0, lastRepeatX = 0;
    int moveX = 0;
    UiSnap snap;

    if (!gfx_init())
    {
        OutputDebugStringA("[main] gfx_init fallo, revisa los mensajes [gfx]\n");
        for (;;)
            Sleep(1000);
    }

    /* Decodificador H.264 de la consola. Si arranca, la pantalla pasa al
     * modo Swap para siempre (Present se cruza con sus colas de GPU). */
    if (gv_init(gfx_device(), gfx_present_params()))
        gfx_set_swap_mode(1);
    au_init();
    av3d_init();    /* avatar 3D (XAvatar); si falla queda el busto 2.5D */
    sfx_init();     /* sonidos de la interfaz (sintetizados o game:\\sonidos\\*.wav) */
    set_theme(0);
    config_load();      /* tema guardado */
    report_init();      /* game:\errores.txt (para el reporte de los probadores) */
    g_splashStart = GetTickCount();

    strcpy(g_status, T(S_NET_INIT));
    draw_screen(sel, top);

    yt_init();      /* visitorData guardado */
    hist_load();    /* videos vistos, para "Porque viste..." */
    search_init();  /* busquedas recientes y el hilo de sugerencias */
    if (net_init())
    {
        g_status[0] = 0;
        thumb_init();
        start_home();
        presence_start();   /* "Usuarios activos" (si hay base en version.h) */
        update_start();     /* hay version nueva? (misma base) */
    }
    else
    {
        strcpy(g_status, T(S_NET_FAIL));
    }
    cuenta_init();          /* sesion de Google guardada (game:\\cuenta.ini) */
    XEnableScreenSaver(FALSE);   /* sin el oscurecido del sistema; la app usa el suyo (30 min) */
    g_lastInput = GetTickCount();

    for (;;)
    {
        ui_profile_poll();   /* foto de jugador del perfil con sesion */
        av3d_poll();         /* y su avatar 3D */
        poll_job();   /* al llegar una lista nueva pide volver a la primera fila (g_listJump) */
        /* fin de la pantalla de carga: ya llego el Inicio (o fallo) */
        if (g_splash && !g_jobBusy && GetTickCount() - g_splashStart > SPLASH_MIN_MS)
            g_splash = 0;
        if (g_listJump >= 0 && g_listJump < g_count)
        {
            /* se vio el siguiente de la lista: la lista sigue ahi */
            sel = g_listJump;
            if (sel < top)
                top = sel;
            if (sel >= top + VISIBLE_ROWS)
                top = sel - VISIBLE_ROWS + 1;
            g_listJump = -1;
        }
        if (g_autoStart)
        {
            /* llegaron los formatos de un video abierto con A: a verlo */
            g_autoStart = 0;
            if (g_screen == SCREEN_VIDEO && g_autoPlay && selected_format())
                start_playback();
        }

        now = read_buttons();
        pressed = (WORD)(now & ~prev);
        /* atenuacion por inactividad: lo que despierta la pantalla no hace
           nada mas (se ignora el mando hasta que se suelte todo) */
        if (g_dimmed && GetTickCount() - g_lastInput < 500)
        {
            g_dimmed = 0;
            g_wakeLock = 1;
        }
        else if (!g_dimmed && GetTickCount() - g_lastInput > IDLE_DIM_MS)
        {
            g_dimmed = 1;
            g_dimStart = GetTickCount();
        }
        if (g_wakeLock)
        {
            if (!now && !g_stickX && g_rx == 0.0f && g_ry == 0.0f && g_lt == 0.0f && g_rt == 0.0f)
                g_wakeLock = 0;
            pressed = 0;
        }
        prev = now;
        if (g_wakeLock)
            now = 0;
        t = GetTickCount();
        sfx_frame();
        ui_snap(&snap, sel);

        /* movimiento arriba/abajo con repeticion al mantener */
        move = 0;
        if (pressed & BTN_UP)
        {
            move = -1;
            holdStart = lastRepeat = t;
        }
        else if (pressed & BTN_DOWN)
        {
            move = 1;
            holdStart = lastRepeat = t;
        }
        else if ((now & (BTN_UP | BTN_DOWN)) &&
                 t - holdStart > REPEAT_DELAY && t - lastRepeat > REPEAT_RATE)
        {
            move = (now & BTN_UP) ? -1 : 1;
            lastRepeat = t;
        }

        /* izquierda/derecha (cruceta o palanca) con repeticion: la cuadricula */
        moveX = 0;
        if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) || (g_stickX < 0 && prevStickX >= 0))
        {
            moveX = -1;
            holdStartX = lastRepeatX = t;
        }
        else if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (g_stickX > 0 && prevStickX <= 0))
        {
            moveX = 1;
            holdStartX = lastRepeatX = t;
        }
        else if (((now & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) || g_stickX) &&
                 t - holdStartX > REPEAT_DELAY && t - lastRepeatX > REPEAT_RATE)
        {
            moveX = ((now & XINPUT_GAMEPAD_DPAD_LEFT) || g_stickX < 0) ? -1 : 1;
            lastRepeatX = t;
        }

        if (g_splash)
        {
            /* pantalla de carga: nada recibe los botones */
        }
        else if (g_exitDlg)
        {
            /* cuadro de "Salir": izq/der (cruceta o palanca) elige, A acepta,
             * B, Back o Start cancelan. Nada mas recibe los botones. */
            int dir = 0;
            if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) || (g_stickX < 0 && prevStickX >= 0))
                dir = -1;
            if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (g_stickX > 0 && prevStickX <= 0))
                dir = 1;
            if (dir < 0)
                g_exitSel = 0;
            else if (dir > 0)
                g_exitSel = 1;

            if ((pressed & XINPUT_GAMEPAD_A) && g_exitSel == 0)
                app_exit();
            if (((pressed & XINPUT_GAMEPAD_A) && g_exitSel == 1) ||
                (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)))
                g_exitDlg = 0;
        }
        else if (g_updDlg)
        {
            /* cuadro de actualizaciones: izq/der elige, A acepta, B cierra
             * (durante la descarga, B la cancela) */
            const char *lab[2];
            int nb = update_buttons(lab), st = update_dl_state();
            if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) || (g_stickX < 0 && prevStickX >= 0))
                g_updSel = 0;
            if (((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (g_stickX > 0 && prevStickX <= 0)) && nb > 1)
                g_updSel = 1;
            if (pressed & XINPUT_GAMEPAD_A)
            {
                if (st == UPD_DL_RUNNING)
                    update_download_cancel();
                else if (g_updSel == 0 && st == UPD_DL_DONE)
                    app_exit();
                else if (g_updSel == 0 && update_can_download())
                    update_download_start();   /* descargar o reintentar */
                else
                    g_updDlg = 0;
            }
            if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
            {
                if (st == UPD_DL_RUNNING)
                    update_download_cancel();
                else
                    g_updDlg = 0;
            }
        }
        else if (g_screen == SCREEN_PLAY)
        {
            int act;

            play_update();
            if (g_autoPlay && g_endState == END_NONE && play_ended())
                start_up_next(sel, top);
            if (g_endState != END_NONE)
            {
                up_next_input(pressed, (g_stickX != prevStickX) ? g_stickX : 0, sel, top);
                act = PLAY_ACT_NONE;
            }
            else
                act = play_input(pressed);   /* B lo maneja la capa de controles */
            if (act == PLAY_ACT_STOP)
                stop_playback(sel, top);
            else if (act == PLAY_ACT_CHANNEL)
            {
                stop_playback(sel, top);
                open_channel(&g_curVideo);
            }
        }
        else if (g_menuFocus && has_rail())
        {
            /* menu lateral desplegado; la palanca derecha y los gatillos
             * mueven al avatar */
            if (g_avatarOn)
                avatar_input(g_rx, g_ry, g_lt, g_rt, prevT ? t - prevT : 0);
            /* X = el gesto del elemento interactivo del avatar (mascota,
             * objeto...) */
            if (g_avatarOn && (pressed & XINPUT_GAMEPAD_X))
                avatar_gesture();
            menu_input(pressed, move);
            if (pressed & XINPUT_GAMEPAD_START)
                open_exit();
        }
        else if (g_screen == SCREEN_SEARCH)
        {
            int r = search_input(pressed, (g_stickX != prevStickX) ? g_stickX : 0);

            if (r == SEARCH_SUBMIT && !g_jobBusy)
            {
                g_section = UI_SEC_SEARCH;
                g_screen = SCREEN_LIST;
                g_count = 0;
                g_status[0] = 0;
                start_job(JOB_SEARCH, search_query());
            }
            else if (r == SEARCH_BACK)
                g_screen = SCREEN_LIST;
            else if (r == SEARCH_MENU)
                open_menu(UI_SEC_SEARCH);   /* Back en el teclado */
        }
        else if (g_screen == SCREEN_SETTINGS)
        {
            if (move)
                g_setSel = clamp_int(g_setSel + move, 0, SET_ROWS - 1);
            if (pressed & XINPUT_GAMEPAD_A)
                settings_change(g_setSel, 1);
            else if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (g_stickX > 0 && prevStickX <= 0))
            {
                if (g_setSel != SET_PROFILE && g_setSel != SET_MANUAL && g_setSel != SET_GOOGLE)
                    settings_change(g_setSel, 1);
            }
            else if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) || (g_stickX < 0 && prevStickX >= 0))
            {
                if (g_setSel != SET_PROFILE && g_setSel != SET_MANUAL && g_setSel != SET_GOOGLE)
                    settings_change(g_setSel, -1);
            }
            if (pressed & XINPUT_GAMEPAD_BACK)
                open_menu(UI_SEC_SETTINGS);
            if (pressed & XINPUT_GAMEPAD_START)
                open_exit();
            if (pressed & XINPUT_GAMEPAD_B)
                g_screen = SCREEN_LIST;
        }
        else if (g_screen == SCREEN_GOOGLE)
        {
            google_input(pressed);
        }
        else if (g_screen == SCREEN_MANUAL)
        {
            if ((pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_LEFT_SHOULDER)) && g_manPage > 0)
                --g_manPage;
            if ((pressed & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_SHOULDER)) && g_manPage < MAN_PAGES - 1)
                ++g_manPage;
            if (pressed & XINPUT_GAMEPAD_BACK)
                open_menu(UI_SEC_SETTINGS);
            if (pressed & XINPUT_GAMEPAD_START)
                open_exit();
            if (pressed & XINPUT_GAMEPAD_B)
                g_screen = SCREEN_SETTINGS;
        }
        else if (g_screen == SCREEN_CHANNEL)
        {
            if (pressed & XINPUT_GAMEPAD_BACK)
                open_menu(g_section);
            if (pressed & XINPUT_GAMEPAD_START)
                open_exit();
            if (g_chSubFocus)
            {
                /* boton Suscribirse: A cambia, abajo/B regresan a los videos */
                if (pressed & XINPUT_GAMEPAD_A)
                    channel_sub_press();
                else if ((pressed & (BTN_DOWN | XINPUT_GAMEPAD_B)) || move > 0)
                    g_chSubFocus = 0;
            }
            else
            {
                if (g_channelOk && !is_busy(JOB_CHANNEL))
                {
                    /* cuadricula como en Inicio: arriba/abajo filas, izq/der en la fila;
                     * arriba de la primera fila: el boton Suscribirse */
                    if (move < 0 && g_chSel < GRID_COLS && channel_can_sub())
                        g_chSubFocus = 1;
                    else if (move)
                        grid_move(&g_chSel, 0, move, g_channel.videoCount);
                    if (moveX)
                        grid_move(&g_chSel, moveX, 0, g_channel.videoCount);
                }
                if ((pressed & XINPUT_GAMEPAD_A) && g_channelOk && g_channel.videoCount > 0 && !g_jobBusy)
                    open_video_from(&g_channel.videos[g_chSel], SCREEN_CHANNEL, 1, g_chSel);
                if (pressed & XINPUT_GAMEPAD_B)
                    g_screen = SCREEN_LIST;   /* si seguia cargando, termina solo */
            }
        }
        else if (g_screen == SCREEN_LIST && g_headerFocus)
        {
            /* arriba: la barra de busqueda (A abre el teclado) o, a su
             * derecha, el icono de actualizaciones (A abre el cuadro) */
            if (moveX > 0 && g_headerFocus == HDR_SEARCH && update_enabled())
                g_headerFocus = HDR_UPDATE;
            else if (moveX < 0 && g_headerFocus == HDR_UPDATE)
                g_headerFocus = HDR_SEARCH;
            if ((pressed & XINPUT_GAMEPAD_A) && g_headerFocus == HDR_UPDATE)
            {
                g_updDlg = 1;
                g_updSel = 0;
            }
            else if (pressed & XINPUT_GAMEPAD_A)
                open_search();
            else if (pressed & XINPUT_GAMEPAD_BACK)
                open_menu(g_section);
            else if ((pressed & (BTN_DOWN | XINPUT_GAMEPAD_B)) || move > 0)
                g_headerFocus = HDR_NONE;
            if (pressed & XINPUT_GAMEPAD_Y)
                open_search();
            if ((pressed & XINPUT_GAMEPAD_X) && !g_jobBusy)
            {
                if (g_section == UI_SEC_SUBS)
                    show_subs(1);
                else if (g_section == UI_SEC_COMMUNITY)
                    show_community(1);
                else
                {
                    g_section = UI_SEC_HOME;
                    start_home();
                }
            }
            if (pressed & XINPUT_GAMEPAD_START)
                open_exit();
        }
        else if (g_screen == SCREEN_LIST)
        {
            /* cuadricula: arriba/abajo entre filas, izq/der dentro de la fila */
            if ((pressed & BTN_UP) && sel < GRID_COLS)
                g_headerFocus = HDR_SEARCH;   /* arriba de la primera fila: la barra de busqueda */
            else if (move)
                grid_move(&sel, 0, move, g_count);
            if (moveX)
                grid_move(&sel, moveX, 0, g_count);

            if ((pressed & XINPUT_GAMEPAD_A) && g_count == 0 && g_section == UI_SEC_SUBS &&
                cuenta_state() == CUENTA_OFF)
                open_google();   /* sin sesion: A lleva a iniciar sesion */
            if ((pressed & XINPUT_GAMEPAD_A) && g_count > 0 && !g_jobBusy)
            {
                if (g_videos[sel].kind == YT_KIND_CHANNEL)
                    open_channel(&g_videos[sel]);   /* un canal de la busqueda */
                else
                    open_video_from(&g_videos[sel], SCREEN_LIST, 1, sel);
            }
            if (pressed & XINPUT_GAMEPAD_BACK)
                open_menu(g_section);   /* Back despliega el menu lateral */
            if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && g_count > 0 && !g_jobBusy)
                open_channel(&g_videos[sel]);   /* RB: el canal del video */
            if (pressed & XINPUT_GAMEPAD_Y)
                open_search();
            if ((pressed & XINPUT_GAMEPAD_X) && !g_jobBusy)
            {
                if (g_section == UI_SEC_SUBS)
                    show_subs(1);
                else if (g_section == UI_SEC_COMMUNITY)
                    show_community(1);
                else
                {
                    g_section = UI_SEC_HOME;
                    start_home();
                }
            }
            if (pressed & XINPUT_GAMEPAD_START)
                open_exit();        /* Start: pregunta antes de salir */
        }
        else if (g_analysisShown)
        {
            if (pressed & XINPUT_GAMEPAD_B)
                g_analysisShown = 0;
        }
        else if (g_autoPlay)
        {
            /* cargando un video abierto con A: B cancela */
            if (pressed & XINPUT_GAMEPAD_B)
            {
                g_autoPlay = 0;
                g_autoStart = 0;
                g_screen = g_videoFrom;
            }
        }
        else
        {
            if (g_focus == FOCUS_FORMATS)
            {
                if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)  move = -FMT_ROWS;
                if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) move =  FMT_ROWS;
            }
            if (g_playerOk && !is_busy(JOB_PLAYER))
                video_nav(move, pressed);

#if !TB_BETA      /* herramientas de desarrollo: probar, HLS, analizar, modo de prueba */
            if ((pressed & XINPUT_GAMEPAD_A) && g_focus == FOCUS_FORMATS && g_playerOk && !g_jobBusy &&
                selected_format())
            {
                g_probeText[0] = 0;
                start_job(JOB_PROBE, selected_format()->url);
            }
            if ((pressed & XINPUT_GAMEPAD_X) && g_playerOk && !g_jobBusy && g_player.hlsUrl[0])
            {
                g_probeText[0] = 0;
                start_job(JOB_HLS, g_player.hlsUrl);
            }
            if ((pressed & XINPUT_GAMEPAD_Y) && g_playerOk && !g_jobBusy && selected_format())
            {
                media_free(&g_probe);
                g_analysisShown = 1;
                g_analysisOk = 0;
                g_analysisErr[0] = 0;
                g_jobInt = g_fmtList[g_fmtSel];
                start_job(JOB_ANALYZE, "");
            }
            if (pressed & XINPUT_GAMEPAD_BACK)
                gv_set_test_mode((gv_test_mode() + 1) % GV_TEST_MODES);
#endif
            if (pressed & XINPUT_GAMEPAD_START)
                start_playback();
            if (pressed & XINPUT_GAMEPAD_B)
                g_screen = g_videoFrom;   /* la lista o la pagina del canal */
        }

        ui_sounds(&snap, pressed, sel);
        prevStickX = g_stickX;
        prevT = t;
        g_listSel = sel;
        draw_screen(sel, top);
    }
}
