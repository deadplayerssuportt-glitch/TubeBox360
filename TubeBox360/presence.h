/* presence.h - Contador de usuarios activos (Firebase Realtime Database). C89.
 *
 * Cada consola conectada escribe cada 60 s "presence/<id>": {t: hora del
 * servidor} y cuenta cuantas escribieron en los ultimos 2.5 minutos. El id
 * es al azar en cada arranque (no identifica a nadie). La direccion de la
 * base va en version.h (TB_PRESENCE_URL); vacia = el contador no se usa. */
#ifndef TB_PRESENCE_H
#define TB_PRESENCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Despues de net_init (hilo principal) */
void presence_start(void);

/* Al salir de la app: borra el registro de esta consola (espera max 2 s) */
void presence_stop(void);

/* Usuarios activos, o -1 si no se sabe (sin internet / sin base) */
int  presence_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TB_PRESENCE_H */
