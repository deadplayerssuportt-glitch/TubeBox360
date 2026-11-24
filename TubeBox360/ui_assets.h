/* ui_assets.h - Letra, iconos y logo de la interfaz (los datos estan en
 * ui_assets.c, que genera assets/gen_assets.py). C89.
 *
 * Todo va como PNG dentro del .xex: gfx.cpp los convierte en texturas con
 * D3DX al arrancar. Si alguno no carga, gfx.cpp sigue con la letra
 * monoespaciada y sin iconos (la app funciona igual, solo se ve mas simple). */
#ifndef UI_ASSETS_H
#define UI_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Una letra del atlas. (x, y, w, h): donde esta en la imagen.
 * (ox, oy): donde va su esquina respecto al punto de la linea base.
 * adv: cuanto avanza la pluma (pixeles, con decimales). */
typedef struct
{
    unsigned short cp;
    short x, y, w, h;
    short ox, oy;
    float adv;
} UiGlyph;

typedef struct
{
    int                  size;      /* tamano de la letra (pixeles por em) */
    int                  ascent;    /* de la linea base hacia arriba */
    int                  descent;   /* hacia abajo */
    int                  texW, texH;
    const unsigned char *png;
    int                  pngLen;
    const UiGlyph       *glyphs;    /* ASCII, Latin-1 y unos signos tipograficos */
    int                  count;
} UiFontDesc;

/* Roboto en 3 tamanos: el que mas se acerque al pedido se escala poco */
extern const UiFontDesc g_font20;
extern const UiFontDesc g_font24;
extern const UiFontDesc g_font40;
/* Roboto Bold 32: titulos (el del video en el reproductor, la cuenta
 * regresiva del siguiente video) */
extern const UiFontDesc g_font32b;
/* Japones y arabe (font_extra.c, gen_font_extra.py): un solo tamano, se
 * carga la primera vez que hace falta */
extern const UiFontDesc g_fontExtra;

/* Iconos (en el mismo orden que ICONS en gen_assets.py) */
enum
{
    UI_ICON_PLAY,
    UI_ICON_PAUSE,
    UI_ICON_REW10,
    UI_ICON_FWD10,
    UI_ICON_CLOSE,
    UI_ICON_HQ,
    UI_ICON_CC,
    UI_ICON_GLOBE,
    UI_ICON_INFO,
    UI_ICON_SEARCH,
    UI_ICON_ORB,        /* orbe rojo: fondo del boton elegido */
    UI_ICON_SUN,
    UI_ICON_MOON,
    UI_ICON_CHECK,
    UI_ICON_ORB_S,      /* orbe chico (selector de tema) */
    UI_ICON_KNOB,       /* punto de la barra de progreso */
    UI_ICON_HOURGLASS,  /* reloj de arena: hora en que acaba el video */
    /* menu lateral y teclado: blancos y planos (se pueden tenir) */
    UI_ICON_M_SEARCH,
    UI_ICON_M_HOME,
    UI_ICON_M_GAMING,
    UI_ICON_M_MUSIC,
    UI_ICON_M_NEWS,
    UI_ICON_M_SPORTS,
    UI_ICON_M_HISTORY,
    UI_ICON_M_SETTINGS,
    UI_ICON_M_USER,       /* perfil sin foto de jugador */
    UI_ICON_M_SOUND,      /* bocina (sonidos de la interfaz) */
    UI_ICON_M_SHORTS,     /* Shorts (rectangulo vertical con play) */
    UI_ICON_K_BACKSPACE,  /* tecla de borrar */
    /* botones del control (ayudas de abajo), celdas de 28 px */
    UI_ICON_PAD_A,
    UI_ICON_PAD_B,
    UI_ICON_PAD_X,
    UI_ICON_PAD_Y,
    UI_ICON_PAD_LB,
    UI_ICON_PAD_RB,
    UI_ICON_PAD_START,
    UI_ICON_PAD_BACK,
    UI_ICON_PAD_DPAD,
    UI_ICON_PAD_LR,
    UI_ICON_PAD_UD,
    UI_ICON_PAD_RIGHT,    /* cruceta con solo la flecha derecha */
    UI_ICON_PAD_LT,       /* gatillos */
    UI_ICON_PAD_RT,
    UI_ICON_PAD_LS,       /* palancas */
    UI_ICON_PAD_RS,
    UI_ICON_PAD_DOWN,     /* cruceta con solo la flecha de abajo */
    UI_ICON_M_SUBS,       /* menu: suscripciones */
    UI_ICON_LIKE,         /* reproductor: me gusta (contorno) */
    UI_ICON_LIKE_ON,      /*   ... ya marcado (relleno) */
    UI_ICON_DISLIKE,
    UI_ICON_DISLIKE_ON,
    UI_ICON_UPDATE,       /* aviso de version nueva (junto a la busqueda) */
    UI_ICON_XBOX,         /* logo de la consola, a color (usuarios activos) */
    UI_ICON_M_STAR,       /* menu: recomendados por la comunidad */
    UI_ICON_STAR,         /* reproductor: recomendar (contorno) */
    UI_ICON_STAR_ON,      /*   ... ya recomendado (relleno) */
    UI_ICON_COUNT
};

typedef struct
{
    short x, y, w, h;
} UiIconRect;

extern const unsigned char g_icons_png[];
extern const int           g_icons_png_len;
extern const int           g_icons_w;
extern const int           g_icons_h;
extern const UiIconRect    g_icon_rects[UI_ICON_COUNT];

extern const unsigned char g_logo_png[];
extern const int           g_logo_png_len;
extern const int           g_logo_w;
extern const int           g_logo_h;

/* Dibujo del mando de Xbox 360 para la pantalla de controles, y el punto
 * de cada boton dentro de la imagen (ahi llega la linea de su nombre) */
typedef struct { short x, y; } UiPoint;
enum
{
    UI_PADPT_LT, UI_PADPT_RT, UI_PADPT_LB, UI_PADPT_RB,
    UI_PADPT_LS, UI_PADPT_RS, UI_PADPT_DPAD,
    UI_PADPT_BACK, UI_PADPT_START, UI_PADPT_GUIDE,
    UI_PADPT_Y, UI_PADPT_X, UI_PADPT_B, UI_PADPT_A,
    UI_PADPT_COUNT
};
extern const unsigned char g_pad_png[];
extern const int           g_pad_png_len;
extern const int           g_pad_w;
extern const int           g_pad_h;
extern const UiPoint       g_pad_points[UI_PADPT_COUNT];

/* Logo de la comunidad DeadPIayers (Configuracion), 150 px de alto */
extern const unsigned char g_dp_png[];
extern const int           g_dp_png_len;
extern const int           g_dp_w;
extern const int           g_dp_h;

/* Sombra degradada (negra con alfa) para el borde de abajo de la cuadricula */
extern const unsigned char g_shadow_png[];
extern const int           g_shadow_png_len;
extern const int           g_shadow_w;
extern const int           g_shadow_h;

/* Sonidos de la interfaz que dio el usuario (PCM 16 bits, ver sfx.cpp) */
#define UI_SOUND(n) extern const short g_snd_##n##_pcm[]; extern const int g_snd_##n##_frames; \
                    extern const int g_snd_##n##_channels; extern const int g_snd_##n##_rate;
UI_SOUND(fila)
UI_SOUND(mover)
UI_SOUND(menu)
UI_SOUND(resultados)
UI_SOUND(salir)
#undef UI_SOUND

/* El mismo logo grande, para la pantalla de carga */
extern const unsigned char g_logo_big_png[];
extern const int           g_logo_big_png_len;
extern const int           g_logo_big_w;
extern const int           g_logo_big_h;

#ifdef __cplusplus
}
#endif

#endif /* UI_ASSETS_H */
