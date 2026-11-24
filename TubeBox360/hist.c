/* hist.c - Historial local de videos vistos (ver hist.h). C89.
 *
 * Formato del archivo: una linea por video, campos separados por tabulador:
 *   id <TAB> titulo <TAB> canal <TAB> duracion <TAB> vistas <TAB> fecha
 * Los tabuladores y saltos de linea dentro del texto se cambian por espacios
 * al guardar. */

#include <stdio.h>
#include <string.h>
#include "hist.h"

#define HIST_PATH "game:\\historial.txt"

static YtVideo g_hist[HIST_MAX];
static int     g_count = 0;

/* Copia un campo hasta el tabulador o el fin de linea. Devuelve el resto. */
static char *take_field(char *p, char *out, int size)
{
    int n = 0, cut = 0;

    while (*p && *p != '\t' && *p != '\r' && *p != '\n')
    {
        if (n < size - 1)
            out[n++] = *p;
        else if (!cut)
        {
            /* no cabe: si se corta un caracter UTF-8 a la mitad, se quita */
            cut = 1;
            if (((unsigned char)*p & 0xC0) == 0x80)
            {
                while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80)
                    --n;
                if (n > 0)
                    --n;
            }
        }
        ++p;
    }
    out[n] = 0;
    if (*p == '\t')
        ++p;
    return p;
}

static int valid_id(const char *id)
{
    int i;

    for (i = 0; id[i]; ++i)
    {
        char ch = id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            return 0;
    }
    return i >= 6;
}

void hist_load(void)
{
    char line[1024];
    char *p;
    YtVideo *v;
    FILE *f = fopen(HIST_PATH, "rb");

    g_count = 0;
    if (!f)
        return;
    while (g_count < HIST_MAX && fgets(line, sizeof(line), f))
    {
        v = &g_hist[g_count];
        memset(v, 0, sizeof(*v));
        p = take_field(line, v->id, sizeof(v->id));
        p = take_field(p, v->title, sizeof(v->title));
        p = take_field(p, v->channel, sizeof(v->channel));
        p = take_field(p, v->length, sizeof(v->length));
        p = take_field(p, v->views, sizeof(v->views));
        take_field(p, v->published, sizeof(v->published));
        if (valid_id(v->id) && v->title[0])
            ++g_count;
    }
    fclose(f);
}

static void put_field(FILE *f, const char *s, int last)
{
    for (; *s; ++s)
        fputc((*s == '\t' || *s == '\r' || *s == '\n') ? ' ' : *s, f);
    fputc(last ? '\n' : '\t', f);
}

static void save(void)
{
    FILE *f = fopen(HIST_PATH, "wb");
    int i;

    if (!f)
        return;
    for (i = 0; i < g_count; ++i)
    {
        put_field(f, g_hist[i].id, 0);
        put_field(f, g_hist[i].title, 0);
        put_field(f, g_hist[i].channel, 0);
        put_field(f, g_hist[i].length, 0);
        put_field(f, g_hist[i].views, 0);
        put_field(f, g_hist[i].published, 1);
    }
    fclose(f);
}

void hist_add(const YtVideo *v)
{
    YtVideo copy;
    int i, found = -1;

    if (!v || !valid_id(v->id))
        return;
    copy = *v;

    for (i = 0; i < g_count; ++i)
        if (strcmp(g_hist[i].id, v->id) == 0)
        {
            found = i;
            break;
        }
    if (found < 0)
        found = g_count < HIST_MAX ? g_count++ : HIST_MAX - 1;   /* se pierde el mas viejo */

    /* recorrer hacia abajo y poner este primero */
    for (i = found; i > 0; --i)
        g_hist[i] = g_hist[i - 1];
    g_hist[0] = copy;
    save();
}

int hist_count(void)
{
    return g_count;
}

int hist_copy(YtVideo *out, int max)
{
    int n = g_count < max ? g_count : max;

    if (n > 0)
        memcpy(out, g_hist, sizeof(YtVideo) * n);
    return n;
}
