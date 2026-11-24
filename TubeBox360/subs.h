/* subs.h - Subtitulos durante el video (5d parte 2). C89.
 *
 * subs_load baja los subtitulos de YouTube en formato json3 en un hilo
 * aparte y los convierte en una lista de textos con su tiempo. Mientras
 * tanto el video sigue. subs_draw dibuja los que tocan en el segundo dado,
 * abajo y centrados, con fondo semitransparente.
 *
 * Los subtitulos automaticos de YouTube van "rodando": cada linea nueva
 * aparece mientras la anterior sigue en pantalla. Por eso se muestran hasta
 * las 2 ultimas que esten vigentes, como en YouTube.
 *
 * Todas las funciones son del hilo principal. */
#ifndef SUBS_H
#define SUBS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Empieza a bajar los subtitulos de esa URL (la de YtCaption; se le
 * agrega &fmt=json3). Quita los que hubiera. */
void subs_load(const char *url);

/* Quita los subtitulos (y descarta una descarga en curso). */
void subs_clear(void);

/* 0 sin subtitulos, 1 bajando, 2 listos, -1 fallo (motivo en subs_error) */
int  subs_state(void);
const char *subs_error(void);

/* Dibuja los subtitulos del segundo sec. bottom = donde termina la ultima
 * linea (mas arriba si la barra de progreso esta a la vista). */
void subs_draw(double sec, float bottom);

/* Solo para pruebas: llena out con las lineas de ese segundo (separadas
 * por \n). Devuelve cuantas lineas. */
int  subs_text_at(double sec, char *out, int size);

/* Solo para pruebas: convierte un json3 ya descargado (modifica json).
 * Devuelve cuantos textos tiene, o -1 si no se pudo leer. */
int  subs_parse_for_test(char *json);

#ifdef __cplusplus
}
#endif

#endif /* SUBS_H */
