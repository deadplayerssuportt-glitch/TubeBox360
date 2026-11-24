/* arabic.h - Arabe en pantalla (une las letras y las pone de derecha a
 * izquierda). C89. */
#ifndef TB_ARABIC_H
#define TB_ARABIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Si el texto (UTF-8) tiene arabe, lo escribe en buf ya unido y en orden
 * visual (de izquierda a derecha en pantalla) y devuelve buf; si no, devuelve
 * el mismo texto. NO es seguro entre hilos (usa memoria estatica): solo
 * desde el hilo que dibuja. */
const char *arabic_visual(const char *utf8, char *buf, int size);

#ifdef __cplusplus
}
#endif

#endif /* TB_ARABIC_H */
