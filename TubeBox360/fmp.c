/* fmp.c - Lector de MP4 fragmentado (ver fmp.h). C89.
 *
 * Un MP4 esta hecho de "cajas": 4 bytes de tamaño + 4 letras de tipo +
 * contenido. Algunas cajas contienen otras (moov > trak > mdia > ...).
 * Todos los numeros van en big-endian, igual que en el PowerPC de la Xbox,
 * pero los leemos byte por byte para que funcione en cualquier CPU. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmp.h"

typedef struct
{
    char                 type[5];
    const unsigned char *data;     /* contenido (despues del encabezado) */
    int                  dataLen;
    int                  size;     /* tamaño total de la caja */
} Box;

/* ------------------------------------------------------------------ */
/* Lectura de numeros                                                  */

static unsigned rd16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned rd24(const unsigned char *p)
{
    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}

static unsigned rd32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static double rd64(const unsigned char *p)
{
    return (double)rd32(p) * 4294967296.0 + (double)rd32(p + 4);
}

static void set_err(char *err, int errSize, const char *msg)
{
    if (err && errSize > 0)
    {
        strncpy(err, msg, errSize - 1);
        err[errSize - 1] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Cajas                                                               */

/* Lee la caja que empieza en p (hay len bytes disponibles). 1 si cabe completa. */
static int read_box(const unsigned char *p, int len, Box *b)
{
    unsigned size;
    int header = 8;

    if (len < 8)
        return 0;

    size = rd32(p);
    memcpy(b->type, p + 4, 4);
    b->type[4] = 0;

    if (size == 1)
    {
        /* tamaño de 64 bits: aceptamos solo si cabe en 32 */
        if (len < 16 || rd32(p + 8) != 0)
            return 0;
        size = rd32(p + 12);
        header = 16;
    }
    else if (size == 0)
    {
        size = (unsigned)len;   /* hasta el final */
    }

    if (size < (unsigned)header || size > (unsigned)len)
        return 0;

    b->size = (int)size;
    b->data = p + header;
    b->dataLen = (int)size - header;
    return 1;
}

/* Busca la primera caja hija con ese tipo */
static int find_box(const unsigned char *p, int len, const char *type, Box *out)
{
    Box b;
    int pos = 0;

    while (pos < len && read_box(p + pos, len - pos, &b))
    {
        if (memcmp(b.type, type, 4) == 0)
        {
            *out = b;
            return 1;
        }
        pos += b.size;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* moov                                                                */

static void parse_avcc(const unsigned char *c, int len, FmpTrack *t)
{
    int pos, n, i, l;

    if (len < 7)
        return;

    t->profile = c[1];
    t->level = c[3];
    t->nalLengthSize = (c[4] & 3) + 1;

    n = c[5] & 0x1F;   /* cuantos SPS */
    pos = 6;
    for (i = 0; i < n && pos + 2 <= len; ++i)
    {
        l = (int)rd16(c + pos);
        pos += 2;
        if (pos + l > len)
            return;
        if (i == 0 && l <= (int)sizeof(t->sps))
        {
            memcpy(t->sps, c + pos, l);
            t->spsLen = l;
        }
        pos += l;
    }

    if (pos >= len)
        return;
    n = c[pos++];      /* cuantos PPS */
    for (i = 0; i < n && pos + 2 <= len; ++i)
    {
        l = (int)rd16(c + pos);
        pos += 2;
        if (pos + l > len)
            return;
        if (i == 0 && l <= (int)sizeof(t->pps))
        {
            memcpy(t->pps, c + pos, l);
            t->ppsLen = l;
        }
        pos += l;
    }
}

/* Encabezado de descriptor MPEG-4: etiqueta + tamaño de 1 a 4 bytes */
static int desc_header(const unsigned char *p, int len, int *tag, int *size)
{
    int i = 1, s = 0;

    if (len < 2)
        return 0;
    *tag = p[0];
    while (i < 5 && i < len)
    {
        s = (s << 7) | (p[i] & 0x7F);
        if (!(p[i++] & 0x80))
            break;
    }
    *size = s;
    return i;
}

static const int g_aacRates[13] =
{
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
};

static void parse_esds(const unsigned char *d, int len, FmpTrack *t)
{
    const unsigned char *p = d + 4;   /* saltar version y banderas */
    int left = len - 4;
    int tag, size, h, flags, pos, freq;

    /* ES_Descriptor (0x03) */
    h = desc_header(p, left, &tag, &size);
    if (!h || tag != 0x03 || left < h + 3)
        return;
    p += h;
    left -= h;
    flags = p[2];
    pos = 3;
    if (flags & 0x80) pos += 2;
    if ((flags & 0x40) && pos < left) pos += 1 + p[pos];
    if (flags & 0x20) pos += 2;
    if (pos >= left)
        return;
    p += pos;
    left -= pos;

    /* DecoderConfigDescriptor (0x04) */
    h = desc_header(p, left, &tag, &size);
    if (!h || tag != 0x04 || left < h + 13)
        return;
    p += h + 13;
    left -= h + 13;

    /* DecoderSpecificInfo (0x05) = AudioSpecificConfig */
    h = desc_header(p, left, &tag, &size);
    if (!h || tag != 0x05 || left < h + size || size < 2)
        return;
    p += h;

    t->ascLen = size < (int)sizeof(t->asc) ? size : (int)sizeof(t->asc);
    memcpy(t->asc, p, t->ascLen);

    t->audioObjectType = p[0] >> 3;
    freq = ((p[0] & 7) << 1) | (p[1] >> 7);
    if (freq < 13)
        t->sampleRate = g_aacRates[freq];
    if ((p[1] >> 3) & 0x0F)
        t->channels = (p[1] >> 3) & 0x0F;
}

static void parse_sample_entry(const Box *e, FmpTrack *t)
{
    Box c;

    if (memcmp(e->type, "avc1", 4) == 0 || memcmp(e->type, "avc3", 4) == 0)
    {
        if (e->dataLen < 78)
            return;
        t->isVideo = 1;
        t->width = (int)rd16(e->data + 24);
        t->height = (int)rd16(e->data + 26);
        if (find_box(e->data + 78, e->dataLen - 78, "avcC", &c))
            parse_avcc(c.data, c.dataLen, t);
    }
    else if (memcmp(e->type, "mp4a", 4) == 0)
    {
        if (e->dataLen < 28)
            return;
        t->isAudio = 1;
        t->channels = (int)rd16(e->data + 16);
        t->sampleRate = (int)(rd32(e->data + 24) >> 16);
        if (find_box(e->data + 28, e->dataLen - 28, "esds", &c))
            parse_esds(c.data, c.dataLen, t);
    }
}

static void parse_trak(const Box *trak, FmpTrack *t)
{
    Box b, mdia, minf, stbl, stsd, entry;
    unsigned duration;

    if (find_box(trak->data, trak->dataLen, "tkhd", &b) && b.dataLen >= 24)
        t->trackId = rd32(b.data + (b.data[0] == 1 ? 20 : 12));

    if (!find_box(trak->data, trak->dataLen, "mdia", &mdia))
        return;

    if (find_box(mdia.data, mdia.dataLen, "mdhd", &b) && b.dataLen >= 24)
    {
        if (b.data[0] == 1 && b.dataLen >= 32)
        {
            t->timescale = rd32(b.data + 20);
            if (t->timescale)
                t->durationSec = rd64(b.data + 24) / t->timescale;
        }
        else
        {
            t->timescale = rd32(b.data + 12);
            duration = rd32(b.data + 16);
            if (t->timescale && duration != 0xFFFFFFFF)
                t->durationSec = (double)duration / t->timescale;
        }
    }

    if (find_box(mdia.data, mdia.dataLen, "minf", &minf) &&
        find_box(minf.data, minf.dataLen, "stbl", &stbl) &&
        find_box(stbl.data, stbl.dataLen, "stsd", &stsd) &&
        stsd.dataLen > 8 &&
        read_box(stsd.data + 8, stsd.dataLen - 8, &entry))
    {
        parse_sample_entry(&entry, t);
    }
}

static void parse_moov(const Box *moov, FmpTrack *t)
{
    Box b, trex;
    int pos = 0;

    while (pos < moov->dataLen && read_box(moov->data + pos, moov->dataLen - pos, &b))
    {
        /* YouTube pone una sola pista por archivo; nos quedamos con la primera util */
        if (memcmp(b.type, "trak", 4) == 0 && !t->isVideo && !t->isAudio)
            parse_trak(&b, t);
        else if (memcmp(b.type, "mvex", 4) == 0 &&
                 find_box(b.data, b.dataLen, "trex", &trex) && trex.dataLen >= 24)
        {
            t->defaultSampleDuration = rd32(trex.data + 12);
            t->defaultSampleSize     = rd32(trex.data + 16);
            t->defaultSampleFlags    = rd32(trex.data + 20);
        }
        pos += b.size;
    }
}

/* sidxEnd: posicion en el archivo donde termina la caja sidx */
static int parse_sidx(const Box *b, double sidxEnd, FmpTrack *t)
{
    const unsigned char *d = b->data;
    int pos, count, i;
    double offset;
    unsigned a;

    if (b->dataLen < 24)
        return 0;

    t->sidxTimescale = rd32(d + 8);
    if (d[0] == 0)
    {
        t->firstTimeSec = t->sidxTimescale ? (double)rd32(d + 12) / t->sidxTimescale : 0.0;
        offset = sidxEnd + rd32(d + 16);
        pos = 20;
    }
    else
    {
        if (b->dataLen < 32)
            return 0;
        t->firstTimeSec = t->sidxTimescale ? rd64(d + 12) / t->sidxTimescale : 0.0;
        offset = sidxEnd + rd64(d + 20);
        pos = 28;
    }

    count = (int)rd16(d + pos + 2);
    pos += 4;
    if (count <= 0 || pos + count * 12 > b->dataLen)
        return 0;

    t->segments = (FmpSegment *)malloc(sizeof(FmpSegment) * count);
    if (!t->segments)
        return 0;

    for (i = 0; i < count; ++i)
    {
        a = rd32(d + pos);
        t->segments[i].offset = offset;
        t->segments[i].size = a & 0x7FFFFFFF;
        t->segments[i].duration = rd32(d + pos + 4);
        t->segments[i].startsWithKey = (int)(rd32(d + pos + 8) >> 31);
        offset += t->segments[i].size;
        pos += 12;
    }
    t->segmentCount = count;
    return 1;
}

int fmp_parse_init(const unsigned char *buf, int len, double fileOffset,
                   FmpTrack *t, char *err, int errSize)
{
    Box b;
    int pos = 0, haveMoov = 0;

    memset(t, 0, sizeof(*t));

    while (pos < len && read_box(buf + pos, len - pos, &b))
    {
        if (memcmp(b.type, "moov", 4) == 0)
        {
            parse_moov(&b, t);
            haveMoov = 1;
        }
        else if (memcmp(b.type, "sidx", 4) == 0 && !t->segments)
        {
            parse_sidx(&b, fileOffset + pos + b.size, t);
        }
        pos += b.size;
    }

    if (!haveMoov)
    {
        set_err(err, errSize, "No se encontro el moov");
        return 0;
    }
    if (!t->isVideo && !t->isAudio)
    {
        set_err(err, errSize, "La pista no es H.264 ni AAC");
        return 0;
    }
    if (t->isVideo && (!t->spsLen || !t->ppsLen))
    {
        set_err(err, errSize, "Falta SPS/PPS en avcC");
        return 0;
    }
    if (!t->segments)
    {
        set_err(err, errSize, "No se encontro el indice (sidx)");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Fragmentos                                                          */

static void parse_traf(const Box *traf, int moofPos, const unsigned char *buf, int len,
                       const FmpTrack *t, FmpFragment *f, double *baseTime)
{
    Box b;
    const unsigned char *d;
    unsigned flags, count, i, dur, size, sflags, firstFlags = 0;
    unsigned defDur = t->defaultSampleDuration;
    unsigned defSize = t->defaultSampleSize;
    unsigned defFlags = t->defaultSampleFlags;
    int pos, dataOffset, first = (f->sampleCount == 0);

    if (find_box(traf->data, traf->dataLen, "tfhd", &b) && b.dataLen >= 8)
    {
        d = b.data;
        flags = rd24(d + 1);
        pos = 8;
        if (flags & 0x01) pos += 8;                     /* base_data_offset */
        if (flags & 0x02) pos += 4;                     /* sample_description_index */
        if ((flags & 0x08) && pos + 4 <= b.dataLen) { defDur = rd32(d + pos);   pos += 4; }
        if ((flags & 0x10) && pos + 4 <= b.dataLen) { defSize = rd32(d + pos);  pos += 4; }
        if ((flags & 0x20) && pos + 4 <= b.dataLen) { defFlags = rd32(d + pos); pos += 4; }
    }

    if (find_box(traf->data, traf->dataLen, "tfdt", &b) && b.dataLen >= 8)
        *baseTime = (b.data[0] == 1 && b.dataLen >= 12) ? rd64(b.data + 4) : (double)rd32(b.data + 4);

    if (!find_box(traf->data, traf->dataLen, "trun", &b) || b.dataLen < 8)
        return;

    d = b.data;
    flags = rd24(d + 1);
    count = rd32(d + 4);
    pos = 8;
    dataOffset = 0;
    if (flags & 0x001) { dataOffset = (int)rd32(d + pos); pos += 4; }
    if (flags & 0x004) { firstFlags = rd32(d + pos);      pos += 4; }

    for (i = 0; i < count; ++i)
    {
        int need = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) +
                   ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
        if (pos + need > b.dataLen)
            break;

        dur = defDur;
        size = defSize;
        sflags = (i == 0 && (flags & 0x004)) ? firstFlags : defFlags;

        if (flags & 0x100) { dur = rd32(d + pos);    pos += 4; }
        if (flags & 0x200) { size = rd32(d + pos);   pos += 4; }
        if (flags & 0x400) { sflags = rd32(d + pos); pos += 4; }
        if (flags & 0x800) pos += 4;                 /* desfase de composicion */

        if (first && i == 0)
        {
            f->firstSampleSize = size;
            f->firstSampleOffset = moofPos + dataOffset;   /* base = inicio del moof */
        }

        f->sampleCount++;
        f->totalBytes += size;
        f->durationSec += t->timescale ? (double)dur / t->timescale : 0.0;
        if (!(sflags & 0x00010000))                    /* sample_is_non_sync_sample */
            f->keyframes++;
    }
    (void)buf;
    (void)len;
}

int fmp_parse_fragment(const unsigned char *buf, int len, const FmpTrack *t,
                       FmpFragment *f, char *err, int errSize)
{
    Box b, traf;
    int pos = 0, inner, haveMoof = 0;
    double baseTime = 0.0;
    const unsigned char *p, *end;
    unsigned n;
    int k;

    memset(f, 0, sizeof(*f));
    f->firstSampleOffset = -1;

    while (pos < len && read_box(buf + pos, len - pos, &b))
    {
        if (memcmp(b.type, "moof", 4) == 0)
        {
            haveMoof = 1;
            inner = 0;
            while (inner < b.dataLen && read_box(b.data + inner, b.dataLen - inner, &traf))
            {
                if (memcmp(traf.type, "traf", 4) == 0)
                    parse_traf(&traf, pos, buf, len, t, f, &baseTime);
                inner += traf.size;
            }
        }
        pos += b.size;
    }

    if (!haveMoof || f->sampleCount == 0)
    {
        set_err(err, errSize, "El fragmento no trae moof con muestras");
        return 0;
    }
    f->startSec = t->timescale ? baseTime / t->timescale : 0.0;

    if (f->firstSampleOffset < 0 || f->firstSampleOffset + (int)f->firstSampleSize > len)
    {
        set_err(err, errSize, "La primera muestra queda fuera del fragmento");
        return 0;
    }

    /* tipos de NAL del primer cuadro: 5 = cuadro clave (IDR), 1 = normal,
     * 6 = SEI (informacion extra), 7/8 = SPS/PPS, 9 = separador */
    if (t->isVideo && t->nalLengthSize >= 1 && t->nalLengthSize <= 4)
    {
        p = buf + f->firstSampleOffset;
        end = p + f->firstSampleSize;
        while (f->nalCount < 16 && p + t->nalLengthSize <= end)
        {
            n = 0;
            for (k = 0; k < t->nalLengthSize; ++k)
                n = (n << 8) | p[k];
            p += t->nalLengthSize;
            if (n == 0 || p + n > end)
                break;
            f->nalTypes[f->nalCount++] = p[0] & 0x1F;
            p += n;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Tabla de muestras                                                   */

static int traf_samples(const Box *traf, int moofPos, int len, const FmpTrack *t,
                        FmpSample *out, int cap, int n)
{
    Box b;
    const unsigned char *d;
    unsigned flags, count, i, dur, size, sflags, firstFlags;
    unsigned defDur = t->defaultSampleDuration;
    unsigned defSize = t->defaultSampleSize;
    unsigned defFlags = t->defaultSampleFlags;
    int pos, inner, offset = -1, version;
    double time = 0.0, cto;

    if (find_box(traf->data, traf->dataLen, "tfhd", &b) && b.dataLen >= 8)
    {
        d = b.data;
        flags = rd24(d + 1);
        pos = 8;
        if (flags & 0x01) pos += 8;
        if (flags & 0x02) pos += 4;
        if ((flags & 0x08) && pos + 4 <= b.dataLen) { defDur = rd32(d + pos);   pos += 4; }
        if ((flags & 0x10) && pos + 4 <= b.dataLen) { defSize = rd32(d + pos);  pos += 4; }
        if ((flags & 0x20) && pos + 4 <= b.dataLen) { defFlags = rd32(d + pos); pos += 4; }
    }

    if (find_box(traf->data, traf->dataLen, "tfdt", &b) && b.dataLen >= 8)
        time = (b.data[0] == 1 && b.dataLen >= 12) ? rd64(b.data + 4) : (double)rd32(b.data + 4);

    /* puede haber varias cajas trun seguidas */
    inner = 0;
    while (inner < traf->dataLen && read_box(traf->data + inner, traf->dataLen - inner, &b))
    {
        inner += b.size;
        if (memcmp(b.type, "trun", 4) != 0 || b.dataLen < 8)
            continue;

        d = b.data;
        version = d[0];
        flags = rd24(d + 1);
        count = rd32(d + 4);
        pos = 8;
        firstFlags = defFlags;
        if (flags & 0x001) { offset = moofPos + (int)rd32(d + pos); pos += 4; }
        if (flags & 0x004) { firstFlags = rd32(d + pos);           pos += 4; }
        if (offset < 0)
            return -1;   /* sin data_offset no sabemos donde estan los datos */

        for (i = 0; i < count; ++i)
        {
            int need = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) +
                       ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
            if (pos + need > b.dataLen)
                return -1;

            dur = defDur;
            size = defSize;
            sflags = (i == 0) ? firstFlags : defFlags;
            cto = 0.0;
            if (flags & 0x100) { dur = rd32(d + pos);    pos += 4; }
            if (flags & 0x200) { size = rd32(d + pos);   pos += 4; }
            if (flags & 0x400) { sflags = rd32(d + pos); pos += 4; }
            if (flags & 0x800)
            {
                /* version 1: desfase con signo; version 0: sin signo */
                cto = version ? (double)(int)rd32(d + pos) : (double)rd32(d + pos);
                pos += 4;
            }

            if (size > (unsigned)len || offset > len - (int)size)
                return -1;

            if (n < cap)
            {
                out[n].offset = offset;
                out[n].size = size;
                out[n].dts = t->timescale ? time / t->timescale : 0.0;
                out[n].pts = t->timescale ? (time + cto) / t->timescale : 0.0;
                out[n].key = !(sflags & 0x00010000);
                ++n;
            }
            offset += (int)size;
            time += dur;
        }
    }
    return n;
}

int fmp_fragment_samples(const unsigned char *buf, int len, const FmpTrack *t,
                         FmpSample *out, int cap)
{
    Box b, traf;
    int pos = 0, inner, n = 0;

    while (pos < len && read_box(buf + pos, len - pos, &b))
    {
        if (memcmp(b.type, "moof", 4) == 0)
        {
            inner = 0;
            while (inner < b.dataLen && read_box(b.data + inner, b.dataLen - inner, &traf))
            {
                if (memcmp(traf.type, "traf", 4) == 0)
                {
                    n = traf_samples(&traf, pos, len, t, out, cap, n);
                    if (n < 0)
                        return -1;
                }
                inner += traf.size;
            }
        }
        pos += b.size;
    }
    return n;
}

void fmp_free(FmpTrack *t)
{
    if (!t)
        return;
    free(t->segments);
    t->segments = NULL;
    t->segmentCount = 0;
}

const char *fmp_profile_name(int profile)
{
    switch (profile)
    {
    case 66:  return "Baseline";
    case 77:  return "Main";
    case 88:  return "Extended";
    case 100: return "High";
    case 110: return "High 10";
    case 122: return "High 4:2:2";
    case 244: return "High 4:4:4";
    default:  return "?";
    }
}
