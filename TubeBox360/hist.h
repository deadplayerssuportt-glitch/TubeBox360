/* hist.h - Historial local de videos vistos (Inicio, paso E3). C89.
 *
 * Se guarda en game:\historial.txt (la carpeta de la app), un video por
 * linea, el mas reciente primero. Sirve para "Porque viste..." en el
 * Inicio: YouTube no da recomendaciones sin iniciar sesion, pero si da los
 * videos relacionados con uno que ya viste.
 *
 * Todas las funciones son del hilo principal, salvo hist_copy, que hace una
 * copia para usarla en el hilo de trabajo. */
#ifndef HIST_H
#define HIST_H

#include "yt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HIST_MAX 50

/* Lee el archivo (si existe). Llamar una vez al arrancar. */
void hist_load(void);

/* Anota que se empezo a ver este video (lo pone primero, sin repetir) y
 * guarda el archivo. */
void hist_add(const YtVideo *v);

/* Cuantos hay */
int  hist_count(void);

/* Copia los primeros max (el mas reciente primero). Devuelve cuantos copio. */
int  hist_copy(YtVideo *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* HIST_H */
