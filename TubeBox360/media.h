/* media.h - Descarga y analisis de las pistas de video y audio. C89.
 *
 * Paso 4b: para el formato de video elegido (H.264) y un audio AAC, baja
 * el encabezado y el indice (initRange..indexRange) y el primer fragmento,
 * y los lee con fmp.c. Es la base del reproductor de la 4c. */
#ifndef MEDIA_H
#define MEDIA_H

#include "yt.h"
#include "fmp.h"
#include "net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int         fmtIndex;     /* posicion en YtPlayer.formats, -1 si no hay */
    int         itag;
    int         ok;
    char        err[160];
    FmpTrack    track;
    FmpFragment frag;
    unsigned    bytes;        /* bytes descargados para esta pista */
} MediaStream;

typedef struct
{
    MediaStream video;
    MediaStream audio;
    unsigned    totalBytes;
} MediaProbe;

/* Idioma de audio predeterminado de la app (5d). Si el video tiene una
 * pista en este idioma (original o doblada) se usa esa; si no, la original. */
#define APP_AUDIO_LANG lang_code()   /* idioma de la app (lang.h) */

/* Configuraciones predeterminadas (Configuracion, se guardan en config.ini) */
enum { PREF_AUDIO_APP, PREF_AUDIO_ORIGINAL, PREF_AUDIO_ES, PREF_AUDIO_EN, PREF_AUDIO_PT, PREF_AUDIO_JA, PREF_AUDIO_AR,
       PREF_AUDIO_COUNT };
enum { PREF_SUBS_AUTO, PREF_SUBS_OFF, PREF_SUBS_ES, PREF_SUBS_EN, PREF_SUBS_PT, PREF_SUBS_JA, PREF_SUBS_AR,
       PREF_SUBS_COUNT };
extern int g_prefAudio;   /* PREF_AUDIO_*: idioma de la app (predeterminado) */
extern int g_prefSubs;    /* PREF_SUBS_*: automaticos (predeterminado) */

/* Pista con la que empieza cada video (ver APP_AUDIO_LANG y g_prefAudio). Devuelve la
 * posicion en p->tracks, o -1 si el video tiene una sola pista. */
int  media_default_track(const YtPlayer *p);

/* Elige el audio de la pista dada (-1 = la unica): itag 140 (AAC 128 kbps)
 * si existe, si no el primer AAC de esa pista. -1 si no hay. */
int  media_pick_audio(const YtPlayer *p, int track);

/* Subtitulos con los que empieza el video (posicion en p->captions, o -1 =
 * apagados): apagados, salvo que el audio elegido (track) no sea en
 * APP_AUDIO_LANG y haya subtitulos en ese idioma. */
int  media_default_caption(const YtPlayer *p, int track);

/* Nombre para mostrar de la pista ("Ingles (US) - doblaje automatico").
 * Con track -1: "Pista unica". */
void media_track_name(const YtPlayer *p, int track, char *out, int size);

/* Analiza el video p->formats[videoIdx] y el audio elegido. Devuelve 1 si
 * las dos pistas salieron bien. Cada pista guarda su propio error. */
int  media_probe(const YtPlayer *p, int videoIdx, MediaProbe *m, char *err, int errSize);

void media_free(MediaProbe *m);

/* --- Para el reproductor (4c) --- */

/* GET con Range: bytes=start-end (incluye end). Devuelve 1 si llego 206/200;
 * liberar r con http_free. */
int  media_fetch_range(const char *url, double start, double end, HttpResponse *r,
                       char *err, int errSize);

/* Baja encabezado + indice de un formato y los lee. bytes (opcional) suma
 * lo descargado. Devuelve 1 si salio bien; liberar t con fmp_free. */
int  media_load_init(const YtFormat *f, FmpTrack *t, unsigned *bytes, char *err, int errSize);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_H */
