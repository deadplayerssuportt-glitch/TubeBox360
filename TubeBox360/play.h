/* play.h - Pantalla de reproduccion (video por la GPU + audio AAC). C89.
 *
 * Tres hilos trabajan juntos:
 *   - descarga: baja el encabezado y luego los fragmentos, hasta 3 adelante.
 *   - alimentador: parte cada fragmento en cuadros, los pasa a Annex B
 *     (avc.c) y se los da al decodificador de la GPU (gpuvid.cpp).
 *   - audio: baja el AAC, lo decodifica (ffaac/aac.c) y lo manda a XAudio2.
 *   - principal: lleva el reloj (el del audio si lo hay), elige el cuadro
 *     que toca y lo dibuja.
 *
 * Diagnostico de atascos (4f): al empezar cada video se anota en
 * game:\diagnostico.txt el video y su SPS/PPS; gpuvid.cpp anota cada atasco
 * y play.c cada recarga, con el estado de la descarga y del audio. */
#ifndef PLAY_H
#define PLAY_H

#include "yt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Empieza a reproducir el formato de video (H.264 con initRange/indexRange)
 * y, si audio no es NULL, ese audio AAC (itag 140) junto con el.
 * Siempre entra a la pantalla; si algo falla, el error se ve ahi. */
void play_start(const YtFormat *f, const YtFormat *audio, const char *title, double lengthSec);

/* 1 mientras la pantalla de reproduccion esta activa */
int  play_active(void);

/* En cada cuadro, antes de dibujar */
void play_update(void);

/* Dibuja todo (entre gfx_begin y gfx_end) */
void play_draw(void);

/* Botones recien presionados. La capa de controles (estilo YouTube TV):
 *   - escondida: A pausa y la muestra, izq/der +-10 s, B sale del video;
 *   - a la vista: la cruceta pasa por los botones y la barra, A elige,
 *     B la esconde. Arriba a la izquierda: cerrar. Al centro, encima de la
 *     barra: -10 s, pausa, +10 s. Fila de abajo: calidad, canal,
 *     subtitulos, idioma del audio, informacion.
 *   - siempre: LB/RB +-1 min, Y siguiente pista de audio, X datos.
 * Devuelve que debe hacer quien llama (PLAY_ACT_...). */
enum { PLAY_ACT_NONE, PLAY_ACT_STOP, PLAY_ACT_CHANNEL };
int  play_input(unsigned short pressed);

/* Datos del video para la capa (canal, vistas, fecha, avatar). Antes de
 * play_start. */
void play_set_info(const YtVideo *v);

/* Calidades que se pueden elegir dentro del video (todas las compatibles):
 * current = la que se pasa a play_start, def = la predeterminada (se marca
 * en rojo en el menu). Antes de play_start. */
void play_set_qualities(const YtFormat **fmts, const char **names, int count, int current, int def);

/* Pistas de audio que se pueden cambiar con Y durante el video (5d):
 * audios[i] es el formato AAC de cada pista y names[i] su nombre;
 * current = la que suena al empezar (la que se pasa a play_start).
 * Llamar antes de play_start. Con count 0 o 1, Y no hace nada.
 * Al cambiar, el video vuelve a empezar desde el inicio del fragmento
 * actual (se repiten unos segundos), igual que al buscar. */
void play_set_tracks(const YtFormat **audios, const char **names, int count, int current);

/* Pista que estaba sonando (la pantalla de formatos la recuerda al volver). */
int  play_current_track(void);

/* Subtitulos que se pueden cambiar con la cruceta abajo durante el video
 * (5d parte 2). current = los que se ven al empezar (-1 = apagados).
 * Empieza a bajarlos de una vez. Llamar antes de play_start; al salir del
 * video, quien llamo debe usar subs_clear(). */
void play_set_captions(const YtCaption *caps, int count, int current);

/* Subtitulos que se estaban viendo (-1 = apagados). */
int  play_current_caption(void);

/* 1 cuando el video llego al final (sin error). */
int  play_ended(void);

/* Esconde la capa de controles, el menu y el panel de datos (para la
 * cuenta regresiva del siguiente video). */
void play_hide_overlay(void);

/* Detiene todo y cierra el decodificador. Puede tardar unos segundos. */
void play_stop(void);

/* Texto que se ve (con la ruedita) mientras play_stop espera a los hilos */
void play_set_wait_msg(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* PLAY_H */
