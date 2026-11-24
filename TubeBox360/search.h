/* search.h - Teclado de busqueda propio (estilo YouTube TV). C89.
 *
 * A la izquierda las sugerencias (las de YouTube mientras se escribe, o
 * las busquedas recientes si no hay texto); a la derecha el teclado:
 * letras con N, &123 para numeros, acentos y simbolos, borrar, y abajo
 * ESPACIO, BORRAR y BUSCAR. Atajos: X borra, Y espacio, Start busca,
 * B regresa. Las recientes se guardan en game:\busquedas.txt. */
#ifndef SEARCH_H
#define SEARCH_H

#ifdef __cplusplus
extern "C" {
#endif

enum { SEARCH_NONE, SEARCH_SUBMIT, SEARCH_BACK, SEARCH_MENU };

/* Una vez al inicio (despues de net_init): hilo de sugerencias y
 * busquedas recientes */
void search_init(void);

/* Al entrar a la pantalla: el cursor en la primera letra */
void search_open(void);

/* Botones recien presionados; stickDir = -1/0/1 si la palanca acaba de
 * moverse a un lado. Devuelve SEARCH_... */
int  search_input(unsigned short pressed, int stickDir);

/* Lo escrito (UTF-8) */
const char *search_text(void);

/* Despues de SEARCH_SUBMIT: lo que hay que buscar (ya quedo en recientes) */
const char *search_query(void);

/* Dibuja sugerencias y teclado en el area de contenido (x0 = izquierda) */
void search_draw(float x0);

#ifdef __cplusplus
}
#endif

#endif /* SEARCH_H */
