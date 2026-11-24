// gpuvid.cpp - Decodificador H.264 por GPU de la Xbox 360 (ver gpuvid.h).
//
// Credito: el decodificador de la consola no esta documentado. Que existe,
// como se inicializa (XAVInitialize extendido + colas de baja prioridad),
// los campos de su configuracion, el formato de los cuadros que entrega,
// el candado interno ("motor lock") y la forma segura de cerrarlo los
// descubrio y publico HG-ERIK en GPU-MEDIA-PLAYER-360 (GPL-3.0-or-later).
// Este archivo es codigo propio de TubeBox360 escrito con esos datos de
// interfaz; la logica de colas, tiempos y dibujo es nuestra.
//
// Hilos:
//   - hilo del alimentador (play.c): gv_feed() mete cuadros en una cola.
//   - hilos del decodificador: llaman read_cb() para sacar de esa cola.
//   - hilo principal: gv_update() saca cuadros listos y elige cual mostrar;
//     gv_draw() lo dibuja.

#include <xtl.h>
#include <xgraphics.h>
#include <d3dx9.h>
#include <xav.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "gpuvid.h"

// ---------------------------------------------------------------------------
// Funciones del decodificador (exportadas por xav.lib, sin encabezado publico)

struct XAVInitEx;
#ifndef GV_XAV_PUBLIC
XBOXAPI HRESULT WINAPI XAVInitialize(const XAVInitEx *pInit, DWORD ThreadAffinities[XAVMaxThreadTypes]);
#endif

extern "C"
{
    int     H264InitParamStructureEx(void *params, DWORD version, DWORD size);
    void   *H264DecodeCreateEx(void *params, void *lowPri);
    int     H264DecodeStart(void *dec, int flags);
    int     H264DecodeClose(void *dec);
    int     H264DecodeGetOutputInfo(void *dec, void *info, DWORD size);
    int     H264DecodeGetOutputGPU(void *dec, DWORD *frame);
    int     H264DecodeReturnOutputGPU(void *dec, DWORD handle, DWORD fence);
    HRESULT D3DDevice_ResetLowPriorityQueues(D3DDevice *dev, DWORD queues, DWORD entries, DWORD flags);
    VOID NTAPI ExRegisterTitleTerminateNotification(void *reg, BOOLEAN create);
}
long D3DDevice_InsertYieldPoint(D3DDevice *dev, unsigned long a, unsigned long b);

// ---------------------------------------------------------------------------
// Ajustes

// Prueba 4d: el decodificador llama read_cb() CON SU CANDADO TOMADO. Si
// read_cb() se queda esperando datos (porque nosotros frenamos la entrada),
// nadie puede sacar los cuadros ya decodificados ("Sin candado" sube) y la
// imagen se congela. Por eso aqui NO se frena por tiempo ni por cantidad:
// la cola de entrada siempre debe tener datos. El freno natural es la
// salida: si nadie toma cuadros, el decodificador deja de pedir datos solo.
#define QUEUE_CAP        96      // cola de entrada (~3 s a 30 fps)
// El decodificador tiene pocos cuadros de salida (8 o 12, ver gv_open). Cada
// cuadro que tenemos prestado (listos + en pantalla + esperando a la GPU)
// es uno menos para el. Si se queda sin ninguno, espera CON EL CANDADO
// TOMADO a que devolvamos uno, pero para devolverlo necesitamos el
// candado: se atora unos segundos (prueba 4d, atascos de ~6 s). Por eso
// pedimos pocos por adelantado.
#define READY_CAP        4       // cuadros decodificados esperando turno
// Para elegir el cuadro de cada refresco (60 Hz). El reloj del audio
// avanza a saltitos de ~5 ms; con margenes se evita tirar cuadros buenos.
#define DUE_AHEAD        0.010   // se muestra si le toca dentro de 10 ms
#define DROP_LATE        0.006   // se tira solo si el siguiente ya va 6 ms tarde
#define PEND_CAP         256     // marcas de tiempo pendientes
#define RET_CAP          32      // devoluciones pendientes
#define RETIRE_CAP       8       // cuadros dibujados esperando a la GPU
#define STAMP_SHIFT      100.0   // la marca de tiempo nunca es negativa
// Hilos de trabajo del decodificador. Pone uno en cada hilo HW, en orden
// (0, 1, 2...). Con 6, uno caia en el HW 5, donde corren nuestro hilo
// principal y el recolector con prioridad TIME_CRITICAL: ese hilo del
// decodificador se quedaba sin CPU y todo el decodificador lo esperaba
// (pausas de 0.6-0.9 s; diagnostico 4f, 2026-11-20). Con 5 usa HW 0-4 y el
// HW 5 queda solo para nosotros.
#define DEC_THREADS      5
#define DEC_OUT_FRAMES   8
#define DEC_OUT_FRAMES_BIG 12

// ---------------------------------------------------------------------------
// Estado

struct Frame
{
    DWORD  handle;
    DWORD  pitchY, heightY, pitchC, heightC;
    DWORD  y, u, v;
    DWORD  cropX, cropY, showW, showH;
    double pts;
};

struct InputAu
{
    BYTE    *data;
    int      len;
    __int64  stamp;
    int      key;
    GvAuInfo info;      // de donde viene (para el diagnostico)
};

struct Retiring
{
    Frame f;
    DWORD fence;
    DWORD tick;         // cuando dejo de estar en pantalla
};

// Registro para que el sistema nos avise al salir de la app
struct TermReg
{
    void      *routine;
    DWORD      priority;
    LIST_ENTRY list;
};

static IDirect3DDevice9 *g_dev;
static int               g_ok;
static char              g_status[128] = "Decodificador sin iniciar";
static CRITICAL_SECTION *g_motor;      // candado interno del decodificador
static TermReg           g_term;

static void             *g_dec;
static BYTE              g_params[0x10C];
static HANDLE            g_outEv;
static volatile LONG     g_closing;
static volatile LONG     g_feeding;
static volatile double   g_clock = -1.0;

// Entrada (protegida con g_qLock)
static CRITICAL_SECTION  g_qLock;
static HANDLE            g_dataEv;
static InputAu           g_queue[QUEUE_CAP];
static int               g_qHead, g_qCount;
static BYTE             *g_stage;           // el buffer que tiene el decodificador
static __int64           g_pend[PEND_CAP];  // marcas entregadas, ordenadas
static int               g_pendN;
static volatile LONG     g_fed, g_taken;

// Salida (solo hilo principal)
static Frame             g_ready[READY_CAP];
static int               g_readyN;
static Frame             g_cur;
static int               g_haveCur, g_curDrawn;
static DWORD             g_curFence;
static Retiring          g_retire[RETIRE_CAP];
static int               g_retireN;
static DWORD             g_ret[RET_CAP];
static int               g_retN;
static long              g_decoded, g_shown, g_dropped;
static long              g_takeOk, g_takeMiss;   // veces que conseguimos / no el candado
// Diagnostico de atascos
static volatile LONG     g_reads;          // cuadros que el decodificador ya leyo
static volatile LONG     g_readWaiting;    // 1 mientras read_cb espera datos
static DWORD             g_lastOutTick;    // ultima vez que salio un cuadro
static DWORD             g_lastKick;
static long              g_kicks;
static int               g_outFrames = DEC_OUT_FRAMES;   // salidas que acepto
static int               g_openLevel;
static int               g_level;          // nivel que pidio gv_open
// Reinicio automatico (ver restart_decoder)
static BYTE              g_hdr[512];       // SPS + PPS con sus codigos de inicio
static int               g_hdrLen;
static volatile LONG     g_needHdr;        // tras reiniciar: esperar cuadro clave
static long              g_restarts;
static volatile LONG     g_restarting;     // 1 mientras se reinicia
// Hilo que recoge los cuadros (4e)
static CRITICAL_SECTION  g_frameLock;      // protege g_ready/g_readyN y g_ret/g_retN
static HANDLE            g_takerThread;
static volatile LONG     g_takerStop;
static volatile LONG     g_lockWaitMax;    // espera mas larga por el candado (ms)
static long              g_lockWaitShown;
static DWORD             g_lockWaitTick;
static DWORD             g_restartTick;    // cuando fue el ultimo reinicio
static long              g_outSinceRestart;
static double            g_firstPts = -1.0;
// Modo de prueba (ver gv_set_test_mode)
static int               g_testMode;
static int               g_readyCap = READY_CAP;   // cuadros listos que se piden (<= READY_CAP)
static int               g_decThreads = DEC_THREADS;

// Dibujo
static IDirect3DVertexShader9      *g_vs;
static IDirect3DPixelShader9       *g_ps;
static IDirect3DVertexDeclaration9 *g_decl;
static D3DTexture                   g_tex[3];   // encabezados que apuntan al cuadro

static void set_status(const char *s)
{
    strncpy(g_status, s, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = 0;
    OutputDebugStringA("[gpuvid] ");
    OutputDebugStringA(s);
    OutputDebugStringA("\n");
}

static __int64 to_stamp(double pts)
{
    return (__int64)((pts + STAMP_SHIFT) * 10000000.0 + 0.5);   // unidades de 100 ns
}

static double from_stamp(__int64 s)
{
    return (double)s / 10000000.0 - STAMP_SHIFT;
}

// ---------------------------------------------------------------------------
// Diagnostico de atascos (4f, paso 1)
//
// Un "hueco" es el tiempo sin que salga ningun cuadro mientras nosotros si
// los esperamos (la fila de listos no esta llena) y el decodificador tiene
// datos (no esta pidiendo mas). A 30 fps sale uno cada 33 ms. Si el hueco
// llega a 300 ms se toma una foto del estado; si despues sale algo, fue una
// PAUSA que se recupero sola; si llega a los 2 s del vigilante, es un
// REINICIO. Todo se anota en game:\diagnostico.txt (carpeta del .xex).

#ifdef GV_SIM
#define DIAG_PATH      "diagnostico_sim.txt"
#else
#define DIAG_PATH      "game:\\diagnostico.txt"
#endif
#define DIAG_RING      12      // ultimos cuadros leidos que se recuerdan
#define DIAG_EVENTS    10      // eventos que se ven en el panel
#define GAP_ONSET_MS   300     // hueco que ya se nota: se toma la foto

struct AuRec
{
    GvAuInfo info;
    __int64  stamp;
    int      len;
    int      key;
    DWORD    tick;             // cuando lo leyo el decodificador
};

struct Snap                    // foto del estado al empezar un atasco
{
    DWORD    tick;             // cuando se tomo
    DWORD    gapStart;         // desde cuando no sale nada
    long     readsInst, outInst;
    int      readyN, haveCur, retireN, retN, outFrames;
    DWORD    retireAge;        // ms que lleva el mas viejo esperando a la GPU
    int      qCount;
    long     readWaiting;
    DWORD    sinceRead;
    long     backlog;
    long     lockWait;
    double   lastOutPts;
    double   clock;
    __int64  nextStamp;        // el cuadro que deberia salir (-1 si no se sabe)
    int      nextWhere;        // 1 ya lo leyo, 2 sigue en nuestra cola, 0 no se encontro
    AuRec    next;             // ese cuadro
    AuRec    ring[DIAG_RING];  // ultimos leidos, el mas viejo primero
    int      ringN;
};

struct DiagEvent
{
    char   type;               // 'R' reinicio, 'P' pausa, 'C' recarga
    double pts;
};

// Los escriben read_cb (con g_qLock) y el hilo recolector
static AuRec             g_ring[DIAG_RING];
static int               g_ringPos, g_ringN;
static volatile LONG     g_readsInst;      // leidos por esta instancia del decodificador
static volatile LONG     g_outInst;        // sacados de esta instancia
static volatile DWORD    g_lastReadTick;
static volatile double   g_lastOutPts = -1.0;
// Solo hilo principal
static DWORD             g_gapBase;        // los huecos cuentan desde aqui
static long              g_diagSeen;       // g_decoded la ultima vez que se vio
static DWORD             g_gapMax;         // hueco mas largo desde el ultimo cuadro
static int               g_episode;        // 1 si hay un atasco con foto tomada
static Snap              g_snap;
static char              g_snapThreads[640];   // estado de los hilos en la foto
static long              g_epBlMin, g_epBlMax;
static long              g_gapHist[6];     // 0.1-0.2 s, 0.2-0.3, 0.3-0.5, 0.5-1, 1-2, reinicio
static long              g_pauses, g_notes;
static long              g_nSamples, g_sumInside, g_maxInside, g_maxHeld, g_maxBacklog, g_sumBacklog;
static DiagEvent         g_events[DIAG_EVENTS];
static long              g_eventTotal;
static char              g_lastEvent[200];
static DWORD             g_sessionTick;
static int               g_threadsFallback;
static int               g_threadsDumped;  // ya se escribio la lista de hilos de este video
static void            (*g_diagHook)(char *, int);
static char              g_dbuf[14336];    // texto del evento que se esta armando
static int               g_dlen;
static char              g_noteBuf[2048];  // notas que se escriben despues (prueba 4)
static int               g_noteLen;
static DWORD             g_lastWriteMs;    // lo que tardo la ultima escritura del archivo

static long backlog_now(void);

// --- Que esta haciendo cada hilo (4f, paso 1b) ---
// Cada hilo anota su estado y desde cuando. Si en un atasco el recolector
// lleva 600 ms "esperando el candado", el decodificador lo tiene tomado;
// si read_cb lleva 600 ms "fuera", el decodificador no nos esta pidiendo.
enum { TS_TAKER, TS_READ, TS_FEED, TS_COUNT };
enum { TK_SLEEP, TK_WAITEV, TK_WAITLOCK, TK_RETURN, TK_INFO, TK_GET, TK_SETTLE, TK_QUEUE, TK_DONE };
enum { RD_OUT, RD_LOCK, RD_FREE, RD_HDR, RD_WAIT };
enum { FD_OUT, FD_MALLOC, FD_LOCK, FD_FULL };
static volatile LONG     g_ts[TS_COUNT];
static volatile DWORD    g_tsTick[TS_COUNT];
static volatile DWORD    g_lastNotifyTick;
static volatile DWORD    g_readThread, g_notifyThread;   // hilos del decodificador que nos llaman
static DWORD             g_mainLast, g_mainGapMax;        // ciclos del hilo principal (maximo del video)
static DWORD             g_mainGapCur, g_mainGapPrev, g_mainGapTick;   // maximo del segundo actual / anterior

static void ts_set(int who, LONG st)
{
    g_tsTick[who] = GetTickCount();
    g_ts[who] = st;
}

static const char *ts_name(int who, LONG st)
{
    static const char *tk[] = { "duerme (fila llena)", "espera aviso", "ESPERA EL CANDADO",
                                "devuelve cuadros", "pregunta si hay cuadro", "toma cuadro",
                                "acomoda marcas", "mete en la fila", "solto el candado" };
    static const char *rd[] = { "fuera (no nos pide)", "DENTRO esperando nuestra cola",
                                "DENTRO liberando memoria", "DENTRO armando SPS/PPS",
                                "DENTRO esperando datos" };
    static const char *fd[] = { "fuera", "pide memoria", "ESPERA LA COLA", "cola llena (espera)" };

    if (who == TS_TAKER && st >= 0 && st <= TK_DONE)
        return tk[st];
    if (who == TS_READ && st >= 0 && st <= RD_WAIT)
        return rd[st];
    if (who == TS_FEED && st >= 0 && st <= FD_FULL)
        return fd[st];
    return "?";
}

// --- Linea de tiempo: contadores de cada 0.1 s (los ultimos 6.4 s) ---
#define TL_N   64
#define HW_N   6
struct TlBucket
{
    volatile LONG period;           // GetTickCount() / 100 de este cubo
    volatile LONG reads, outs, notifies, locks, lockMax, updates, notes, writeMax;
    volatile LONG cpu[HW_N];        // espera mas larga de cada sonda en ese 0.1 s (ms)
};
static TlBucket          g_tl[TL_N];

static TlBucket *tl_now(void)
{
    LONG p = (LONG)(GetTickCount() / 100);
    TlBucket *b = &g_tl[p % TL_N];
    int i;

    if (b->period != p)
    {
        // cubo nuevo: lo limpia el primero que llega (si dos llegan a la
        // vez se puede perder una cuenta; para el diagnostico no importa)
        b->reads = b->outs = b->notifies = b->locks = b->lockMax = 0;
        b->updates = b->notes = b->writeMax = 0;
        for (i = 0; i < HW_N; ++i)
            b->cpu[i] = 0;
        b->period = p;
    }
    return b;
}

static void tl_max(volatile LONG *v, LONG x)
{
    if (x > *v)
        *v = x;
}

// --- Sondas de CPU: un hilo chiquito en cada hilo HW (0-5) ---
// Prioridad "bajo lo normal": si una sonda no corre en 600 ms, algo de
// prioridad normal o mas alta ocupo ese hilo HW todo ese tiempo.
static volatile DWORD    g_probeTick[HW_N];
static volatile LONG     g_probeMaxS[HW_N];    // espera mas larga en el video (ms)
static volatile LONG     g_probeOver[HW_N];    // veces que espero mas de 50 ms
static volatile LONG     g_probesOn;

// Nombres de nuestros hilos (para la lista de hilos)
struct NamedThread
{
    DWORD id;
    char  name[24];
    LONG  session;      // video en que se nombro (0 = para siempre: sondas)
};
#define NAMED_N 64
static NamedThread       g_named[NAMED_N];
static volatile LONG     g_namedN;
static volatile LONG     g_session = 1;   // sube en cada gv_close

static void name_thread(const char *name, LONG session);

static const char *thread_label(DWORD id)
{
    int i, n = g_namedN < NAMED_N ? (int)g_namedN : NAMED_N;

    for (i = n - 1; i >= 0; --i)
        if (g_named[i].id == id && (g_named[i].session == 0 || g_named[i].session == g_session))
            return g_named[i].name;
    if (id == g_readThread)
        return "decodificador: el que lee (read_cb)";
    if (id == g_notifyThread)
        return "decodificador: el que avisa";
    return "";
}

static DWORD WINAPI probe_thread(LPVOID param)
{
    int hw = (int)(DWORD)param;
    char name[24];
    DWORD now, d;

    XSetThreadProcessor(GetCurrentThread(), hw);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    _snprintf(name, sizeof(name) - 1, "sonda HW %d", hw);
    name[sizeof(name) - 1] = 0;
    name_thread(name, 0);
    g_probeTick[hw] = GetTickCount();
    for (;;)
    {
        Sleep(4);
        now = GetTickCount();
        d = now - g_probeTick[hw];         // 4-5 ms si nadie le estorba
        g_probeTick[hw] = now;
        tl_max(&tl_now()->cpu[hw], (LONG)d);
        tl_max(&g_probeMaxS[hw], (LONG)d);
        if (d > 50)
            InterlockedIncrement(&g_probeOver[hw]);
    }
}

static void start_probes(void)
{
#ifndef GV_SIM
    int i;
    HANDLE h;

    if (g_probesOn)
        return;
    g_probesOn = 1;
    for (i = 0; i < HW_N; ++i)
    {
        h = CreateThread(NULL, 16 * 1024, probe_thread, (LPVOID)(DWORD)i, 0, NULL);
        if (h)
            CloseHandle(h);
    }
#endif
}

static void diag_append_file(const char *text)
{
    FILE *f;
    DWORD t0 = GetTickCount();

    OutputDebugStringA(text);
    f = fopen(DIAG_PATH, "a");
    if (f)
    {
        fputs(text, f);
        fclose(f);
    }
    g_lastWriteMs = GetTickCount() - t0;
    tl_max(&tl_now()->writeMax, (LONG)g_lastWriteMs);
}

// Notas guardadas para despues (prueba 4): se escriben antes del siguiente evento
static void diag_flush_notes(void)
{
    if (g_noteLen > 0)
    {
        diag_append_file(g_noteBuf);
        g_noteLen = 0;
        g_noteBuf[0] = 0;
    }
}

static void dcat(const char *fmt, ...)
{
    va_list a;
    int n, room = (int)sizeof(g_dbuf) - 1 - g_dlen;

    if (room <= 0)
        return;
    va_start(a, fmt);
    n = _vsnprintf(g_dbuf + g_dlen, room, fmt, a);
    va_end(a);
    if (n < 0 || n >= room)
        g_dlen = (int)sizeof(g_dbuf) - 1;
    else
        g_dlen += n;
    g_dbuf[g_dlen] = 0;
}

// Segundos del video como "1:42.37" ("-" si no se sabe). Con centesimas:
// a 60 fps un cuadro dura 0.017 s.
static const char *tstr(double s, char *buf, int size)
{
    long t;

    if (s < 0.0)
    {
        _snprintf(buf, size - 1, "-");
        buf[size - 1] = 0;
        return buf;
    }
    t = (long)(s * 100.0 + 0.5);
    _snprintf(buf, size - 1, "%ld:%02ld.%02ld", t / 6000, (t % 6000) / 100, t % 100);
    buf[size - 1] = 0;
    return buf;
}

// Empieza un evento: "[mm:ss] video 1:42.3  " (mm:ss = tiempo desde que abrio)
static void dcat_begin(double pts)
{
    char t[24];
    DWORD w = (GetTickCount() - g_sessionTick) / 1000;

    g_dlen = 0;
    g_dbuf[0] = 0;
    dcat("[%02lu:%02lu] video %s  ", (unsigned long)(w / 60), (unsigned long)(w % 60),
         tstr(pts, t, sizeof(t)));
}

static void add_event(char type, double pts)
{
    g_events[g_eventTotal % DIAG_EVENTS].type = type;
    g_events[g_eventTotal % DIAG_EVENTS].pts = pts;
    ++g_eventTotal;
}

static void dcat_au(const AuRec *r, DWORD now)
{
    char t[24];

    dcat("frag %d cuadro %d  %-16s %8s  %6d bytes  hace %lu ms%s", r->info.frag, r->info.sample,
         r->info.desc, tstr(from_stamp(r->stamp), t, sizeof(t)), r->len,
         (unsigned long)(now - r->tick), r->key ? "  (clave)" : "");
}

static void take_snapshot(Snap *s, DWORD gapStart, double clock)
{
    int i, k;
    DWORD now = GetTickCount();

    memset(s, 0, sizeof(*s));
    s->tick = now;
    s->gapStart = gapStart;
    s->readsInst = g_readsInst;
    s->outInst = g_outInst;
    EnterCriticalSection(&g_frameLock);
    s->readyN = g_readyN;
    s->retN = g_retN;
    LeaveCriticalSection(&g_frameLock);
    s->haveCur = g_haveCur;
    s->retireN = g_retireN;
    s->retireAge = g_retireN > 0 ? now - g_retire[0].tick : 0;
    s->outFrames = g_outFrames;
    s->qCount = g_qCount;
    s->readWaiting = g_readWaiting;
    s->sinceRead = now - g_lastReadTick;
    s->backlog = backlog_now();
    s->lockWait = g_lockWaitMax > g_lockWaitShown ? g_lockWaitMax : g_lockWaitShown;
    s->lastOutPts = g_lastOutPts;
    s->clock = clock;
    s->nextStamp = -1;

    EnterCriticalSection(&g_qLock);
    for (i = 0; i < g_ringN; ++i)
        s->ring[i] = g_ring[(g_ringPos - g_ringN + i + DIAG_RING) % DIAG_RING];
    s->ringN = g_ringN;
    if (g_pendN > 0)
    {
        // el que sigue en orden de presentacion: ya leido o aun en la cola
        s->nextStamp = g_pend[0];
        for (i = 0; i < s->ringN && !s->nextWhere; ++i)
        {
            if (s->ring[i].stamp == s->nextStamp)
            {
                s->next = s->ring[i];
                s->nextWhere = 1;
            }
        }
        for (i = 0; i < g_qCount && !s->nextWhere; ++i)
        {
            k = (g_qHead + i) % QUEUE_CAP;
            if (g_queue[k].stamp == s->nextStamp)
            {
                s->next.info = g_queue[k].info;
                s->next.stamp = g_queue[k].stamp;
                s->next.len = g_queue[k].len;
                s->next.key = g_queue[k].key;
                s->next.tick = now;
                s->nextWhere = 2;
            }
        }
    }
    LeaveCriticalSection(&g_qLock);
}

static double snap_pts(const Snap *s)
{
    if (s->nextStamp >= 0)
        return from_stamp(s->nextStamp);
    return s->lastOutPts;
}

static void dcat_state(const Snap *s)
{
    char t[24];

    if (s->nextStamp >= 0)
    {
        dcat("    esperado %s: ", tstr(from_stamp(s->nextStamp), t, sizeof(t)));
        if (s->nextWhere == 1)
            dcat("frag %d cuadro %d [%s], lo leyo hace %lu ms\n", s->next.info.frag,
                 s->next.info.sample, s->next.info.desc, (unsigned long)(s->tick - s->next.tick));
        else if (s->nextWhere == 2)
            dcat("frag %d cuadro %d [%s], sigue en nuestra cola (no lo ha leido)\n",
                 s->next.info.frag, s->next.info.sample, s->next.info.desc);
        else
            dcat("no esta entre los ultimos %d leidos\n", DIAG_RING);
    }
    dcat("    foto a los %lu ms: dentro %ld (leidos %ld, sacados %ld), prestados %d de %d "
         "(listos %d, pantalla %d, GPU %d", (unsigned long)(s->tick - s->gapStart),
         s->readsInst - s->outInst, s->readsInst, s->outInst,
         s->readyN + s->haveCur + s->retireN + s->retN, s->outFrames,
         s->readyN, s->haveCur, s->retireN);
    if (s->retireN > 0)
        dcat(" de hace %lu ms", (unsigned long)s->retireAge);
    dcat(", por devolver %d)\n", s->retN);
    dcat("    cola %d, pide datos %s, sin leer %lu ms, trabajos GPU %ld, candado max %ld ms, "
         "ultimo cuadro sacado %s\n", s->qCount, s->readWaiting ? "si" : "no",
         (unsigned long)s->sinceRead, s->backlog, s->lockWait, tstr(s->lastOutPts, t, sizeof(t)));
}

static void dcat_ring(const Snap *s, int maxN)
{
    int i = s->ringN > maxN ? s->ringN - maxN : 0;

    dcat("    ultimos leidos (viejo -> nuevo; > = el esperado):\n");
    for (; i < s->ringN; ++i)
    {
        dcat("     %s ", s->ring[i].stamp == s->nextStamp ? ">" : " ");
        dcat_au(&s->ring[i], s->tick);
        dcat("\n");
    }
}

static void dcat_context(void)
{
    char c[320];

    if (!g_diagHook)
        return;
    c[0] = 0;
    g_diagHook(c, sizeof(c));
    c[sizeof(c) - 1] = 0;
    if (c[0])
        dcat("    %s\n", c);
}

// Que hacia cada hilo en este momento (texto de dos lineas)
static void fmt_threads(char *out, int size, const char *when)
{
    DWORD now = GetTickCount();
    int i, pos;

    pos = _snprintf(out, size - 1,
                    "    hilos %s: recolector %s (hace %lu ms); read_cb %s (hace %lu ms); alimentador %s "
                    "(hace %lu ms); ultimo aviso hace %lu ms; hilo principal: ciclo mas largo en 2 s %lu ms\n",
                    when, ts_name(TS_TAKER, g_ts[TS_TAKER]), (unsigned long)(now - g_tsTick[TS_TAKER]),
                    ts_name(TS_READ, g_ts[TS_READ]), (unsigned long)(now - g_tsTick[TS_READ]),
                    ts_name(TS_FEED, g_ts[TS_FEED]), (unsigned long)(now - g_tsTick[TS_FEED]),
                    (unsigned long)(now - g_lastNotifyTick),
                    (unsigned long)(g_mainGapCur > g_mainGapPrev ? g_mainGapCur : g_mainGapPrev));
    if (pos < 0 || pos >= size - 1)
    {
        out[size - 1] = 0;
        return;
    }
    if (g_probesOn)
    {
        int n = _snprintf(out + pos, size - 1 - pos, "    CPU %s: ms sin correr la sonda de cada hilo HW 0-5:", when);
        if (n < 0 || n >= size - 1 - pos)
        {
            out[size - 1] = 0;
            return;
        }
        pos += n;
        for (i = 0; i < HW_N && pos < size - 12; ++i)
        {
            LONG d = (LONG)(now - g_probeTick[i]);

            n = _snprintf(out + pos, size - 1 - pos, " %ld", (long)(d < 0 ? 0 : d));
            if (n < 0)
                break;
            pos += n;
        }
        if (pos < size - 2)
        {
            out[pos++] = '\n';
            out[pos] = 0;
        }
    }
    out[size - 1] = 0;
}

static void dcat_threads(const char *when)
{
    char t[640];

    fmt_threads(t, sizeof(t), when);
    dcat("%s", t);
}

// Linea de tiempo desde 1.5 s antes del hueco hasta ahora
static void dcat_timeline(DWORD gapStart)
{
    DWORD now = GetTickCount();
    LONG p0 = (LONG)(gapStart / 100) - 15, p1 = (LONG)(now / 100), p;
    const TlBucket *b;
    int i;

    if (p1 - p0 > TL_N - 2)
        p0 = p1 - (TL_N - 2);
    dcat("    linea de tiempo cada 0.1 s (t=0: deja de salir; lee/sale/aviso del decodificador; candado = "
         "veces que el recolector lo tomo y su espera mas larga; ciclos del hilo principal; "
         "cpu = espera mas larga de la sonda de cada hilo HW 0-5, 4-5 ms es normal):\n");
    for (p = p0; p <= p1; ++p)
    {
        b = &g_tl[p % TL_N];
        dcat("     %+5.1f ", ((double)p * 100.0 - (double)gapStart) / 1000.0);
        if (b->period != p)
        {
            dcat(" (sin datos)\n");
            continue;
        }
        dcat(" lee %2ld  sale %2ld  aviso %2ld | candado %3ld (%3ld ms) | ciclos %ld | cpu",
             (long)b->reads, (long)b->outs, (long)b->notifies, (long)b->locks, (long)b->lockMax,
             (long)b->updates);
        for (i = 0; i < HW_N; ++i)
            dcat(" %3ld", (long)b->cpu[i]);
        if (b->notes)
            dcat("  <- RECARGA");
        if (b->writeMax > 0)
            dcat("  (escribir %ld ms)", (long)b->writeMax);
        dcat("\n");
    }
}

#ifndef GV_DIAG_XBDM
#if defined(_DEBUG) && !defined(GV_SIM)
#define GV_DIAG_XBDM 1      // si xbdm da error al compilar o enlazar, cambia este 1 por 0
#else
#define GV_DIAG_XBDM 0
#endif
#endif

#if GV_DIAG_XBDM
#include <xbdm.h>
#pragma comment(lib, "xbdm.lib")

// Lista de todos los hilos de la app con su hilo HW y prioridad (una vez
// por video, a los ~5 s). Asi se ve donde corren los hilos del decodificador.
static void diag_thread_dump(void)
{
    static DWORD ids[96];
    static struct { char name[48]; DWORD base, size; } mods[48];
    DWORD n = 96, i, start;
    DM_THREADINFOEX ti;
    PDM_WALK_MODULES walk = NULL;
    DMN_MODLOAD mod;
    const char *mname;
    int nm = 0, k;
    HRESULT hr;

    while (nm < 48 && DmWalkLoadedModules(&walk, &mod) == XBDM_NOERR)
    {
        strncpy(mods[nm].name, mod.Name, sizeof(mods[nm].name) - 1);
        mods[nm].name[sizeof(mods[nm].name) - 1] = 0;
        mods[nm].base = (DWORD)mod.BaseAddress;
        mods[nm].size = (DWORD)mod.Size;
        ++nm;
    }
    if (walk)
        DmCloseLoadedModules(walk);

    dcat_begin(g_lastOutPts);
    dcat("HILOS de la app (lista de xbdm):\n");
    hr = DmGetThreadList(ids, &n);
    if (hr != XBDM_NOERR)
    {
        dcat("    DmGetThreadList fallo (%08lX)\n", (unsigned long)hr);
        diag_append_file(g_dbuf);
        return;
    }
    for (i = 0; i < n && i < 96; ++i)
    {
        memset(&ti, 0, sizeof(ti));
        ti.Size = sizeof(ti);
        if (DmGetThreadInfoEx(ids[i], &ti) != XBDM_NOERR)
        {
            dcat("    id %08lX  (sin datos)\n", (unsigned long)ids[i]);
            continue;
        }
        start = (DWORD)ti.StartAddress;
        mname = "?";
        for (k = 0; k < nm; ++k)
            if (start - mods[k].base < mods[k].size)
                mname = mods[k].name;
        dcat("    id %08lX  HW %d  prioridad %ld  inicio %08lX (%s)  %s\n", (unsigned long)ids[i],
             (int)ti.CurrentProcessor, (long)ti.Priority, (unsigned long)start, mname,
             thread_label(ids[i]));
    }
    diag_append_file(g_dbuf);
}
#endif

static void set_last_event(const char *what, const Snap *s)
{
    if (s->nextWhere)
        _snprintf(g_lastEvent, sizeof(g_lastEvent) - 1,
                  "%s, frag %d:%d [%s], dentro %ld, prestados %d/%d, GPU %ld, sin leer %lu ms",
                  what, s->next.info.frag, s->next.info.sample, s->next.info.desc,
                  s->readsInst - s->outInst, s->readyN + s->haveCur + s->retireN + s->retN,
                  s->outFrames, s->backlog, (unsigned long)s->sinceRead);
    else
        _snprintf(g_lastEvent, sizeof(g_lastEvent) - 1,
                  "%s, dentro %ld, prestados %d/%d, GPU %ld, sin leer %lu ms", what,
                  s->readsInst - s->outInst, s->readyN + s->haveCur + s->retireN + s->retN,
                  s->outFrames, s->backlog, (unsigned long)s->sinceRead);
    g_lastEvent[sizeof(g_lastEvent) - 1] = 0;
}

static void gap_bucket(DWORD ms)
{
    if (ms < 100)
        return;
    ++g_gapHist[ms < 200 ? 0 : ms < 300 ? 1 : ms < 500 ? 2 : ms < 1000 ? 3 : 4];
}

// Salio un cuadro despues de un atasco con foto: fue una pausa
static void diag_pause_end(void)
{
    char what[48];

    ++g_pauses;
    add_event('P', snap_pts(&g_snap));
    dcat_begin(snap_pts(&g_snap));
    dcat("PAUSA de %lu ms (se recupero sola)\n", (unsigned long)g_gapMax);
    dcat_state(&g_snap);
    dcat("%s", g_snapThreads);
    dcat("    durante la pausa: leyo %ld mas, trabajos GPU entre %ld y %ld\n",
         (long)(g_readsInst - g_snap.readsInst), g_epBlMin, g_epBlMax);
    dcat_threads("al terminar");
    dcat_ring(&g_snap, 6);
    dcat_timeline(g_snap.gapStart);
    dcat_context();
    diag_flush_notes();
    diag_append_file(g_dbuf);
    _snprintf(what, sizeof(what) - 1, "Pausa %lu ms", (unsigned long)g_gapMax);
    what[sizeof(what) - 1] = 0;
    set_last_event(what, &g_snap);
}

// Cada refresco, antes del vigilante: mide los huecos y toma la foto
static void diag_track(double clock)
{
    DWORD now = GetTickCount(), start, gap;
    long bl;

    // no cuentan: el decodificador espera datos, no hay nada en la cola,
    // o esta instancia aun no saca su primer cuadro (al abrir o reiniciar)
    if (g_readWaiting || g_qCount == 0 || g_outInst == 0)
        g_gapBase = now;
    start = g_lastOutTick;
    if ((LONG)(g_gapBase - start) > 0)
        start = g_gapBase;
    gap = now - start;

    // como se ve en marcha normal (para comparar con las fotos)
    if (!g_episode && g_outInst > 0)
    {
        long inside = g_readsInst - g_outInst, held = g_readyN + g_haveCur + g_retireN + g_retN;

        bl = backlog_now();
        ++g_nSamples;
        g_sumInside += inside;
        g_sumBacklog += bl > 0 ? bl : 0;
        if (inside > g_maxInside)
            g_maxInside = inside;
        if (held > g_maxHeld)
            g_maxHeld = held;
        if (bl > g_maxBacklog)
            g_maxBacklog = bl;
    }

    if (g_decoded != g_diagSeen)
    {
        // salio algo: termino el hueco anterior
        gap_bucket(g_gapMax);
        if (g_episode)
            diag_pause_end();
        g_episode = 0;
        g_gapMax = 0;
        g_diagSeen = g_decoded;
        return;
    }
    if (gap > g_gapMax)
        g_gapMax = gap;
    if (!g_episode && gap >= GAP_ONSET_MS)
    {
        take_snapshot(&g_snap, start, clock);
        fmt_threads(g_snapThreads, sizeof(g_snapThreads), "en la foto");
        g_episode = 1;
        g_epBlMin = g_epBlMax = g_snap.backlog;
    }
    else if (g_episode)
    {
        bl = backlog_now();
        if (bl < g_epBlMin)
            g_epBlMin = bl;
        if (bl > g_epBlMax)
            g_epBlMax = bl;
    }
}

// Justo antes de reiniciar: arma el texto (se escribe despues del reinicio)
static void diag_restart_begin(double clock)
{
    DWORD now = GetTickCount();

    if (!g_episode)
    {
        // sin foto (por ejemplo, el reinicio anterior no saco nada): ahora
        take_snapshot(&g_snap, g_lastOutTick, clock);
        fmt_threads(g_snapThreads, sizeof(g_snapThreads), "en la foto");
        g_epBlMin = g_epBlMax = g_snap.backlog;
    }
    ++g_gapHist[5];
    add_event('R', snap_pts(&g_snap));
    dcat_begin(snap_pts(&g_snap));
    dcat("REINICIO #%ld (no salio nada en %lu ms)\n", g_restarts + 1,
         (unsigned long)(now - g_snap.gapStart));
    dcat_state(&g_snap);
    dcat("%s", g_snapThreads);
    dcat_threads("al reiniciar");
    dcat("    al reiniciar: sin leer %lu ms, leyo %ld mas desde la foto, trabajos GPU %ld "
         "(entre %ld y %ld)\n", (unsigned long)(now - g_lastReadTick),
         (long)(g_readsInst - g_snap.readsInst), backlog_now(), g_epBlMin, g_epBlMax);
    dcat_ring(&g_snap, DIAG_RING);
    dcat_timeline(g_snap.gapStart);
    dcat_context();
    set_last_event("Reinicio", &g_snap);
    g_episode = 0;
    g_gapMax = 0;
}

static void diag_restart_end(DWORD ms)
{
    dcat("    el reinicio tardo %lu ms%s\n", (unsigned long)ms,
         g_dec ? "" : " y el decodificador NO volvio a abrir");
    diag_flush_notes();
    diag_append_file(g_dbuf);
    g_diagSeen = g_decoded;
}

static void diag_summary(void)
{
    dcat_begin(g_lastOutPts);
    dcat("FIN (se cerro el decodificador)\n");
    dcat("    decodificados %ld, mostrados %ld, tirados %ld; reinicios %ld, pausas %ld, recargas %ld\n",
         g_decoded, g_shown, g_dropped, g_restarts, g_pauses, g_notes);
    dcat("    huecos sin salida: 0.1-0.2 s: %ld | 0.2-0.3 s: %ld | 0.3-0.5 s: %ld | 0.5-1 s: %ld | "
         "1-2 s: %ld | reinicio: %ld\n", g_gapHist[0], g_gapHist[1], g_gapHist[2], g_gapHist[3],
         g_gapHist[4], g_gapHist[5]);
    if (g_nSamples > 0)
        dcat("    en marcha normal: dentro %.1f en promedio (max %ld), prestados max %ld de %d, "
             "trabajos GPU %.1f en promedio (max %ld)\n", (double)g_sumInside / g_nSamples,
             g_maxInside, g_maxHeld, g_outFrames, (double)g_sumBacklog / g_nSamples, g_maxBacklog);
    if (g_probesOn)
    {
        int k;

        dcat("    CPU por hilo HW (sondas): espera mas larga y veces que espero mas de 50 ms:");
        for (k = 0; k < HW_N; ++k)
            dcat("  HW%d %ld ms/%ld", k, (long)g_probeMaxS[k], (long)g_probeOver[k]);
        dcat("\n");
    }
    dcat("    hilo principal: el ciclo mas largo fue de %lu ms\n", (unsigned long)g_mainGapMax);
    if (g_episode)
        dcat("    (al cerrar habia un atasco de %lu ms sin terminar)\n", (unsigned long)g_gapMax);
    diag_flush_notes();
    diag_append_file(g_dbuf);
}

// ---------------------------------------------------------------------------
// Candado del decodificador.
// El hilo "motor" del decodificador trabaja dentro de su propio candado y
// las llamadas que sacan o devuelven cuadros tienen que tomarlo tambien.
// Su direccion es fija para la version de dash.ExtraAVCodecs que conocemos;
// antes de usarla se comprueba que el codigo en esa zona sea el esperado.
// (Direccion y firma: HG-ERIK.)

static void find_motor_lock(void)
{
    g_motor = NULL;
#ifdef GV_SIM
    return;   /* simulacion en PC: esa direccion no existe */
#endif
    __try
    {
        const DWORD *code = (const DWORD *)0x9F89CAA0;
        if (code[0] == 0x3D609FB1 && code[1] == 0x7C641B78 && code[2] == 0x386B9160)
            g_motor = (CRITICAL_SECTION *)(0x9FB09160 + 0xC4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_motor = NULL;
    }
}

static int motor_try(void)
{
    return !g_motor || TryEnterCriticalSection(g_motor);
}

static void motor_enter(void)
{
    if (g_motor)
        EnterCriticalSection(g_motor);
}

static void motor_leave(void)
{
    if (g_motor)
        LeaveCriticalSection(g_motor);
}

// Devuelve al decodificador los cuadros pendientes. Llamar con el candado.
static void flush_returns(void)
{
    DWORD h[RET_CAP];
    int i, n;

    EnterCriticalSection(&g_frameLock);
    n = g_retN;
    memcpy(h, g_ret, n * sizeof(DWORD));
    g_retN = 0;
    LeaveCriticalSection(&g_frameLock);
    for (i = 0; i < n; ++i)
        H264DecodeReturnOutputGPU(g_dec, h[i], 0);
}

// Deja un cuadro para devolver. Lo devuelve el hilo recolector (que tiene
// el candado). Nunca se llama con g_frameLock tomado desde afuera.
static void give_back(DWORD handle)
{
    int full;

    EnterCriticalSection(&g_frameLock);
    full = g_retN == RET_CAP;
    if (!full)
        g_ret[g_retN++] = handle;
    LeaveCriticalSection(&g_frameLock);
    if (full)
    {
        motor_enter();
        H264DecodeReturnOutputGPU(g_dec, handle, 0);
        motor_leave();
    }
}

// ---------------------------------------------------------------------------
// Al salir de la app el sistema llama esto, para que los hilos del
// decodificador no sigan corriendo sobre memoria liberada.

static void terminate_routine(TermReg *reg)
{
    int i, got = 0;

    ExRegisterTitleTerminateNotification(reg, FALSE);
    g_takerStop = 1;   // el hilo recolector no debe tocar nada mas
    if (g_dec && g_motor)
    {
        for (i = 0; i < 500 && !got; ++i)
        {
            got = TryEnterCriticalSection(g_motor) ? 1 : 0;
            if (!got)
                Sleep(2);
        }
        if (got)
            LeaveCriticalSection(g_motor);
        else
            g_dec = NULL;   // esta trabado: mejor no tocarlo
    }
    if (g_dec)
    {
        g_closing = 1;
        H264DecodeClose(g_dec);
        g_dec = NULL;
    }
}

// ---------------------------------------------------------------------------
// Callbacks que llama el decodificador desde sus hilos

// Pide el siguiente cuadro. Espera hasta que haya uno. Al cerrar devuelve
// sin datos. El buffer entregado debe seguir vivo hasta la siguiente llamada.
static int __cdecl read_cb(void *ctx, BYTE **data, DWORD *len, DWORD unused, BYTE *info)
{
    InputAu au;

    (void)ctx;
    (void)unused;
    g_readThread = GetCurrentThreadId();
    for (;;)
    {
        if (g_closing)
        {
            *data = NULL;
            *len = 0;
            ts_set(TS_READ, RD_OUT);
            return 0;
        }
        if (g_feeding)
        {
            ts_set(TS_READ, RD_LOCK);
            EnterCriticalSection(&g_qLock);
            while (g_qCount > 0 && g_needHdr && !g_queue[g_qHead].key)
            {
                // reiniciado: se tira todo hasta el siguiente cuadro clave
                ts_set(TS_READ, RD_FREE);
                free(g_queue[g_qHead].data);
                g_qHead = (g_qHead + 1) % QUEUE_CAP;
                --g_qCount;
            }
            if (g_qCount > 0)
            {
                au = g_queue[g_qHead];
                g_qHead = (g_qHead + 1) % QUEUE_CAP;
                --g_qCount;
                if (g_needHdr && g_hdrLen > 0)
                {
                    // AUD | SPS | PPS | resto del cuadro
                    BYTE *nb;

                    ts_set(TS_READ, RD_HDR);
                    nb = (BYTE *)malloc(au.len + g_hdrLen);
                    if (nb)
                    {
                        int aud = (au.len > 6 && au.data[0] == 0 && au.data[1] == 0 &&
                                   au.data[2] == 0 && au.data[3] == 1 &&
                                   (au.data[4] & 0x1F) == 9) ? 6 : 0;
                        memcpy(nb, au.data, aud);
                        memcpy(nb + aud, g_hdr, g_hdrLen);
                        memcpy(nb + aud + g_hdrLen, au.data + aud, au.len - aud);
                        free(au.data);
                        au.data = nb;
                        au.len += g_hdrLen;
                    }
                }
                g_needHdr = 0;
                ts_set(TS_READ, RD_FREE);
                if (g_stage)
                    free(g_stage);
                g_stage = au.data;
                // diagnostico: recordar los ultimos cuadros leidos
                g_ring[g_ringPos].info = au.info;
                g_ring[g_ringPos].stamp = au.stamp;
                g_ring[g_ringPos].len = au.len;
                g_ring[g_ringPos].key = au.key;
                g_ring[g_ringPos].tick = GetTickCount();
                g_ringPos = (g_ringPos + 1) % DIAG_RING;
                if (g_ringN < DIAG_RING)
                    ++g_ringN;
                g_lastReadTick = GetTickCount();
                LeaveCriticalSection(&g_qLock);

                if (info)
                {
                    *(__int64 *)(info + 8) = au.stamp;
                    *(DWORD *)(info + 0x10) = 1;
                }
                *data = au.data;
                *len = (DWORD)au.len;
                InterlockedIncrement(&g_reads);
                InterlockedIncrement(&g_readsInst);
                InterlockedIncrement(&tl_now()->reads);
                g_readWaiting = 0;
                ts_set(TS_READ, RD_OUT);
                return 0;
            }
            LeaveCriticalSection(&g_qLock);
        }
        g_readWaiting = 1;
        ts_set(TS_READ, RD_WAIT);
#ifdef GV_SIM
        // simulacion de un solo hilo: nadie mas puede traer datos
        *data = NULL;
        *len = 0;
        ts_set(TS_READ, RD_OUT);
        return 0;
#endif
        WaitForSingleObject(g_dataEv, 10);
    }
}

// Avisa que hay un cuadro decodificado
static void __cdecl notify_cb(void *ev)
{
    g_lastNotifyTick = GetTickCount();
    g_notifyThread = GetCurrentThreadId();
    InterlockedIncrement(&tl_now()->notifies);
    SetEvent((HANDLE)ev);
}

// ---------------------------------------------------------------------------
// Shaders para pasar de YUV (como sale del decodificador) a RGB

static const char g_vsSrc[] =
    "float4 screen : register(c0);\n"
    "struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut main(VSIn i)\n"
    "{\n"
    "    VSOut o;\n"
    "    o.pos = float4(i.pos.x * screen.x - 1.0, i.pos.y * screen.y + 1.0, 0.0, 1.0);\n"
    "    o.uv  = i.uv;\n"
    "    return o;\n"
    "}\n";

// BT.601 rango limitado (16-235): lo normal para videos de 360p/480p
static const char g_psSrc[] =
    "sampler2D texY : register(s0);\n"
    "sampler2D texU : register(s1);\n"
    "sampler2D texV : register(s2);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR\n"
    "{\n"
    "    float y = (tex2D(texY, uv).r - 0.0625) * 1.164;\n"
    "    float u = tex2D(texU, uv).r - 0.5;\n"
    "    float v = tex2D(texV, uv).r - 0.5;\n"
    "    return float4(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u, 1.0);\n"
    "}\n";

static const D3DVERTEXELEMENT9 g_elements[] =
{
    { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
};

struct QuadVert
{
    float x, y, u, v;
};

static int build_shaders(void)
{
    ID3DXBuffer *code = NULL, *err = NULL;

    if (FAILED(D3DXCompileShader(g_vsSrc, (UINT)strlen(g_vsSrc), NULL, NULL, "main", "vs_2_0",
                                 0, &code, &err, NULL)))
    {
        if (err)
        {
            OutputDebugStringA((const char *)err->GetBufferPointer());
            err->Release();
        }
        return 0;
    }
    g_dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &g_vs);
    code->Release();

    code = NULL;
    err = NULL;
    if (FAILED(D3DXCompileShader(g_psSrc, (UINT)strlen(g_psSrc), NULL, NULL, "main", "ps_2_0",
                                 0, &code, &err, NULL)))
    {
        if (err)
        {
            OutputDebugStringA((const char *)err->GetBufferPointer());
            err->Release();
        }
        return 0;
    }
    g_dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &g_ps);
    code->Release();

    return SUCCEEDED(g_dev->CreateVertexDeclaration(g_elements, &g_decl));
}

// ---------------------------------------------------------------------------
// API

extern "C" int gv_init(void *device, void *pp)
{
    static BYTE initOut[0x80];
    DWORD initEx[8];
    DWORD affinity[XAVMaxThreadTypes] = { 4, 1, 1 };
    HRESULT hrXav, hrQueues;
    char msg[128];

    g_dev = (IDirect3DDevice9 *)device;
    InitializeCriticalSection(&g_qLock);
    InitializeCriticalSection(&g_frameLock);
    g_dataEv = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!build_shaders())
    {
        set_status("No compilaron los shaders de video");
        return 0;
    }

    // XAVInitialize con la estructura extendida: dispositivo, parametros
    // de presentacion, sin modo juego, y un buffer de salida al final.
    initEx[0] = (DWORD)device;
    initEx[1] = (DWORD)pp;
    initEx[2] = FALSE;
    initEx[3] = 1;
    initEx[4] = 0;
    initEx[5] = 0;
    initEx[6] = 0;
    initEx[7] = (DWORD)initOut;
#ifdef GV_XAV_PUBLIC
    hrXav = XAVInitialize((const XAVInit *)initEx, affinity);
#else
    hrXav = XAVInitialize((const XAVInitEx *)initEx, affinity);
#endif
    if (FAILED(hrXav))
    {
        _snprintf(msg, sizeof(msg) - 1, "XAVInitialize fallo (%08lX)", (unsigned long)hrXav);
        msg[sizeof(msg) - 1] = 0;
        set_status(msg);
        return 0;
    }

    find_motor_lock();
    start_probes();

    g_term.routine = (void *)terminate_routine;
    g_term.priority = 100;
    ExRegisterTitleTerminateNotification(&g_term, TRUE);

    // El decodificador graba su trabajo de GPU en colas de baja prioridad;
    // el dispositivo no las tiene hasta que se crean aqui.
    hrQueues = D3DDevice_ResetLowPriorityQueues((D3DDevice *)g_dev, 1, 1024, 0);
    g_ok = SUCCEEDED(hrQueues) ? 1 : 0;

    _snprintf(msg, sizeof(msg) - 1, "XAV %08lX, colas %08lX, candado %s",
              (unsigned long)hrXav, (unsigned long)hrQueues,
              g_motor ? "encontrado" : "NO encontrado");
    msg[sizeof(msg) - 1] = 0;
    set_status(msg);
    return g_ok;
}

extern "C" int gv_available(void)
{
    return g_ok;
}

extern "C" int gv_motor_lock(void)
{
    return g_motor != NULL;
}

extern "C" const char *gv_status(void)
{
    return g_status;
}

extern "C" int gv_is_open(void)
{
    return g_dec != NULL;
}

static void close_decoder(void);
static void start_taker(void);
static void stop_taker(void);

// Crea y arranca el decodificador con g_level (lo usan gv_open y el reinicio)
static int create_decoder(void)
{
    int pass;

    // instancia nueva: el diagnostico empieza de cero
    EnterCriticalSection(&g_qLock);
    g_ringN = 0;
    g_ringPos = 0;
    LeaveCriticalSection(&g_qLock);
    g_readsInst = 0;
    g_outInst = 0;
    g_lastReadTick = GetTickCount();

    // Primero con mas memoria para el decodificador; si no la acepta, con
    // los valores de HG-ERIK (nivel minimo 4.1 y 8 cuadros de salida).
    //
    // Por que (prueba 4d, itag 298): en la pausa el decodificador ya habia
    // leido 8163 cuadros y sacado 8158; tenia datos de sobra y 7 cuadros de
    // salida libres, y aun asi no avanzaba ~6-30 s. Solo pasa en algunos
    // videos. Lo mas probable es que se quede sin buffers internos para
    // cuadros de referencia: con nivel 4.1 y tamaño maximo 1080p solo
    // reserva 4, y con 6 hilos decodificando a la vez necesita mas. El
    // nivel 5.1 le hace reservar hasta 16.
    //
    // Modo de prueba 1 o 2: menos hilos CABAC. Si asi no abre, con los normales.
    for (pass = 0; pass < 2 && !g_dec; ++pass)
    {
        static const int tryLevel[2]  = { 51, 0 };
        static const int tryFrames[2] = { DEC_OUT_FRAMES_BIG, DEC_OUT_FRAMES };
        int t;

        if (pass == 1)
        {
            if (g_decThreads == DEC_THREADS)
                break;
            g_decThreads = DEC_THREADS;
            g_threadsFallback = 1;
        }
        for (t = 0; t < 2 && !g_dec; ++t)
        {
            int lv = tryLevel[t] ? tryLevel[t] : (g_level < 41 ? 41 : (g_level > 51 ? 51 : g_level));

#define FIELD(off) (*(DWORD *)(g_params + (off)))
            memset(g_params, 0, sizeof(g_params));
            H264InitParamStructureEx(g_params, 0x47, sizeof(g_params));
            g_params[0xE] = 1;
            FIELD(0x10)  = 1;
            FIELD(0x14)  = (DWORD)read_cb;         // de donde saca los cuadros
            FIELD(0x18)  = 0;
            FIELD(0x1C)  = (DWORD)notify_cb;       // aviso de cuadro listo
            FIELD(0x20)  = (DWORD)g_outEv;
            FIELD(0x24)  = g_decThreads;           // hilos para CABAC
            FIELD(0x48)  = 1;
            FIELD(0x58)  = 1920;                   // tamaño maximo
            FIELD(0x5C)  = 1080;
            FIELD(0x68)  = 0;
            FIELD(0x70)  = lv;                     // nivel: decide cuantas referencias reserva
            FIELD(0x80)  = (DWORD)g_dev;
            FIELD(0x94)  = tryFrames[t];           // cuadros de salida
            FIELD(0x9C)  = 1;
            FIELD(0x100) = 1;
            FIELD(0x104) = 1;
#undef FIELD
            g_dec = H264DecodeCreateEx(g_params, NULL);
            if (g_dec)
            {
                g_outFrames = tryFrames[t];
                g_openLevel = lv;
            }
        }
    }
    if (!g_dec)
        return 0;
    H264DecodeStart(g_dec, 0);
    return 1;
}

extern "C" int gv_open(int level)
{
    char msg[128];

    if (!g_ok)
        return 0;
    if (g_dec)
        return 1;

    g_qHead = g_qCount = 0;
    g_pendN = 0;
    g_fed = g_taken = 0;
    g_readyN = 0;
    g_haveCur = g_curDrawn = 0;
    g_retireN = 0;
    g_retN = 0;
    g_decoded = g_shown = g_dropped = 0;
    g_takeOk = g_takeMiss = 0;
    g_reads = 0;
    g_readWaiting = 0;
    g_lastOutTick = GetTickCount();
    g_lastKick = 0;
    g_kicks = 0;
    g_firstPts = -1.0;
    g_clock = -1.0;
    g_closing = 0;
    g_feeding = 1;

    g_outEv = CreateEvent(NULL, TRUE, FALSE, NULL);

    g_level = level;
    g_hdrLen = 0;
    g_needHdr = 0;
    g_restarts = 0;
    g_lockWaitMax = 0;
    g_lockWaitShown = 0;
    g_lockWaitTick = GetTickCount();

    // modo de prueba (ver gv_set_test_mode)
    {
        static const int threads[GV_TEST_MODES] = { DEC_THREADS, 6, 4, 2, DEC_THREADS };

        g_readyCap = READY_CAP;
        g_decThreads = threads[g_testMode >= 0 && g_testMode < GV_TEST_MODES ? g_testMode : 0];
    }
    g_threadsFallback = 0;

    // diagnostico
    g_sessionTick = GetTickCount();
    g_gapBase = g_sessionTick;
    g_diagSeen = 0;
    g_gapMax = 0;
    g_episode = 0;
    memset(g_gapHist, 0, sizeof(g_gapHist));
    g_pauses = 0;
    g_notes = 0;
    g_nSamples = g_sumInside = g_maxInside = g_maxHeld = g_sumBacklog = 0;
    g_maxBacklog = -1;
    g_threadsDumped = 0;
    g_mainLast = 0;
    g_mainGapMax = 0;
    g_mainGapCur = g_mainGapPrev = 0;
    g_mainGapTick = GetTickCount();
    g_noteLen = 0;
    g_noteBuf[0] = 0;
    {
        int k;

        for (k = 0; k < HW_N; ++k)
        {
            g_probeMaxS[k] = 0;
            g_probeOver[k] = 0;
        }
    }
    g_eventTotal = 0;
    g_lastEvent[0] = 0;
    g_lastOutPts = -1.0;
    g_restartTick = g_sessionTick;
    g_outSinceRestart = 0;

    if (!create_decoder())
    {
        set_status("H264DecodeCreateEx fallo");
        CloseHandle(g_outEv);
        g_outEv = NULL;
        return 0;
    }
    start_taker();

    _snprintf(msg, sizeof(msg) - 1, "Decodificador abierto: nivel %d.%d, %d salidas, %d hilo%s%s, %d listos, candado %s",
              g_openLevel / 10, g_openLevel % 10, g_outFrames, g_decThreads, g_decThreads == 1 ? "" : "s",
              g_threadsFallback ? " (con menos no abrio)" : "", g_readyCap,
              g_motor ? "encontrado" : "NO encontrado");
    msg[sizeof(msg) - 1] = 0;
    set_status(msg);
    return 1;
}

// Guarda los SPS/PPS del primer cuadro (para el reinicio)
static void capture_headers(const unsigned char *p, int len)
{
    int i = 0, start, end, type;

    while (i + 4 < len)
    {
        if (!(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1))
        {
            ++i;
            continue;
        }
        start = i;
        type = p[i + 4] & 0x1F;
        end = i + 4;
        while (end + 3 < len && !(p[end] == 0 && p[end + 1] == 0 && p[end + 2] == 0 && p[end + 3] == 1))
            ++end;
        if (end + 3 >= len)
            end = len;
        if ((type == 7 || type == 8) && g_hdrLen + (end - start) <= (int)sizeof(g_hdr))
        {
            memcpy(g_hdr + g_hdrLen, p + start, end - start);
            g_hdrLen += end - start;
        }
        i = end;
    }
}

extern "C" int gv_feed(const unsigned char *annexb, int len, double pts, int key, const GvAuInfo *info)
{
    BYTE *copy;
    __int64 stamp;
    int i;

    // Durante un reinicio el alimentador solo espera (antes recibia -1,
    // creia que el decodificador se habia cerrado y dejaba de alimentar)
    if (g_restarting)
        return 0;
    if (!g_dec || g_closing || !g_feeding)
        return -1;
    if (g_qCount >= QUEUE_CAP)
    {
        if (g_ts[TS_FEED] != FD_FULL)
            ts_set(TS_FEED, FD_FULL);
        return 0;
    }

    ts_set(TS_FEED, FD_MALLOC);
    copy = (BYTE *)malloc(len);
    if (!copy)
    {
        ts_set(TS_FEED, FD_OUT);
        return 0;
    }
    memcpy(copy, annexb, len);
    stamp = to_stamp(pts);
    if (g_hdrLen == 0)
        capture_headers(annexb, len);

    ts_set(TS_FEED, FD_LOCK);
    EnterCriticalSection(&g_qLock);
    i = (g_qHead + g_qCount) % QUEUE_CAP;
    g_queue[i].data = copy;
    g_queue[i].len = len;
    g_queue[i].stamp = stamp;
    g_queue[i].key = key;
    if (info)
        g_queue[i].info = *info;
    else
        memset(&g_queue[i].info, 0, sizeof(GvAuInfo));
    ++g_qCount;

    // marcas pendientes, en orden (insercion simple: son pocas)
    if (g_pendN == PEND_CAP)
    {
        memmove(g_pend, g_pend + 1, (PEND_CAP - 1) * sizeof(g_pend[0]));
        --g_pendN;
        InterlockedIncrement(&g_taken);
    }
    i = g_pendN;
    while (i > 0 && g_pend[i - 1] > stamp)
    {
        g_pend[i] = g_pend[i - 1];
        --i;
    }
    g_pend[i] = stamp;
    ++g_pendN;
    InterlockedIncrement(&g_fed);
    LeaveCriticalSection(&g_qLock);
    ts_set(TS_FEED, FD_OUT);

    SetEvent(g_dataEv);
    return 1;
}

// Quita la marca de un cuadro que salio. Las anteriores (en tiempo) ya no
// van a salir, asi que tambien cuentan como tomadas. 0 si no la conocemos.
static int settle_stamp(__int64 stamp)
{
    int i, found = -1;

    EnterCriticalSection(&g_qLock);
    for (i = 0; i < g_pendN; ++i)
    {
        if (g_pend[i] == stamp)
        {
            found = i;
            break;
        }
    }
    if (found >= 0)
    {
        memmove(g_pend, g_pend + found + 1, (g_pendN - found - 1) * sizeof(g_pend[0]));
        g_pendN -= found + 1;
        InterlockedExchangeAdd(&g_taken, found + 1);
    }
    LeaveCriticalSection(&g_qLock);
    return found >= 0;
}

// Saca del decodificador los cuadros listos (con el candado ya tomado)
static void take_frames(void)
{
    __declspec(align(8)) BYTE info[0xA0];
    DWORD fr[15];
    Frame f;
    int i;

    for (;;)
    {
        EnterCriticalSection(&g_frameLock);
        i = g_readyN < g_readyCap;
        LeaveCriticalSection(&g_frameLock);
        if (!i)
            break;
        memset(info, 0, sizeof(info));
        memset(fr, 0, sizeof(fr));
        ResetEvent(g_outEv);
        ts_set(TS_TAKER, TK_INFO);
        if (H264DecodeGetOutputInfo(g_dec, info, sizeof(info)) != 0)
            break;
        ts_set(TS_TAKER, TK_GET);
        if (H264DecodeGetOutputGPU(g_dec, fr) != 0)
            break;
        InterlockedIncrement(&tl_now()->outs);

        f.heightY = fr[1];
        f.heightC = fr[3];
        f.pitchY  = fr[4];
        f.pitchC  = fr[5];
        f.y = fr[6];
        f.u = fr[7];
        f.v = fr[8];
        f.cropX = fr[9];
        f.cropY = fr[10];
        f.showW = fr[11];
        f.showH = fr[12];
        f.handle = fr[14];
        f.pts = from_stamp(*(const __int64 *)(info + 0x60));
        InterlockedIncrement(&g_outInst);

        ts_set(TS_TAKER, TK_SETTLE);
        if (!settle_stamp(*(const __int64 *)(info + 0x60)))
        {
            // no es de los que mandamos: se devuelve de una vez
            H264DecodeReturnOutputGPU(g_dec, f.handle, 0);
            continue;
        }

        g_lastOutPts = f.pts;
        ++g_decoded;
        ++g_outSinceRestart;
        g_lastOutTick = GetTickCount();
        if (g_firstPts < 0.0)
        {
            char msg[128];
            g_firstPts = f.pts;
            _snprintf(msg, sizeof(msg) - 1, "Primer cuadro %lux%lu  \xC2\xB7  decodificador nivel %d.%d, %d salidas",
                      (unsigned long)f.showW, (unsigned long)f.showH,
                      g_openLevel / 10, g_openLevel % 10, g_outFrames);
            msg[sizeof(msg) - 1] = 0;
            set_status(msg);
        }

        // en orden de pts (normalmente ya vienen asi)
        ts_set(TS_TAKER, TK_QUEUE);
        EnterCriticalSection(&g_frameLock);
        i = g_readyN;
        while (i > 0 && g_ready[i - 1].pts > f.pts)
        {
            g_ready[i] = g_ready[i - 1];
            --i;
        }
        g_ready[i] = f;
        ++g_readyN;
        LeaveCriticalSection(&g_frameLock);
    }
}

static void pop_ready(void)
{
    memmove(g_ready, g_ready + 1, (g_readyN - 1) * sizeof(Frame));
    --g_readyN;
}

// El cuadro en pantalla deja de serlo
static void retire_current(void)
{
    if (!g_haveCur)
        return;
    if (!g_curDrawn)
        give_back(g_cur.handle);
    else
    {
        if (g_retireN == RETIRE_CAP)
        {
            // no deberia pasar; espera a la GPU con el mas viejo
            DWORD t0 = GetTickCount();
            while (g_dev->IsFencePending(g_retire[0].fence) && GetTickCount() - t0 < 100)
                Sleep(1);
            give_back(g_retire[0].f.handle);
            memmove(g_retire, g_retire + 1, (RETIRE_CAP - 1) * sizeof(Retiring));
            --g_retireN;
        }
        g_retire[g_retireN].f = g_cur;
        g_retire[g_retireN].fence = g_curFence;
        g_retire[g_retireN].tick = GetTickCount();
        ++g_retireN;
    }
    g_haveCur = 0;
}

static void set_current(const Frame *f)
{
    retire_current();
    g_cur = *f;
    g_haveCur = 1;
    g_curDrawn = 0;
    ++g_shown;

    XGSetTextureHeader(f->pitchY, f->heightY, 1, 0, D3DFMT_LIN_L8, 0, 0, 0, 0, &g_tex[0], NULL, NULL);
    XGSetTextureHeader(f->pitchC, f->heightC, 1, 0, D3DFMT_LIN_L8, 0, 0, 0, 0, &g_tex[1], NULL, NULL);
    XGSetTextureHeader(f->pitchC, f->heightC, 1, 0, D3DFMT_LIN_L8, 0, 0, 0, 0, &g_tex[2], NULL, NULL);
    XGOffsetResourceAddress(&g_tex[0], (void *)f->y);
    XGOffsetResourceAddress(&g_tex[1], (void *)f->u);
    XGOffsetResourceAddress(&g_tex[2], (void *)f->v);
}

// ---------------------------------------------------------------------------
// Hilo recolector (4e). Antes el hilo de dibujo intentaba tomar el candado
// una vez por refresco (60 veces por segundo) y fallaba muchas veces: a
// 60 fps los cuadros salian tarde y se tiraban ~40%. Este hilo espera el
// aviso del decodificador ("hay cuadro listo") y entonces espera el
// candado sin girar, asi lo agarra en cuanto el motor lo suelta.

static void taker_step(void)
{
    int room, rets;
    DWORD t0, w;

    EnterCriticalSection(&g_frameLock);
    room = g_readyN < g_readyCap;
    rets = g_retN;
    LeaveCriticalSection(&g_frameLock);

#ifndef GV_SIM
    if (!room && !rets)
    {
        ts_set(TS_TAKER, TK_SLEEP);
        Sleep(2);                          // fila llena: nada que hacer
        return;
    }
    if (room && !rets)
    {
        ts_set(TS_TAKER, TK_WAITEV);
        WaitForSingleObject(g_outEv, 4);   // espera el aviso (o 4 ms)
    }
    if (g_takerStop)
        return;
#else
    if (!room && !rets)
        return;
#endif

    ts_set(TS_TAKER, TK_WAITLOCK);
    t0 = GetTickCount();
    motor_enter();
    if (g_takerStop || !g_dec)
    {
        motor_leave();   // se esta cerrando: no tocar el decodificador
        ts_set(TS_TAKER, TK_DONE);
        return;
    }
    w = GetTickCount() - t0;
    if ((LONG)w > g_lockWaitMax)
        g_lockWaitMax = (LONG)w;
    {
        TlBucket *b = tl_now();

        InterlockedIncrement(&b->locks);
        tl_max(&b->lockMax, (LONG)w);
    }
    ts_set(TS_TAKER, TK_RETURN);
    flush_returns();
    if (room)
        take_frames();
    motor_leave();
    ts_set(TS_TAKER, TK_DONE);
}

static DWORD WINAPI taker_thread(LPVOID param)
{
    (void)param;
    XSetThreadProcessor(GetCurrentThread(), 5);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    gv_diag_thread_name("recolector");
    while (!g_takerStop)
        taker_step();
    return 0;
}

static void start_taker(void)
{
#ifndef GV_SIM
    g_takerStop = 0;
    g_takerThread = CreateThread(NULL, 64 * 1024, taker_thread, NULL, 0, NULL);
#endif
}

static void stop_taker(void)
{
    if (!g_takerThread)
        return;
    g_takerStop = 1;
    if (g_outEv)
        SetEvent(g_outEv);
    WaitForSingleObject(g_takerThread, 3000);
    CloseHandle(g_takerThread);
    g_takerThread = NULL;
}

// Reinicio automatico. En videos de 720p60/1080p el decodificador a veces
// se queda parado: tiene datos y cuadros libres, el candado esta libre, y
// no lee ni saca nada durante 6 s, 30 s o para siempre (prueba 4d). No
// encontramos la causa. Si pasa, se cierra, se vuelve a abrir y se sigue
// desde el siguiente cuadro clave de la cola (con SPS/PPS otra vez).
static void restart_decoder(void)
{
    int i, n;
    char msg[128];

    g_restarting = 1;
    __lwsync();
    close_decoder();

    EnterCriticalSection(&g_qLock);
    if (g_stage)
        free(g_stage);
    g_stage = NULL;
    // las marcas pendientes ahora son solo las de la cola
    g_pendN = 0;
    for (i = 0, n = g_qCount; i < n && g_pendN < PEND_CAP; ++i)
    {
        __int64 st = g_queue[(g_qHead + i) % QUEUE_CAP].stamp;
        int j = g_pendN;
        while (j > 0 && g_pend[j - 1] > st)
        {
            g_pend[j] = g_pend[j - 1];
            --j;
        }
        g_pend[j] = st;
        ++g_pendN;
    }
    g_taken = g_fed - g_pendN;
    g_needHdr = 1;
    LeaveCriticalSection(&g_qLock);

    g_closing = 0;
    g_feeding = 1;
    ResetEvent(g_outEv);
    ++g_restarts;
    g_lastOutTick = GetTickCount();
    g_restartTick = g_lastOutTick;
    g_outSinceRestart = 0;

    if (!create_decoder())
    {
        set_status("Reinicio: el decodificador no volvio a abrir");
        g_restarting = 0;
        return;
    }
    start_taker();
    __lwsync();
    g_restarting = 0;
    _snprintf(msg, sizeof(msg) - 1, "Decodificador reiniciado (%ld)", g_restarts);
    msg[sizeof(msg) - 1] = 0;
    set_status(msg);
}

extern "C" void gv_update(double clock)
{
    int i, nd, haveShow, full;
    DWORD drops[READY_CAP];
    Frame show;

    if (!g_dec)
        return;
    g_clock = clock;

    // diagnostico: cada cuanto corre el hilo principal
    {
        DWORD now = GetTickCount();

        if (now - g_mainGapTick >= 1000)
        {
            g_mainGapPrev = g_mainGapCur;
            g_mainGapCur = 0;
            g_mainGapTick = now;
        }
        if (g_mainLast)
        {
            if (now - g_mainLast > g_mainGapMax)
                g_mainGapMax = now - g_mainLast;
            if (now - g_mainLast > g_mainGapCur)
                g_mainGapCur = now - g_mainLast;
        }
        g_mainLast = now;
        InterlockedIncrement(&tl_now()->updates);
    }
#if GV_DIAG_XBDM
    if (!g_threadsDumped && g_decoded >= 150 && !g_episode)
    {
        g_threadsDumped = 1;
        diag_thread_dump();
    }
#endif

    // cuadros que la GPU ya termino de dibujar: se pueden devolver
    i = 0;
    while (i < g_retireN)
    {
        if (!g_dev->IsFencePending(g_retire[i].fence))
        {
            give_back(g_retire[i].f.handle);
            memmove(g_retire + i, g_retire + i + 1, (g_retireN - i - 1) * sizeof(Retiring));
            --g_retireN;
        }
        else
            ++i;
    }

#ifdef GV_SIM
    taker_step();   // en la simulacion no hay hilo recolector
#endif

    // espera mas larga por el candado en el ultimo segundo (para el panel)
    if (GetTickCount() - g_lockWaitTick >= 1000)
    {
        g_lockWaitShown = g_lockWaitMax;
        g_lockWaitMax = 0;
        g_lockWaitTick = GetTickCount();
    }

    // El tiempo "sin salida" solo cuenta mientras NOSOTROS esperamos
    // cuadros. Si la fila de listos esta llena (cuadros que aun no les toca,
    // por ejemplo tras un reinicio que salto hacia adelante), no pedimos
    // mas y eso no es un atasco del decodificador.
    EnterCriticalSection(&g_frameLock);
    full = g_readyN >= g_readyCap;
    LeaveCriticalSection(&g_frameLock);
    if (full)
        g_lastOutTick = GetTickCount();

    diag_track(clock);

    // Vigilante: 2 s esperando cuadros sin que salga nada, con datos en la
    // cola y sin que el decodificador este pidiendo mas -> reinicio (ver
    // restart_decoder). Tras un reinicio, otro solo si ya salio algun
    // cuadro o pasaron 6 s.
    if (g_decoded > 0 && g_qCount > 0 && !g_readWaiting &&
        GetTickCount() - g_lastOutTick > 2000 &&
        (g_outSinceRestart > 0 || GetTickCount() - g_restartTick > 6000))
    {
        DWORD t0;

        diag_restart_begin(clock);
        t0 = GetTickCount();
        restart_decoder();
        diag_restart_end(GetTickCount() - t0);
        if (!g_dec)
            return;
    }

    // Elegir el cuadro de este refresco (con la fila protegida; las
    // devoluciones se hacen despues, ya sin el candado de la fila)
    nd = 0;
    haveShow = 0;
    EnterCriticalSection(&g_frameLock);
    if (!g_haveCur && g_readyN > 0)
    {
        // sin cuadro en pantalla (al empezar o tras un reinicio): se muestra
        // el primero que haya aunque todavia no sea su hora (no queda negro)
        show = g_ready[0];
        haveShow = 1;
        pop_ready();
    }
    else if (clock >= 0.0)
    {
        // tira los que ya pasaron (solo si el siguiente tambien ya va tarde)
        while (g_readyN > 1 && g_ready[1].pts + DROP_LATE <= clock)
        {
            drops[nd++] = g_ready[0].handle;
            pop_ready();
        }
        if (g_readyN > 0 && g_ready[0].pts <= clock + DUE_AHEAD)
        {
            show = g_ready[0];
            haveShow = 1;
            pop_ready();
        }
    }
    LeaveCriticalSection(&g_frameLock);

    for (i = 0; i < nd; ++i)
    {
        give_back(drops[i]);
        ++g_dropped;
    }
    if (haveShow)
        set_current(&show);
}

extern "C" int gv_draw(float x, float y, float w, float h)
{
    float screen[4] = { 2.0f / 1280.0f, -2.0f / 720.0f, 0.0f, 0.0f };
    QuadVert q[4];
    float u0, u1, v0, v1;
    int i;

    if (!g_dec || !g_haveCur)
        return 0;

    u0 = (float)g_cur.cropX / g_cur.pitchY;
    u1 = (float)(g_cur.cropX + g_cur.showW) / g_cur.pitchY;
    v0 = (float)g_cur.cropY / g_cur.heightY;
    v1 = (float)(g_cur.cropY + g_cur.showH) / g_cur.heightY;

    q[0].x = x;     q[0].y = y;     q[0].u = u0; q[0].v = v0;
    q[1].x = x + w; q[1].y = y;     q[1].u = u1; q[1].v = v0;
    q[2].x = x;     q[2].y = y + h; q[2].u = u0; q[2].v = v1;
    q[3].x = x + w; q[3].y = y + h; q[3].u = u1; q[3].v = v1;

    g_dev->SetVertexDeclaration(g_decl);
    g_dev->SetVertexShader(g_vs);
    g_dev->SetPixelShader(g_ps);
    g_dev->SetVertexShaderConstantF(0, screen, 1);
    for (i = 0; i < 3; ++i)
    {
        g_dev->SetTexture(i, &g_tex[i]);
        g_dev->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(i, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(i, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    }
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVert));
    for (i = 0; i < 3; ++i)
        g_dev->SetTexture(i, NULL);

    // la GPU marca esta cerca cuando termine de leer el cuadro
    g_curFence = g_dev->InsertFence();
    g_curDrawn = 1;
    return 1;
}

extern "C" void gv_get_stats(GvStats *s)
{
    long waiting = -1;

    if (g_dec && *(DWORD *)g_dec == 0x20000420)
        waiting = *(long *)((BYTE *)g_dec + 0x63E4);

    s->fed      = g_fed;
    s->decoded  = g_decoded;
    s->shown    = g_shown;
    s->dropped  = g_dropped;
    s->inFlight = g_fed - g_taken;
    s->queued   = g_readyN;
    s->backlog  = waiting;
    s->firstPts = g_firstPts;
    s->curPts   = g_haveCur ? g_cur.pts : -1.0;
    s->width    = g_haveCur ? (int)g_cur.showW : 0;
    s->lockMiss = g_lockWaitShown;
    s->held     = g_readyN + g_haveCur + g_retireN + g_retN;
    s->inQueue  = g_qCount;
    s->reads    = g_reads;
    s->readWaiting = g_readWaiting;
    s->sinceOutMs = g_dec ? (long)(GetTickCount() - g_lastOutTick) : 0;
    s->kicks    = g_restarts;
    s->height   = g_haveCur ? (int)g_cur.showH : 0;
}

static long backlog_now(void)
{
    if (g_dec && *(DWORD *)g_dec == 0x20000420)
        return *(long *)((BYTE *)g_dec + 0x63E4);
    return -1;
}

// Pasos 1-4 del cierre: devuelve todo y cierra el decodificador, pero deja
// la cola de entrada (el reinicio la reutiliza).
static void close_decoder(void)
{
    DWORD start, t0, fence;
    int i, r;
    char msg[128];

    // 0. el hilo recolector se detiene antes que nada
    stop_taker();

    // 1. ya no se entregan cuadros nuevos
    g_feeding = 0;

    // 2. espera a que la GPU termine de dibujar y devuelve todos los cuadros
    fence = g_dev->InsertFence();
    t0 = GetTickCount();
    while (g_dev->IsFencePending(fence) && GetTickCount() - t0 < 200)
        Sleep(1);
    for (i = 0; i < g_retireN; ++i)
        give_back(g_retire[i].f.handle);
    g_retireN = 0;
    if (g_haveCur)
        give_back(g_cur.handle);
    g_haveCur = 0;
    for (i = 0; i < g_readyN; ++i)
        give_back(g_ready[i].handle);
    g_readyN = 0;
    motor_enter();
    flush_returns();
    motor_leave();

    // 3. los hilos del decodificador dejan de esperar datos
    g_closing = 1;
    SetEvent(g_dataEv);

    // 4. deja que la GPU vacie el trabajo pendiente del decodificador
    //    (hasta 3 s), y luego lo cierra
    start = GetTickCount();
    while (backlog_now() > 0 && GetTickCount() - start < 3000)
    {
        for (i = 0; i < 16; ++i)
            D3DDevice_InsertYieldPoint((D3DDevice *)g_dev, 64, 0);
        fence = g_dev->InsertFence();
        t0 = GetTickCount();
        while (g_dev->IsFencePending(fence) && GetTickCount() - t0 < 50)
            Sleep(1);
    }
    r = H264DecodeClose(g_dec);
    g_dec = NULL;

    _snprintf(msg, sizeof(msg) - 1, "Decodificador cerrado (%d) en %lu ms", r,
              (unsigned long)(GetTickCount() - start));
    msg[sizeof(msg) - 1] = 0;
    set_status(msg);
}

extern "C" void gv_close(void)
{
    if (!g_dec)
        return;
    diag_summary();
    close_decoder();
    InterlockedIncrement(&g_session);   // los hilos del siguiente video se nombran de nuevo

    // 5. limpia la cola de entrada
    EnterCriticalSection(&g_qLock);
    while (g_qCount > 0)
    {
        free(g_queue[g_qHead].data);
        g_qHead = (g_qHead + 1) % QUEUE_CAP;
        --g_qCount;
    }
    if (g_stage)
        free(g_stage);
    g_stage = NULL;
    g_pendN = 0;
    LeaveCriticalSection(&g_qLock);

    if (g_outEv)
        CloseHandle(g_outEv);
    g_outEv = NULL;
    g_closing = 0;
    g_clock = -1.0;
}


// ---------------------------------------------------------------------------
// Diagnostico: API

extern "C" void gv_set_test_mode(int mode)
{
    if (mode < 0 || mode >= GV_TEST_MODES)
        mode = 0;
    g_testMode = mode;
}

extern "C" int gv_test_mode(void)
{
    return g_testMode;
}

extern "C" const char *gv_test_mode_name(int mode)
{
    static const char *names[GV_TEST_MODES] = { "normal", "6 hilos", "4 hilos", "2 hilos", "audio sigue" };
    return (mode >= 0 && mode < GV_TEST_MODES) ? names[mode] : "?";
}

extern "C" void gv_log(const char *text)
{
    diag_append_file(text);
}

extern "C" void gv_diag_note(char type, double pts, const char *title, const char *detail)
{
    if (type == 'C')
        ++g_notes;
    InterlockedIncrement(&tl_now()->notes);
    add_event(type, pts);
    dcat_begin(pts);
    dcat("%s\n", title);
    if (detail && detail[0])
        dcat("    %s\n", detail);
    dcat_threads("ahora");
    dcat_context();
    if (g_testMode == 4)
    {
        // prueba 4: no escribir ahora (la escritura podria "despertar" al
        // decodificador); se escribe con el siguiente evento
        if (g_noteLen + g_dlen < (int)sizeof(g_noteBuf) - 1)
        {
            memcpy(g_noteBuf + g_noteLen, g_dbuf, g_dlen + 1);
            g_noteLen += g_dlen;
        }
    }
    else
        diag_append_file(g_dbuf);
    _snprintf(g_lastEvent, sizeof(g_lastEvent) - 1, "%s  %s", title, detail ? detail : "");
    g_lastEvent[sizeof(g_lastEvent) - 1] = 0;
}

static void name_thread(const char *name, LONG session)
{
    LONG i = InterlockedIncrement(&g_namedN) - 1;
    NamedThread *t = &g_named[i % NAMED_N];

    t->id = GetCurrentThreadId();
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = 0;
    t->session = session;
}

extern "C" void gv_diag_thread_name(const char *name)
{
    name_thread(name, g_session);
}

extern "C" void gv_set_diag_hook(void (*fn)(char *buf, int size))
{
    g_diagHook = fn;
}

extern "C" void gv_diag_panel(char *l1, char *l2, char *l3, int size)
{
    int i, n, pos, fit;
    long first, t;
    const DiagEvent *e;

    // cada linea cabe en 96 letras (ancho del panel)
    _snprintf(l1, size - 1, "Diagn\xC3\xB3stico, prueba %d (%s): %ld reinicios, %ld pausas, %ld recargas, "
              "%ld huecos de 0.3-2 s", g_testMode, gv_test_mode_name(g_testMode),
              g_restarts, g_pauses, g_notes, g_gapHist[2] + g_gapHist[3] + g_gapHist[4]);
    l1[size - 1] = 0;

    // los mas recientes que quepan
    pos = _snprintf(l2, size - 1, "Atascos (R reinicio, P pausa, C recarga):");
    if (pos < 0)
        pos = 0;
    fit = (94 - pos) / 7;
    n = g_eventTotal < DIAG_EVENTS ? (int)g_eventTotal : DIAG_EVENTS;
    if (n > fit)
        n = fit;
    first = g_eventTotal - n;
    if (n == 0)
        pos += _snprintf(l2 + pos, size - 1 - pos, " ninguno");
    for (i = 0; i < n && pos >= 0 && pos < size - 12; ++i)
    {
        e = &g_events[(first + i) % DIAG_EVENTS];
        t = e->pts < 0.0 ? 0 : (long)e->pts;
        pos += _snprintf(l2 + pos, size - 1 - pos, " %ld:%02ld%c", t / 60, t % 60, e->type);
    }
    l2[size - 1] = 0;

    if (g_lastEvent[0])
        _snprintf(l3, size - 1, "\xC3\x9Altimo: %s", g_lastEvent);
    else
        _snprintf(l3, size - 1, "Todo se anota en diagnostico.txt (carpeta del .xex)");
    l3[size - 1] = 0;
}

#ifdef GV_SIM
/* solo para la simulacion en PC (punteros de 64 bits) */
extern "C" void *gv_sim_read_cb(void) { return (void *)read_cb; }
#endif
