/* community.h - "Recomendados por la comunidad" (Firebase). C89.
 *
 * Misma base que el contador de usuarios (TB_PRESENCE_URL):
 *   recomendados/{videoId} = { ti: titulo, ch: canal, ci: id del canal,
 *                              l: duracion, a: avatar, t: hora del servidor }
 * La lista son los 10 con la hora mas nueva. Recomendar un video que ya
 * esta lo sube al primer lugar (no se repite). Al recomendar, la consola
 * borra los que quedaron despues del lugar 10 (cada nueva sustituye a la
 * mas vieja). */
#ifndef TB_COMMUNITY_H
#define TB_COMMUNITY_H

#include "yt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMMUNITY_MAX 10

int  community_enabled(void);   /* hay base de Firebase en version.h */

/* Recomienda (hilo propio; se puede llamar desde el hilo principal).
 * Devuelve 0 si no se pudo ni empezar: sin base, o hace menos de 30 s de
 * la anterior (community_wait_secs dice cuanto falta). */
int  community_recommend(const YtVideo *v);
int  community_wait_secs(void);

/* Ya recomendado (en esta sesion o en la ultima lista leida) */
int  community_is_recommended(const char *videoId);

/* Resultado de la ultima recomendacion, una sola vez:
 * 0 = nada nuevo, 1 = se guardo, -1 = fallo */
int  community_take_result(void);

/* Sube cada vez que esta consola recomienda algo con exito (para saber
 * que la lista guardada ya cambio) */
int  community_version(void);

/* Lee la lista (bloquea: hilo de trabajo). Devuelve cuantos (0..max) o -1 */
int  community_fetch(YtVideo *out, int max, char *err, int errSize);

#ifdef __cplusplus
}
#endif

#endif
