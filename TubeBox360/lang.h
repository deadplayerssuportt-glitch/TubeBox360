/* lang.h - Idioma de la app: espanol (predeterminado), ingles, portugues,
 * japones y arabe. C89.
 *
 * Los textos estan en lang_tab.h (generado). T(S_...) da el texto en el
 * idioma elegido. El idioma tambien decide con que idioma se le pide todo a
 * YouTube (fechas, vistas, nombres de pistas) y que pista de audio y
 * subtitulos se prefieren. */
#ifndef LANG_H
#define LANG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
#define X(id, es, en, pt, ja, ar) id,
#include "lang_tab.h"
#undef X
    STR_COUNT
} StrId;

enum { LANG_ES, LANG_EN, LANG_PT, LANG_JA, LANG_AR, LANG_COUNT };

void        lang_set(int lang);
int         lang_get(void);
const char *T(int id);

/* "es", "en", "pt", "ja" o "ar" (pista de audio y subtitulos preferidos) */
const char *lang_code(void);

/* hl y gl para YouTube: "es-419"/"MX", "en"/"US", "pt-BR"/"BR", "ja"/"JP", "ar"/"SA" */
const char *lang_hl(void);
const char *lang_gl(void);     /* region para YouTube: la elegida o la del idioma */

/* Region (Configuracion): 0 = automatica segun el idioma */
#define REGION_COUNT 19
void        lang_set_region(int r);
int         lang_region(void);
int         lang_region_from_code(const char *code);   /* "MX" -> indice (0 si no) */
const char *lang_region_code(int r);                   /* "MX" o "auto" (config.ini) */
const char *lang_region_name(int r);                   /* "Mexico", "Espana"... */

/* Dia (0 = domingo) y mes (0 = enero) abreviados para la fecha del reproductor */
const char *lang_day(int d);
const char *lang_month(int m);

#ifdef __cplusplus
}
#endif

#endif /* LANG_H */
