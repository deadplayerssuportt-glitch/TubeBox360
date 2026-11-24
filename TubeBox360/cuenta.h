/* cuenta.h - Cuenta de Google (opcional): iniciar sesion, suscripciones y
 * "me gusta / no me gusta". C89.
 *
 * Usa la API oficial de datos de YouTube (YouTube Data API v3) con OAuth 2.0
 * para TV y dispositivos con entrada limitada: la consola muestra un codigo,
 * el usuario abre google.com/device en su telefono o PC, lo escribe y da
 * permiso. La consola nunca ve la contrasena.
 *
 * El ID y el secreto del cliente OAuth van en version.h
 * (TB_GOOGLE_CLIENT_ID / TB_GOOGLE_CLIENT_SECRET). Vacios = no se puede
 * iniciar sesion (la fila de Configuracion lo dice).
 *
 * La sesion se guarda en game:\cuenta.ini (el "refresh token"). Ese archivo
 * es como una llave de la cuenta: no se comparte ni va en los reportes. */
#ifndef TB_CUENTA_H
#define TB_CUENTA_H

#include "yt.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    CUENTA_OFF,        /* sin sesion */
    CUENTA_NOCLIENT,   /* falta el ID de cliente en version.h */
    CUENTA_ASKING,     /* pidiendo el codigo a Google */
    CUENTA_CODE,       /* mostrando el codigo; esperando al usuario */
    CUENTA_FINISH,     /* ya dio permiso: leyendo el nombre del canal */
    CUENTA_ON          /* sesion iniciada */
};

/* Lee game:\cuenta.ini. Una vez al inicio (despues de net_init). */
void        cuenta_init(void);
/* Al salir de la app (espera a los hilos como maximo ~2 s) */
void        cuenta_shutdown(void);

int         cuenta_state(void);
int         cuenta_signed_in(void);
const char *cuenta_name(void);      /* nombre del canal de YouTube ("" sin sesion) */
const char *cuenta_avatar(void);    /* URL de su foto ("" si no hay) */
const char *cuenta_handle(void);    /* "@usuario" del canal ("" si no tiene) */

/* Ultimo aviso (error al iniciar sesion, sesion vencida...). "" = nada.
 * cuenta_clear_msg lo borra. */
const char *cuenta_msg(void);
void        cuenta_clear_msg(void);

/* Inicio de sesion (hilo propio). Mientras el estado es CUENTA_CODE,
 * cuenta_code da el codigo, la pagina y los segundos que le quedan. */
void        cuenta_login_start(void);
void        cuenta_login_cancel(void);
void        cuenta_code(char *code, int codeSize, char *url, int urlSize, int *secsLeft);

/* Cierra la sesion: borra game:\cuenta.ini y avisa a Google (revoca). */
void        cuenta_logout(void);

/* ---- Para los hilos de trabajo (bloquean; nunca en el hilo principal) ---- */

/* Suscripciones: chans = canales (YT_KIND_CHANNEL, ordenados por actividad),
 * vids = los videos mas nuevos de los canales con actividad reciente.
 * Devuelve 1 si salio bien (aunque venga vacio), 0 en error (motivo en err). */
int cuenta_subscriptions(YtVideo *vids, int maxVids, int *nVids,
                         YtVideo *chans, int maxChans, int *nChans,
                         char *err, int errSize);

/* ---- Suscribirse a un canal (asincrono, como me gusta) ----
 * cuesta en cuota: revisar 1, suscribir 50, cancelar 50 */
enum { SUB_UNKNOWN = -9, SUB_NO = 0, SUB_YES = 1 };
void cuenta_sub_request(const char *channelId);    /* revisar en segundo plano */
int  cuenta_sub_state(const char *channelId);      /* SUB_UNKNOWN mientras llega */
void cuenta_sub_set(const char *channelId, int subscribe);   /* 1 = suscribir, 0 = cancelar */
int  cuenta_sub_version(void);                     /* sube con cada cambio (para recargar Suscripciones) */

/* ---- Me gusta / no me gusta (asincrono, se puede llamar en cada cuadro) ---- */
enum { RATE_UNKNOWN = -9, RATE_DISLIKE = -1, RATE_NONE = 0, RATE_LIKE = 1 };

/* Pide en segundo plano la calificacion actual de este video */
void cuenta_rating_request(const char *videoId);
/* Calificacion conocida de este video (RATE_UNKNOWN mientras llega) */
int  cuenta_rating(const char *videoId);
/* Califica (RATE_LIKE, RATE_DISLIKE o RATE_NONE para quitarla). Se ve al
 * instante; si Google responde con error se regresa y queda en cuenta_msg. */
void cuenta_rate(const char *videoId, int rating);

#ifdef __cplusplus
}
#endif

#endif /* TB_CUENTA_H */
