/* thumb.h - Miniaturas de los videos (paso 5a). C89.
 *
 * Uso: al dibujar, thumb_draw(id, ...) con el id del video. Si la
 * miniatura no esta, la pide y dibuja un recuadro gris mientras llega.
 *
 * Por dentro:
 *   - 3 hilos la descargan de https://i.ytimg.com/vi/<id>/mqdefault.jpg
 *     (320x180, sin barras negras) a traves del proxy. Primero las que se
 *     pidieron mas recientemente y, entre esas, en el orden en que se
 *     dibujan (la fila de arriba primero).
 *   - El hilo principal convierte el JPEG en textura con D3DX, como mucho
 *     una por cuadro para que la pantalla no se trabe.
 *   - Se guardan las ultimas THUMB_SLOTS (96: miniaturas, avatares y
 *     banners). Cuando se llenan se recicla la que lleva mas tiempo sin
 *     dibujarse.
 *
 * Todas las funciones se llaman desde el hilo principal. Durante el video
 * (thumb_set_active(0)) solo se bajan avatares. */
#ifndef THUMB_H
#define THUMB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Arranca los hilos de descarga. Llamar despues de net_init. 1 = bien. */
int  thumb_init(void);

/* Una vez por cuadro, antes de dibujar. */
void thumb_frame(void);

/* 0 mientras se reproduce un video (no descarga miniaturas para no
 * quitarle red ni CPU al video), 1 al volver a las listas. */
void thumb_set_active(int on);

/* Dibuja la miniatura del video en el rectangulo (o el recuadro gris si
 * aun no llega). Devuelve 1 si dibujo la imagen. */
int  thumb_draw(const char *id, float x, float y, float w, float h);

/* La pide sin dibujarla (para las filas que estan por aparecer). */
void thumb_prefetch(const char *id);

/* --- Avatares y banners de canales --- */

enum { THUMB_VIDEO, THUMB_AVATAR, THUMB_BANNER };

/* Avatar circular de size x size. url = la del avatar (de YtVideo o
 * YtChannel). Mientras llega, o si no hay url, un circulo gris con la
 * inicial de name. Devuelve 1 si dibujo la imagen. */
int  thumb_draw_avatar(const char *url, float x, float y, float size, const char *name);
void thumb_prefetch_avatar(const char *url);

/* Colores de los recuadros mientras llegan las imagenes (tema claro u
 * oscuro): miniaturas y banner, circulo del avatar, y su inicial. */
void thumb_set_colors(unsigned int holder, unsigned int avatar, unsigned int letter);

/* Banner del canal en el recuadro (recortado arriba y abajo para no
 * deformarlo). */
int  thumb_draw_banner(const char *url, float x, float y, float w, float h);

/* Para el reporte de pruebas: si D3DX no pudo leer algun JPEG, o si la
 * ultima descarga fallo, escribe el motivo en out y devuelve 1. Si todo
 * va bien devuelve 0. */
int  thumb_status(char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* THUMB_H */
