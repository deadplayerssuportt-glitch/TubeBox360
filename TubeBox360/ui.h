/* ui.h - Piezas de la interfaz que comparten las pantallas. C89.
 *
 *   - Tema oscuro/claro (colores COL_...).
 *   - Ayudas de botones con el icono de cada boton del control
 *     ("A=Ver|B=Volver").
 *   - Rectangulos redondeados.
 *   - Perfil de la consola: gamertag y foto de jugador (gamer picture).
 *   - Menu lateral estilo YouTube TV: angosto con iconos y, al entrar en
 *     el, desplegado en rojo con los nombres. */
#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Colores 0xAARRGGBB del tema */
typedef struct
{
    unsigned int bg, bar, sel, text, soft, dim, panel, line, holder, pill, rail, chip;
} UiTheme;

extern const UiTheme *g_th;
extern int            g_lightTheme;

#define COL_BG      (g_th->bg)
#define COL_BAR     (g_th->bar)
#define COL_SEL     (g_th->sel)
#define COL_TEXT    (g_th->text)
#define COL_SOFT    (g_th->soft)
#define COL_DIM     (g_th->dim)
#define COL_PANEL   (g_th->panel)
#define COL_CHIP    (g_th->chip)
#define COL_ACCENT  0xFFE62117
#define COL_GUIDE   0xFFE62117   /* menu desplegado (rojo de YouTube TV clasico) */
#define COL_OK      0xFF2EA84F
#define COL_WHITE   0xFFFFFFFF

void ui_set_theme(int light);

/* --- Ayudas de botones ---------------------------------------------
 * spec: "A=Ver|LB+RB=Pagina|B=Volver". Botones: A B X Y LB RB START BACK
 * DPAD LR UD. Se dibujan con el icono del boton y el texto al lado. */
float ui_hints_width(const char *spec);
/* Usuarios activos + "Tu sesion esta activa", centrado; devuelve la altura */
float ui_draw_active_users(float cx, float y);
/* Boton "Suscribirse" (rojo con brillo) / "Suscrito" (blanco), rectangulo de
 * forma rectangular; elegido: burbuja roja alrededor con la misma forma */
#define UI_SUB_W  184.0f
#define UI_SUB_H  46.0f
void  ui_draw_sub_pill(float x, float y, int subscribed, int focused, int enabled);
float ui_hints(float x, float y, const char *spec, unsigned int textColor);   /* devuelve la x final */

/* Ayudas abajo de la pantalla, desde x (sin barra de fondo) */
void  ui_footer(float x, const char *spec);

/* Rectangulo con esquinas redondeadas (solo colores opacos: las esquinas
 * se enciman con los lados) */
void  ui_round_rect(float x, float y, float w, float h, float r, unsigned int color);

/* El rojo con brillo del menu de calidades (degradado y reflejo arriba) */
void  ui_red_gloss(float x, float y, float w, float h);

/* Linea de grosor w entre dos puntos (con cuadritos; solo colores opacos) */
void  ui_line(float x0, float y0, float x1, float y1, float w, unsigned int color);

/* --- Perfil de la consola ------------------------------------------ */

/* En cada cuadro: cada 2 s revisa quien tiene sesion y, si cambio, lee su
 * gamertag y su foto de jugador. */
void  ui_profile_poll(void);

/* Foto de jugador redonda (o una silueta si no hay sesion). */
void  ui_profile_draw(float x, float y, float size);

/* Gamertag ("" sin sesion) */
const char *ui_profile_name(void);
void *ui_profile_texture(void);   /* foto de jugador 64x64 (NULL si no hay) */

/* Abre la ventana de la Xbox para elegir perfil */
void  ui_profile_signin(void);

/* --- Menu lateral --------------------------------------------------- */
enum
{
    UI_SEC_PROFILE,     /* foto de jugador: A = elegir perfil */
    UI_SEC_SEARCH,
    UI_SEC_HOME,
    UI_SEC_SUBS,        /* suscripciones (con cuenta de Google) */
    UI_SEC_COMMUNITY,   /* recomendados por la comunidad (Firebase) */
    UI_SEC_GAMING,
    UI_SEC_MUSIC,
    UI_SEC_NEWS,
    UI_SEC_SPORTS,
    UI_SEC_HISTORY,
    UI_SEC_SETTINGS,
    UI_SEC_COUNT
};

#define UI_RAIL_W   124.0f   /* ancho del menu plegado */
#define UI_GUIDE_W  320.0f   /* ancho desplegado */
#define UI_CONTENT_X 160.0f  /* donde empieza el contenido a la derecha del menu */
#define UI_RAIL_CX  70.0f    /* centro de los iconos (lejos del borde de la tele) */

const char *ui_section_name(int sec);

/* current = la seccion que se esta viendo; focus = la elegida con la
 * cruceta cuando el menu esta desplegado, o -1 (plegado) */
void  ui_draw_rail(int current, int focus);

/* Ruedita de carga estilo Xbox 360 (dos arcos que giran). cx, cy = centro;
 * rad = radio del arco de afuera. */
void ui_draw_spinner(float cx, float cy, float rad);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
