/* gpuvid.h - Decodificador H.264 por GPU de la Xbox 360 (interfaz en C).
 *
 * La consola trae un decodificador H.264 que usa la GPU (lo usa el
 * reproductor de video del dashboard). Vive en dash.ExtraAVCodecs.xex y
 * se carga con xav.lib. Microsoft no lo documento: las funciones, el
 * orden de las llamadas y los campos de su configuracion los descubrio
 * HG-ERIK en su proyecto GPU-MEDIA-PLAYER-360 (GPL-3.0). Este modulo es
 * codigo propio de TubeBox360 escrito a partir de esos datos.
 *
 * Como se usa:
 *   gv_init()  una vez, despues de gfx_init().
 *   gv_open()  al empezar un video.
 *   gv_feed()  desde un hilo: un cuadro Annex B (ver avc.h) por llamada.
 *   En cada cuadro de pantalla: gv_update(reloj) y gv_draw().
 *   gv_close() al terminar (y siempre antes de salir de la app).
 *
 * Mientras esta abierto hay que usar gfx_set_swap_mode(1). */
#ifndef GPUVID_H
#define GPUVID_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    long   fed;          /* cuadros entregados al decodificador */
    long   decoded;      /* cuadros que ya salieron decodificados */
    long   shown;        /* cuadros mostrados */
    long   dropped;      /* cuadros tirados por llegar tarde */
    long   inFlight;     /* entregados que aun no salen */
    int    queued;       /* decodificados esperando su turno */
    long   backlog;      /* trabajos pendientes dentro del decodificador (-1 si no se sabe) */
    double firstPts;     /* tiempo del primer cuadro decodificado, -1 si aun no hay */
    double curPts;       /* tiempo del cuadro en pantalla, -1 si no hay */
    int    width, height;    /* tamaño visible del cuadro */
    long   lockMiss;         /* espera mas larga por el candado en el ultimo segundo (ms) */
    int    held;             /* cuadros de salida que tenemos prestados (de 8 o 12) */
    int    inQueue;          /* cuadros en nuestra cola de entrada */
    long   reads;            /* cuadros que el decodificador ya leyo */
    int    readWaiting;      /* 1 si el decodificador esta esperando datos */
    long   sinceOutMs;       /* ms desde el ultimo cuadro decodificado */
    long   kicks;            /* reinicios automaticos por atasco */
} GvStats;

/* device: IDirect3DDevice9*, pp: D3DPRESENT_PARAMETERS* (ver gfx.h).
 * Devuelve 1 si el decodificador esta disponible. */
int  gv_init(void *device, void *pp);

int  gv_available(void);
int  gv_motor_lock(void);         /* 1 si se encontro el candado del decodificador */
const char *gv_status(void);      /* texto corto con el resultado de gv_init */

/* level: nivel H.264 del video (30 = 3.0). Devuelve 1 si abrio. */
int  gv_open(int level);
int  gv_is_open(void);

/* Datos de un cuadro para el diagnostico (de que parte del archivo viene) */
typedef struct
{
    int  frag;           /* fragmento del archivo (1 = el primero) */
    int  sample;         /* cuadro dentro del fragmento (0 = el primero) */
    char desc[28];       /* tipo y NALs, ver avc_describe_sample() */
} GvAuInfo;

/* Entrega un cuadro (se copia). pts en segundos; key = 1 si es cuadro clave;
 * info puede ser NULL. Devuelve 1 si lo tomo, 0 si hay que esperar (la cola
 * de entrada esta llena) y -1 si el decodificador esta cerrado. */
int  gv_feed(const unsigned char *annexb, int len, double pts, int key, const GvAuInfo *info);

/* En cada cuadro de pantalla. clock = segundo del video que deberia
 * verse ahora, o -1 si el reloj aun no arranca (entonces solo se prepara
 * el primer cuadro). */
void gv_update(double clock);

/* Dibuja el cuadro actual en el rectangulo dado (pixeles de 1280x720).
 * Llamar entre gfx_begin y gfx_end, despues de gfx_flush(); al terminar
 * llamar gfx_reset_state(). Devuelve 1 si habia cuadro. */
int  gv_draw(float x, float y, float w, float h);

void gv_get_stats(GvStats *s);

/* Cierra el decodificador. Puede tardar hasta ~3 s. */
void gv_close(void);

/* ---- Diagnostico de atascos (4f, paso 1) ----
 * Cada atasco (pausa que se recupera sola o reinicio) y cada recarga se
 * anota en game:\diagnostico.txt (la carpeta del .xex) con el estado del
 * decodificador y los ultimos cuadros que leyo. */

/* Modos de prueba. Se aplican al abrir el siguiente video.
 *   0 normal (5 hilos del decodificador, en HW 0-4; el HW 5 es nuestro)
 *   1 = 6 hilos (como antes: uno cae en el HW 5)   2 = 4 hilos   3 = 2 hilos
 *   4 = 5 hilos y, si se acaban los cuadros, el audio sigue (play.c no pausa
 *       el reloj); la nota de la recarga se escribe al terminar la pausa */
#define GV_TEST_MODES 5
void gv_set_test_mode(int mode);
int  gv_test_mode(void);
const char *gv_test_mode_name(int mode);

/* Nombre del hilo que llama (sale en la lista de hilos del diagnostico) */
void gv_diag_thread_name(const char *name);

/* Agrega texto tal cual al archivo de diagnostico (y a la salida de depuracion) */
void gv_log(const char *text);

/* Anota un evento que gpuvid no ve (por ejemplo una recarga de play.c).
 * type: letra para el panel ('C' = recarga). pts: segundo del video. */
void gv_diag_note(char type, double pts, const char *title, const char *detail);

/* Funcion que escribe una linea de contexto (descarga, audio...) para
 * agregarla a cada evento. NULL para quitarla. */
void gv_set_diag_hook(void (*fn)(char *buf, int size));

/* Tres lineas cortas para el panel de datos (UTF-8) */
void gv_diag_panel(char *l1, char *l2, char *l3, int size);

#ifdef __cplusplus
}
#endif

#endif /* GPUVID_H */
