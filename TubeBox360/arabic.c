/* arabic.c - Arabe en pantalla: une las letras (formas inicial, media,
 * final y aislada) y las pone de derecha a izquierda. C89. Ver arabic.h.
 * Bidi simplificado: alcanza para titulos, subtitulos y la interfaz. */

#include <string.h>
#include "arabic.h"

#define MAXCP 600

/* Formas de presentacion (U+FE80..) de U+0621..U+064A:
 * aislada, final, inicial, media (0 = no tiene) */
static const unsigned short kForms[0x064A - 0x0621 + 1][4] = {
    { 0xFE80, 0x0000, 0x0000, 0x0000 }, /* 0621 */
    { 0xFE81, 0xFE82, 0x0000, 0x0000 }, /* 0622 */
    { 0xFE83, 0xFE84, 0x0000, 0x0000 }, /* 0623 */
    { 0xFE85, 0xFE86, 0x0000, 0x0000 }, /* 0624 */
    { 0xFE87, 0xFE88, 0x0000, 0x0000 }, /* 0625 */
    { 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C }, /* 0626 */
    { 0xFE8D, 0xFE8E, 0x0000, 0x0000 }, /* 0627 */
    { 0xFE8F, 0xFE90, 0xFE91, 0xFE92 }, /* 0628 */
    { 0xFE93, 0xFE94, 0x0000, 0x0000 }, /* 0629 */
    { 0xFE95, 0xFE96, 0xFE97, 0xFE98 }, /* 062A */
    { 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C }, /* 062B */
    { 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 }, /* 062C */
    { 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 }, /* 062D */
    { 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 }, /* 062E */
    { 0xFEA9, 0xFEAA, 0x0000, 0x0000 }, /* 062F */
    { 0xFEAB, 0xFEAC, 0x0000, 0x0000 }, /* 0630 */
    { 0xFEAD, 0xFEAE, 0x0000, 0x0000 }, /* 0631 */
    { 0xFEAF, 0xFEB0, 0x0000, 0x0000 }, /* 0632 */
    { 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 }, /* 0633 */
    { 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 }, /* 0634 */
    { 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC }, /* 0635 */
    { 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 }, /* 0636 */
    { 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 }, /* 0637 */
    { 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 }, /* 0638 */
    { 0xFEC9, 0xFECA, 0xFECB, 0xFECC }, /* 0639 */
    { 0xFECD, 0xFECE, 0xFECF, 0xFED0 }, /* 063A */
    { 0x0000, 0x0000, 0x0000, 0x0000 }, /* 063B */
    { 0x0000, 0x0000, 0x0000, 0x0000 }, /* 063C */
    { 0x0000, 0x0000, 0x0000, 0x0000 }, /* 063D */
    { 0x0000, 0x0000, 0x0000, 0x0000 }, /* 063E */
    { 0x0000, 0x0000, 0x0000, 0x0000 }, /* 063F */
    { 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0640 */
    { 0xFED1, 0xFED2, 0xFED3, 0xFED4 }, /* 0641 */
    { 0xFED5, 0xFED6, 0xFED7, 0xFED8 }, /* 0642 */
    { 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC }, /* 0643 */
    { 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 }, /* 0644 */
    { 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 }, /* 0645 */
    { 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 }, /* 0646 */
    { 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC }, /* 0647 */
    { 0xFEED, 0xFEEE, 0x0000, 0x0000 }, /* 0648 */
    { 0xFEEF, 0xFEF0, 0x0000, 0x0000 }, /* 0649 */
    { 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 }, /* 064A */
};

static int is_letter(unsigned int c)       /* letra arabe con formas */
{
    return c >= 0x0621 && c <= 0x064A && kForms[c - 0x0621][0] != 0;
}

static int joins_next(unsigned int c)      /* se une con la que sigue */
{
    if (c == 0x0640)
        return 1;                          /* tatweel */
    return is_letter(c) && kForms[c - 0x0621][2] != 0;
}

static int joins_prev(unsigned int c)      /* se une con la anterior */
{
    return c == 0x0640 || (is_letter(c) && kForms[c - 0x0621][1] != 0);
}

static int is_harakat(unsigned int c)
{
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670;
}

/* fuertemente de derecha a izquierda */
static int is_rtl(unsigned int c)
{
    return (c >= 0x0590 && c <= 0x08FF && !(c >= 0x0660 && c <= 0x0669)) ||
           (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

static int is_neutral(unsigned int c)
{
    if (c < 0x80)
        return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
    return c == 0x060C || c == 0x061B || c == 0x061F || c == 0x00A0 || c == 0x2026 ||
           (c >= 0x2010 && c <= 0x2027) || c == 0x00AB || c == 0x00BB;
}

static unsigned int mirror(unsigned int c)
{
    switch (c)
    {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB;
    case 0x00BB: return 0x00AB;
    default: return c;
    }
}

static const char *utf8_next(const char *s, unsigned int *cp)
{
    const unsigned char *u = (const unsigned char *)s;

    if (u[0] < 0x80) { *cp = u[0]; return s + 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return s + 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2])
    {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return s + 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3])
    {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return s + 4;
    }
    *cp = '?';
    return s + 1;
}

static int utf8_put(char *out, int pos, int size, unsigned int c)
{
    if (c < 0x80)
    {
        if (pos + 1 >= size) return -1;
        out[pos++] = (char)c;
    }
    else if (c < 0x800)
    {
        if (pos + 2 >= size) return -1;
        out[pos++] = (char)(0xC0 | (c >> 6));
        out[pos++] = (char)(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        if (pos + 3 >= size) return -1;
        out[pos++] = (char)(0xE0 | (c >> 12));
        out[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (c & 0x3F));
    }
    else
    {
        if (pos + 4 >= size) return -1;
        out[pos++] = (char)(0xF0 | (c >> 18));
        out[pos++] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (c & 0x3F));
    }
    return pos;
}

static void reverse(unsigned int *a, int from, int to)   /* [from, to) */
{
    unsigned int t;

    for (--to; from < to; ++from, --to)
    {
        t = a[from];
        a[from] = a[to];
        a[to] = t;
    }
}

const char *arabic_visual(const char *utf8, char *buf, int size)
{
    static unsigned int in[MAXCP];
    static unsigned int sh[MAXCP];
    static signed char dir[MAXCP];     /* 1 = der-izq, 0 = izq-der, -1 = neutro */
    const char *s;
    unsigned int c, prev, next;
    int n = 0, m = 0, i, j, k, any = 0, rtlPara = -1, pos, form;

    /* (rapido) sin arabe: igual */
    for (s = utf8; *s; ++s)
        if ((unsigned char)*s == 0xD8 || (unsigned char)*s == 0xD9 || (unsigned char)*s == 0xDB)
        {
            any = 1;
            break;
        }
    if (!any)
        return utf8;

    for (s = utf8; *s && n < MAXCP;)
    {
        s = utf8_next(s, &c);
        if (!is_harakat(c))
            in[n++] = c;               /* las marcas de vocal no se dibujan */
    }

    /* 1. formas unidas (y lam + alef) */
    for (i = 0; i < n; ++i)
    {
        c = in[i];
        if (!is_letter(c))
        {
            sh[m++] = c;
            continue;
        }
        prev = i > 0 ? in[i - 1] : 0;
        next = i + 1 < n ? in[i + 1] : 0;
        if (c == 0x0644 && (next == 0x0622 || next == 0x0623 || next == 0x0625 || next == 0x0627))
        {
            static const unsigned short la[4][2] = { { 0xFEF5, 0xFEF6 }, { 0xFEF7, 0xFEF8 },
                                                     { 0xFEF9, 0xFEFA }, { 0xFEFB, 0xFEFC } };
            k = next == 0x0622 ? 0 : next == 0x0623 ? 1 : next == 0x0625 ? 2 : 3;
            sh[m++] = la[k][joins_next(prev) ? 1 : 0];
            ++i;                       /* el alef ya va en la ligadura */
            continue;
        }
        form = 0;
        if (joins_next(prev) && joins_prev(c))
            form = 1;                  /* final */
        if (joins_next(c) && joins_prev(next))
            form = form == 1 ? 3 : 2;  /* media / inicial */
        sh[m++] = kForms[c - 0x0621][form] ? kForms[c - 0x0621][form] : kForms[c - 0x0621][0];
    }

    /* 2. direccion de cada letra; la del parrafo = la primera fuerte */
    for (i = 0; i < m; ++i)
    {
        c = sh[i];
        dir[i] = is_rtl(c) ? 1 : is_neutral(c) ? -1 : 0;
        if (rtlPara < 0 && dir[i] >= 0)
            rtlPara = dir[i];
    }
    if (rtlPara < 0)
        rtlPara = 0;
    /* neutros: si a los dos lados hay la misma direccion, esa; si no, la del parrafo */
    for (i = 0; i < m; ++i)
    {
        if (dir[i] >= 0)
            continue;
        for (j = i; j < m && dir[j] < 0; ++j)
            ;
        {
            int before = i > 0 ? dir[i - 1] : rtlPara;
            int after = j < m ? dir[j] : rtlPara;
            int d = before == after ? before : rtlPara;
            for (k = i; k < j; ++k)
                dir[k] = (signed char)d;
        }
        i = j - 1;
    }
    for (i = 0; i < m; ++i)
        if (dir[i] == 1)
            sh[i] = mirror(sh[i]);

    /* 3. orden visual */
    if (rtlPara)
    {
        reverse(sh, 0, m);
        /* lo izq-der (numeros, latin) vuelve a su orden dentro de su tramo */
        for (i = 0; i < m / 2; ++i)
        {
            signed char t = dir[i];
            dir[i] = dir[m - 1 - i];
            dir[m - 1 - i] = t;
        }
        for (i = 0; i < m; ++i)
            if (dir[i] == 0)
            {
                for (j = i; j < m && dir[j] == 0; ++j)
                    ;
                reverse(sh, i, j);
                i = j - 1;
            }
    }
    else
    {
        for (i = 0; i < m; ++i)
            if (dir[i] == 1)
            {
                for (j = i; j < m && dir[j] == 1; ++j)
                    ;
                reverse(sh, i, j);
                i = j - 1;
            }
    }

    pos = 0;
    for (i = 0; i < m; ++i)
    {
        k = utf8_put(buf, pos, size, sh[i]);
        if (k < 0)
            break;
        pos = k;
    }
    buf[pos] = 0;
    return buf;
}
