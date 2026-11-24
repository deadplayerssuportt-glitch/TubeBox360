/* update.h - Aviso y descarga de version nueva (Firebase: rama "version"). C89.
 *
 * En la base de datos (la misma del contador de usuarios), escrito a mano
 * desde console.firebase.google.com:
 *
 *   version
 *     build:  101                 (numero entero; mayor = mas nueva)
 *     v:      "1.01"              (lo que se muestra)
 *     notas:  "Que trae..."       (opcional)
 *     xex:    "enlace de Google Drive (compartido: cualquiera con el enlace)"
 *     size:   21534720            (opcional: bytes del .xex, para el %)
 *     url:    "pagina para descargar a mano"   (opcional)
 *
 * Si build > TB_BUILD (version.h): punto amarillo en el icono de descarga
 * junto a la barra de busqueda. La descarga se guarda en
 * game:\Actualizacion\default.xex; el usuario la copia a mano fuera de la app. */
#ifndef TB_UPDATE_H
#define TB_UPDATE_H

#ifdef __cplusplus
extern "C" {
#endif

void        update_start(void);     /* una sola consulta en segundo plano */
void        update_stop(void);
int         update_enabled(void);   /* hay base de Firebase en version.h */
int         update_checked(void);   /* 0 = aun no, 1 = se consulto, -1 = no se pudo */
int         update_available(void); /* 1 = hay una version mas nueva */
const char *update_version(void);   /* "1.01" */
const char *update_notes(void);     /* puede ser "" */
const char *update_url(void);       /* puede ser "" */
int         update_can_download(void);   /* hay enlace "xex" */

/* Descarga (hilo propio) */
enum { UPD_DL_IDLE, UPD_DL_RUNNING, UPD_DL_DONE, UPD_DL_FAIL };
void        update_download_start(void);
void        update_download_cancel(void);
int         update_dl_state(void);
void        update_dl_progress(double *done, double *total);   /* total 0 = no se sabe */
const char *update_dl_error(void);

#define UPD_DIR   "game:\\Actualizacion"
#define UPD_FILE  "game:\\Actualizacion\\default.xex"

#ifdef __cplusplus
}
#endif

#endif
