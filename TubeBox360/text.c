/* text.c - Utilidades de texto UTF-8 (ver text.h). C89. */

#include <string.h>
#include "text.h"

void fit_text(const char *src, int maxChars, char *dst, int dstSize)
{
    int chars = 0, i = 0, cut = -1;

    while (src[i])
    {
        if (((unsigned char)src[i] & 0xC0) != 0x80)
        {
            if (chars == maxChars - 3)
                cut = i;
            ++chars;
        }
        ++i;
    }
    if (chars <= maxChars || cut < 0)
    {
        cut = i;
        if (cut > dstSize - 1)
            cut = dstSize - 1;
        memcpy(dst, src, cut);
        dst[cut] = 0;
        return;
    }
    if (cut > dstSize - 4)
        cut = dstSize - 4;
    memcpy(dst, src, cut);
    strcpy(dst + cut, "...");
}

const char *wrap_line(const char *src, int maxChars, char *line, int lineSize)
{
    int chars = 0, i = 0, lastSpace = -1, n;

    while (src[i])
    {
        if (((unsigned char)src[i] & 0xC0) != 0x80)
        {
            if (chars == maxChars)
                break;
            if (src[i] == ' ')
                lastSpace = i;
            ++chars;
        }
        ++i;
    }
    if (src[i] && lastSpace > 0)
        i = lastSpace;

    n = (i < lineSize - 1) ? i : lineSize - 1;
    memcpy(line, src, n);
    line[n] = 0;

    while (src[i] == ' ')
        ++i;
    return src + i;
}

void utf16_to_utf8(const unsigned short *src, char *dst, int size)
{
    int n = 0;
    unsigned int cp;

    while (*src && n < size - 5)
    {
        cp = *src++;
        if (cp >= 0xD800 && cp <= 0xDBFF && *src >= 0xDC00 && *src <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (*src++ - 0xDC00);

        if (cp < 0x80)
        {
            dst[n++] = (char)cp;
        }
        else if (cp < 0x800)
        {
            dst[n++] = (char)(0xC0 | (cp >> 6));
            dst[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            dst[n++] = (char)(0xE0 | (cp >> 12));
            dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            dst[n++] = (char)(0xF0 | (cp >> 18));
            dst[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[n] = 0;
}
