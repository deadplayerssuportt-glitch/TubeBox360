/* report.h - Registro de errores y reporte para los probadores. C89.
 *
 *   game:\errores.txt            cada error (con fecha y hora), se va
 *                                agregando; sobrevive a un cuelgue
 *   game:\TubeBox360_reporte.txt lo crea report_write (Configuracion ->
 *                                "Probar la conexion y crear reporte"):
 *                                version, memoria, config.ini, prueba de
 *                                red y lo ultimo de cada bitacora */
#ifndef TB_REPORT_H
#define TB_REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#define REPORT_PATH  "game:\\TubeBox360_reporte.txt"

/* Al arrancar (hilo principal): recorta las bitacoras si crecieron mucho
 * y anota el inicio de la sesion. */
void report_init(void);

/* Anota un error. where = donde paso ("busqueda", "reproductor"...).
 * Se puede llamar desde cualquier hilo. */
void report_error(const char *where, const char *fmt, ...);

/* Escribe el reporte completo. selftest = el texto de la prueba de red
 * (puede ser NULL). Devuelve 1 si se guardo. */
int report_write(const char *selftest);

#ifdef __cplusplus
}
#endif

#endif /* TB_REPORT_H */
