/* sfx.h - Sonidos de la interfaz (estilo del dashboard NXE de la 360).
 *
 * Los sonidos se sintetizan al arrancar (son propios, no se copian los de
 * Microsoft). Si existe game:\sonidos\<nombre>.wav (PCM de 16 bits, mono o
 * estereo, cualquier frecuencia) se usa ese archivo en lugar del sintetizado:
 *   mover.wav  elegir.wav  volver.wav  menu_abrir.wav  menu_cerrar.wav
 *   tecla.wav  clic.wav  fila.wav  resultados.wav  salir.wav
 * mover, menu_abrir/menu_cerrar, fila, resultados y salir ya vienen con los
 * sonidos que dio el usuario (dentro del .xex, en ui_assets.c).
 *
 * Uso: sfx_init() despues de au_init(); sfx_play(SFX_...) desde el hilo
 * principal. sfx_frame() al empezar cada vuelta del ciclo principal y
 * sfx_played() para saber si ya sono algo en esta vuelta. */
#ifndef SFX_H
#define SFX_H

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    SFX_MOVE,         /* recorrer videos, menu, ajustes */
    SFX_SELECT,       /* A */
    SFX_BACK,         /* B */
    SFX_MENU_OPEN,    /* se despliega el menu lateral */
    SFX_MENU_CLOSE,
    SFX_KEY,          /* tecla del teclado en pantalla (mecanica) */
    SFX_CLICK,        /* Buscar / elegir una sugerencia (clic de mouse) */
    SFX_ROW,          /* cambiar de fila en la cuadricula */
    SFX_RESULTS,      /* llegaron los resultados de una busqueda */
    SFX_EXIT,         /* se abre el cuadro de Salir */
    SFX_COUNT
};

int  sfx_init(void);          /* 1 si hay sonido */
void sfx_play(int id);
void sfx_enable(int on);      /* 0 = silencio (se guarda en la configuracion) */
int  sfx_enabled(void);
void sfx_frame(void);
int  sfx_played(void);

#ifdef __cplusplus
}
#endif

#endif /* SFX_H */
