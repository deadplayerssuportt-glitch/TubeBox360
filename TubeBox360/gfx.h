/* gfx.h - Interfaz en C de la capa de graficos.
 * La implementacion (gfx.cpp) usa Direct3D 9 de Xbox 360, que en el XDK
 * esta hecho para C++. Todo lo demas del proyecto llama solo a estas
 * funciones y se queda en C puro.
 *
 * Coordenadas en pixeles sobre una pantalla de 1280x720.
 * Colores en formato 0xAARRGGBB.
 * El texto se recibe en UTF-8 (acentos, ñ, ¿ y ¡ funcionan). */
#ifndef GFX_H
#define GFX_H

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_W   1280
#define SCREEN_H   720
#define GFX_CHAR_W 12   /* ancho de un caracter monoespaciado a escala 1.0 */
#define GFX_CHAR_H 24   /* alto de una linea a escala 1.0 */

/* Crea el dispositivo, compila los shaders y arma la textura de la fuente.
 * Devuelve 1 si todo salio bien, 0 si fallo (el detalle sale en la
 * ventana Output de Visual Studio). */
int   gfx_init(void);

/* Inicia un cuadro: limpia la pantalla con el color dado. */
void  gfx_begin(unsigned int clearColor);

/* Rectangulo solido (acepta transparencia en el canal alfa). */
void  gfx_rect(float x, float y, float w, float h, unsigned int color);

/* Rectangulo con degradado vertical (color de arriba a color de abajo). */
void  gfx_rect_vgrad(float x, float y, float w, float h, unsigned int top, unsigned int bottom);

/* Dibuja texto con la letra de la interfaz (Roboto; si no cargo, la
 * monoespaciada) y devuelve la x donde termino. y = arriba de la linea;
 * escala 1.0 = linea de 24 px. */
float gfx_text(float x, float y, float scale, unsigned int color, const char *utf8);

/* Ancho en pixeles que ocuparia el texto. */
float gfx_text_width(float scale, const char *utf8);

/* Lo mismo con la letra monoespaciada (12x24 a escala 1.0): para tablas
 * con columnas y paneles de datos. */
float gfx_text_mono(float x, float y, float scale, unsigned int color, const char *utf8);
float gfx_text_mono_width(float scale, const char *utf8);

/* gfx_font_bold(1): gfx_text, gfx_text_width, gfx_fit_text y
 * gfx_wrap_text usan Roboto Bold (hecha para escala 1.58 = 32 px) hasta
 * gfx_font_bold(0). Sin ella se sigue con la normal. */
void  gfx_font_bold(int on);

/* Copia en dst lo que quepa de src en maxW pixeles (con "..." al final si
 * se corta). Con la letra de la interfaz. */
void  gfx_fit_text(const char *src, float maxW, float scale, char *dst, int dstSize);

/* Parte el texto en lineas de maxW pixeles: copia en line la primera
 * (cortando en un espacio) y devuelve donde sigue (cadena vacia al final). */
const char *gfx_wrap_text(const char *src, float maxW, float scale, char *line, int lineSize);

/* Icono de ui_assets.h (UI_ICON_...) en un cuadro de size x size; color lo
 * tine (0xFFFFFFFF = tal cual). Devuelve 0 si los iconos no cargaron. */
int   gfx_icon(int id, float x, float y, float size, unsigned int color);

/* Logo de TubeBox360 de alto h. Devuelve el ancho dibujado (0 si no cargo). */
float gfx_logo(float x, float y, float h);

/* El logo grande (pantalla de carga) y el ancho que ocupa cualquiera de
 * los dos a alto h (big = 1 el grande). */
float gfx_logo_big(float x, float y, float h);
float gfx_logo_width(int big, float h);

/* Si la letra, los iconos o el logo no cargaron, el motivo ("" si todo bien) */
const char *gfx_assets_error(void);

/* Termina el cuadro y lo muestra (espera al refresco vertical). */
void  gfx_end(void);

/* --- Imagenes (paso 5a) --- */

/* Textura desde un archivo de imagen en memoria (JPEG, PNG...), escalada
 * a w x h. Devuelve NULL si no se pudo; el HRESULT queda en *hrOut (puede
 * ser NULL). Solo desde el hilo principal. */
void *gfx_texture_load(const void *data, int len, int w, int h, long *hrOut);

/* Libera la textura. Solo si no se dibujo en los ultimos cuadros (la GPU
 * puede ir atrasada). */
void  gfx_texture_free(void *tex);

/* Textura de w x h desde pixeles 0xAARRGGBB (fila tras fila). NULL si falla. */
void *gfx_texture_from_argb(const unsigned int *px, int w, int h);

/* Dibuja la textura completa en el rectangulo dado. color multiplica la
 * imagen: 0xFFFFFFFF = tal cual; el alfa sirve para aparecerla poco a poco. */
void  gfx_image(void *tex, float x, float y, float w, float h, unsigned int color);

/* Solo una parte de la textura (u0,v0)-(u1,v1), de 0 a 1. Para recortar
 * banners. */
void  gfx_image_uv(void *tex, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1, unsigned int color);

/* La textura recortada en circulo (avatares de canales), de size x size. */
void  gfx_image_circle(void *tex, float x, float y, float size, unsigned int color);

/* Circulo de un color (de size x size). */
void  gfx_circle(float x, float y, float size, unsigned int color);

/* Recorte: lo que se dibuje despues solo aparece dentro del rectangulo
 * (para listas que se desplazan). gfx_clip_off lo quita. */
void  gfx_clip(float x, float y, float w, float h);
void  gfx_clip_off(void);

/* Elipse de un color dentro del rectangulo (x, y, w, h), orilla suave. */
void  gfx_ellipse(float x, float y, float w, float h, unsigned int color);

/* La textura recortada en elipse (como gfx_image_circle pero w x h). */
void  gfx_image_ellipse(void *tex, float x, float y, float w, float h, unsigned int color);

/* Arco de aro con orilla suave (centro cx,cy; radio al centro del trazo;
 * grosor thick). Angulos en radianes (0 = derecha, crece en el sentido del
 * reloj porque y va hacia abajo). El color pasa de c0 (en a0) a c1 (en a1):
 * sirve para la cola que se desvanece de la ruedita de carga. */
void  gfx_arc(float cx, float cy, float radius, float thick, float a0, float a1,
              unsigned int c0, unsigned int c1);

/* --- 3D dentro de un rectangulo (el avatar del menu) ---
 * gfx_begin_3d manda lo pendiente, pone un buffer de profundidad (se crea
 * la primera vez, en la EDRAM despues de la pantalla), el viewport en el
 * rectangulo y borra la profundidad. 0 si no se pudo. gfx_end_3d deja todo
 * como lo usa el resto de gfx. */
int   gfx_begin_3d(float x, float y, float w, float h);
void  gfx_end_3d(void);

/* Igual pero con antialias: se dibuja en una superficie aparte de w x h con
 * MSAA 4x (o 2x si no cabe en la EDRAM), con fondo transparente.
 * gfx_end_3d_aa la reduce a una textura (orillas suaves) y la devuelve;
 * se pone en pantalla con gfx_image_premul. 0 / NULL si no se pudo. */
int   gfx_begin_3d_aa(int w, int h);
void *gfx_end_3d_aa(void);
int   gfx_aa_samples(void);          /* 4, 2 o 0 */

/* Textura con el color ya multiplicado por su alfa (la del 3D con antialias) */
void  gfx_image_premul(void *tex, float x, float y, float w, float h);

/* Espera a que el GPU termine y cuenta pixeles de la textura: con alfa
 * (> 0x80) y con color (no negros). Para revisar una sola vez. */
void  gfx_texture_count(void *tex, int w, int h, int *withAlpha, int *withColor);

/* --- Para el reproductor (paso 4c) --- */

/* IDirect3DDevice9* y D3DPRESENT_PARAMETERS* (como void* para C) */
void *gfx_device(void);
void *gfx_present_params(void);

/* Antes de que otro modulo dibuje con sus propios shaders: manda a la
 * GPU lo que esta pendiente. Despues: gfx_reset_state() deja todo listo
 * para seguir con gfx_rect/gfx_text. */
void  gfx_flush(void);
void  gfx_reset_state(void);

/* 1 = modo video (Resolve + Swap en buffers propios), 0 = Present normal.
 * Devuelve cuantos buffers propios hay (3; 0 si no se pudieron crear y
 * se usa el de D3D). */
int   gfx_set_swap_mode(int on);

/* Cuantos refrescos verticales van (solo cuenta despues de activar el modo video) */
long  gfx_vblank_count(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
