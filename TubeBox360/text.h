/* text.h - Utilidades de texto UTF-8 para la interfaz. C89. */
#ifndef TEXT_H
#define TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Copia src en dst con un maximo de maxChars caracteres; si no cabe,
 * corta y agrega "..." */
void fit_text(const char *src, int maxChars, char *dst, int dstSize);

/* Saca en line la primera linea de src que quepa en maxChars, cortando en
 * un espacio si se puede. Devuelve donde empieza el resto. */
const char *wrap_line(const char *src, int maxChars, char *line, int lineSize);

/* UTF-16 (lo que devuelve el teclado de la Xbox) a UTF-8 */
void utf16_to_utf8(const unsigned short *src, char *dst, int size);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_H */
