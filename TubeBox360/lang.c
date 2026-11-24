/* lang.c - Textos de la app en 5 idiomas. C89. Ver lang.h. */
#include <string.h>
#include "lang.h"

static const char *const kTab[STR_COUNT][LANG_COUNT] = {
#define X(id, es, en, pt, ja, ar) { es, en, pt, ja, ar },
#include "lang_tab.h"
#undef X
};

static const char *const kDays[LANG_COUNT][7] = {
    { "Dom", "Lun", "Mar", "Mi\xC3\xA9", "Jue", "Vie", "S\xC3\xA1" "b" },
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    { "Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "S\xC3\xA1" "b" },
    { "\xE6\x97\xA5", "\xE6\x9C\x88", "\xE7\x81\xAB", "\xE6\xB0\xB4", "\xE6\x9C\xA8", "\xE9\x87\x91", "\xE5\x9C\x9F" },
    { "\xD8\xA7\xD9\x84\xD8\xA3\xD8\xAD\xD8\xAF", "\xD8\xA7\xD9\x84\xD8\xA5\xD8\xAB\xD9\x86\xD9\x8A\xD9\x86", "\xD8\xA7\xD9\x84\xD8\xAB\xD9\x84\xD8\xA7\xD8\xAB\xD8\xA7\xD8\xA1", "\xD8\xA7\xD9\x84\xD8\xA3\xD8\xB1\xD8\xA8\xD8\xB9\xD8\xA7\xD8\xA1", "\xD8\xA7\xD9\x84\xD8\xAE\xD9\x85\xD9\x8A\xD8\xB3", "\xD8\xA7\xD9\x84\xD8\xAC\xD9\x85\xD8\xB9\xD8\xA9", "\xD8\xA7\xD9\x84\xD8\xB3\xD8\xA8\xD8\xAA" }
};

static const char *const kMonths[LANG_COUNT][12] = {
    { "ene.", "feb.", "mar.", "abr.", "may.", "jun.", "jul.", "ago.", "sep.", "oct.", "nov.", "dic." },
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
    { "jan.", "fev.", "mar.", "abr.", "mai.", "jun.", "jul.", "ago.", "set.", "out.", "nov.", "dez." },
    { "1\xE6\x9C\x88", "2\xE6\x9C\x88", "3\xE6\x9C\x88", "4\xE6\x9C\x88", "5\xE6\x9C\x88", "6\xE6\x9C\x88", "7\xE6\x9C\x88", "8\xE6\x9C\x88", "9\xE6\x9C\x88", "10\xE6\x9C\x88", "11\xE6\x9C\x88", "12\xE6\x9C\x88" },
    { "\xD9\x8A\xD9\x86\xD8\xA7\xD9\x8A\xD8\xB1", "\xD9\x81\xD8\xA8\xD8\xB1\xD8\xA7\xD9\x8A\xD8\xB1", "\xD9\x85\xD8\xA7\xD8\xB1\xD8\xB3", "\xD8\xA3\xD8\xA8\xD8\xB1\xD9\x8A\xD9\x84", "\xD9\x85\xD8\xA7\xD9\x8A\xD9\x88", "\xD9\x8A\xD9\x88\xD9\x86\xD9\x8A\xD9\x88", "\xD9\x8A\xD9\x88\xD9\x84\xD9\x8A\xD9\x88", "\xD8\xA3\xD8\xBA\xD8\xB3\xD8\xB7\xD8\xB3", "\xD8\xB3\xD8\xA8\xD8\xAA\xD9\x85\xD8\xA8\xD8\xB1", "\xD8\xA3\xD9\x83\xD8\xAA\xD9\x88\xD8\xA8\xD8\xB1", "\xD9\x86\xD9\x88\xD9\x81\xD9\x85\xD8\xA8\xD8\xB1", "\xD8\xAF\xD9\x8A\xD8\xB3\xD9\x85\xD8\xA8\xD8\xB1" }
};

static int g_lang = LANG_ES;

void lang_set(int lang)
{
    g_lang = (lang >= 0 && lang < LANG_COUNT) ? lang : LANG_ES;
}

int lang_get(void)
{
    return g_lang;
}

const char *T(int id)
{
    return (id >= 0 && id < STR_COUNT) ? kTab[id][g_lang] : "";
}

const char *lang_code(void)
{
    static const char *const c[LANG_COUNT] = { "es", "en", "pt", "ja", "ar" };
    return c[g_lang];
}

const char *lang_hl(void)
{
    static const char *const c[LANG_COUNT] = { "es-419", "en", "pt-BR", "ja", "ar" };
    return c[g_lang];
}

/* Regiones que se pueden elegir en Configuracion (0 = automatica, segun el
 * idioma). El nombre va en su propio idioma, asi no hay que traducirlo. */
static const char *const kRegionCode[REGION_COUNT] =
{
    "", "MX", "ES", "AR", "CO", "CL", "PE", "VE", "EC", "GT",
    "US", "GB", "CA", "BR", "PT", "JP", "SA", "AE", "EG"
};
static const char *const kRegionName[REGION_COUNT] =
{
    "", "M\xC3\xA9xico", "Espa\xC3\xB1" "a", "Argentina", "Colombia", "Chile", "Per\xC3\xBA", "Venezuela",
    "Ecuador", "Guatemala", "United States", "United Kingdom", "Canada", "Brasil", "Portugal",
    "\xE6\x97\xA5\xE6\x9C\xAC",                                   /* Japon */
    "\xD8\xA7\xD9\x84\xD8\xB3\xD8\xB9\xD9\x88\xD8\xAF\xD9\x8A\xD8\xA9",   /* Arabia Saudita */
    "\xD8\xA7\xD9\x84\xD8\xA5\xD9\x85\xD8\xA7\xD8\xB1\xD8\xA7\xD8\xAA",   /* Emiratos */
    "\xD9\x85\xD8\xB5\xD8\xB1"                                    /* Egipto */
};
static int g_region = 0;

static const char *auto_gl(void)
{
    static const char *const c[LANG_COUNT] = { "MX", "US", "BR", "JP", "SA" };
    return c[g_lang];
}

const char *lang_gl(void)
{
    return g_region > 0 ? kRegionCode[g_region] : auto_gl();
}

void lang_set_region(int r)
{
    g_region = (r >= 0 && r < REGION_COUNT) ? r : 0;
}

int lang_region(void)
{
    return g_region;
}

int lang_region_from_code(const char *code)
{
    int i;
    for (i = 1; i < REGION_COUNT; ++i)
        if (code && strncmp(code, kRegionCode[i], 2) == 0)
            return i;
    return 0;
}

const char *lang_region_code(int r)
{
    return (r > 0 && r < REGION_COUNT) ? kRegionCode[r] : "auto";
}

/* Nombre para mostrar; la automatica dice cual usa ahora */
const char *lang_region_name(int r)
{
    int i;
    if (r > 0 && r < REGION_COUNT)
        return kRegionName[r];
    for (i = 1; i < REGION_COUNT; ++i)
        if (strcmp(kRegionCode[i], auto_gl()) == 0)
            return kRegionName[i];
    return auto_gl();
}

const char *lang_day(int d)
{
    return kDays[g_lang][((d % 7) + 7) % 7];
}

const char *lang_month(int m)
{
    return kMonths[g_lang][((m % 12) + 12) % 12];
}
