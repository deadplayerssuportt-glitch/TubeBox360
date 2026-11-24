/* avatar.h - El avatar del jugador junto al menu lateral desplegado. C89.
 *
 * Como en el dashboard de la 360: mira hacia el jugador, la palanca derecha
 * le mueve la cabeza y los gatillos lo giran. Abajo va un panelito con los
 * botones.
 *
 * Dibujo: por ahora avatar.c lo arma en 2.5D con la foto de jugador como
 * cara (busto que gira y cabeza que sigue a la palanca). El avatar 3D de
 * verdad (XAvatar del XDK) entra detras de esta misma interfaz cuando se
 * confirme la API con xavatar.h (ver LEEME): main.c no cambia.
 *
 * Uso: avatar_reset() al abrir el menu; cada vuelta con el menu abierto
 * avatar_input() y avatar_draw(). */
#ifndef TB_AVATAR_H
#define TB_AVATAR_H

#ifdef __cplusplus
extern "C" {
#endif

void avatar_reset(void);

/* X con el menu abierto: el gesto del elemento interactivo (o
 * el aviso de que el avatar no tiene) */
void avatar_gesture(void);

/* rx, ry: palanca derecha de -1 a 1 (ry positivo = arriba); lt, rt:
 * gatillos de 0 a 1; ms: milisegundos desde la vuelta anterior */
void avatar_input(float rx, float ry, float lt, float rt, unsigned long ms);

/* El avatar centrado en cx con los pies en floorY, de alto h; debajo el
 * panel de ayuda (titulo y botones) */
void avatar_draw(float cx, float floorY, float h);

#ifdef __cplusplus
}
#endif

#endif /* TB_AVATAR_H */
