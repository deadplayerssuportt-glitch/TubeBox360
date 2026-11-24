/* avatar3d.h - El avatar 3D de verdad (API XAvatar del XDK), en C.
 *
 * avatar.c lo usa cuando esta listo; mientras carga, o si el perfil no
 * tiene avatar, avatar.c dibuja su busto 2.5D.
 *
 * av3d_init() una vez despues de gfx_init(); av3d_poll() cada vuelta del
 * ciclo principal (sigue al perfil con sesion y va cargando el avatar). */
#ifndef AVATAR3D_H
#define AVATAR3D_H

#ifdef __cplusplus
extern "C" {
#endif

int  av3d_init(void);            /* 1 si XAvatarInitialize funciono */
void av3d_poll(void);
int  av3d_ready(void);           /* 1 = ya se puede dibujar */
void av3d_wave(void);            /* que salude (al abrir el menu lateral) */
/* Gesto del elemento interactivo (mascota, objeto) sin fin; llamarla otra
 * vez lo detiene. 1 = empezo, 0 = se detuvo, -1 = no tiene / no esta listo */
int  av3d_gesture(void);
int  av3d_loading(void);         /* 1 = pidiendo los datos o cargando */
const char *av3d_status(void);   /* para la depuracion */

/* Dibuja el avatar de cuerpo entero dentro del rectangulo (x, y, w, h).
 * yaw/pitch de la cabeza de -1 a 1, turn = giro del cuerpo en radianes,
 * dt = segundos desde la vuelta anterior. 1 si se dibujo. */
int  av3d_draw(float x, float y, float w, float h, float yaw, float pitch, float turn, float dt);

#ifdef __cplusplus
}
#endif

#endif /* AVATAR3D_H */
