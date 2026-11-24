/* avc.c - Conversion de MP4 a Annex B (ver avc.h). C89. */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "avc.h"

/* Access Unit Delimiter: NAL tipo 9, primary_pic_type 7 (cualquiera) */
static const unsigned char g_aud[2] = { 0x09, 0xF0 };

static int put_nal(unsigned char *dst, int dstCap, int pos, const unsigned char *nal, int len)
{
    if (dst)
    {
        if (pos + 4 + len > dstCap)
            return -1;
        dst[pos] = 0;
        dst[pos + 1] = 0;
        dst[pos + 2] = 0;
        dst[pos + 3] = 1;
        memcpy(dst + pos + 4, nal, len);
    }
    return pos + 4 + len;
}

int avc_to_annexb(const FmpTrack *t, const unsigned char *src, int size,
                  int withHeaders, unsigned char *dst, int dstCap)
{
    int pos = 0, in = 0, k;
    unsigned n;
    int lenSize = t->nalLengthSize;

    if (lenSize < 1 || lenSize > 4 || size <= 0)
        return -1;

    /* AUD (tipo 9) al inicio: marca donde empieza el cuadro, como lo
     * hacen los flujos de TV. Si la muestra ya trae uno, se salta abajo. */
    pos = put_nal(dst, dstCap, pos, g_aud, 2);
    if (pos < 0)
        return -1;

    if (withHeaders)
    {
        if (!t->spsLen || !t->ppsLen)
            return -1;
        pos = put_nal(dst, dstCap, pos, t->sps, t->spsLen);
        if (pos < 0)
            return -1;
        pos = put_nal(dst, dstCap, pos, t->pps, t->ppsLen);
        if (pos < 0)
            return -1;
    }

    while (in < size)
    {
        if (in + lenSize > size)
            return -1;
        n = 0;
        for (k = 0; k < lenSize; ++k)
            n = (n << 8) | src[in + k];
        in += lenSize;
        if (n == 0 || n > (unsigned)(size - in))
            return -1;
        if ((src[in] & 0x1F) == 9)   /* AUD repetido: ya pusimos uno */
        {
            in += (int)n;
            continue;
        }
        pos = put_nal(dst, dstCap, pos, src + in, (int)n);
        if (pos < 0)
            return -1;
        in += (int)n;
    }
    return pos;
}


/* ------------------------------------------------------------------ */
/* Diagnostico: lectura de SPS, PPS y encabezados de rebanada          */

/* Agrega texto al final de out sin pasarse. Devuelve la nueva longitud. */
static int app(char *out, int size, int pos, const char *fmt, ...)
{
    va_list a;
    int n;

    if (pos < 0 || pos >= size - 1)
        return pos;
    va_start(a, fmt);
    n = _vsnprintf(out + pos, size - 1 - pos, fmt, a);
    va_end(a);
    if (n < 0 || n >= size - 1 - pos)
        pos = size - 1;
    else
        pos += n;
    out[pos] = 0;
    return pos;
}

typedef struct
{
    unsigned char buf[320];   /* RBSP: el NAL sin su primer byte y sin los 03 de prevencion */
    int len;                  /* bytes validos */
    int pos;                  /* bit actual */
    int err;                  /* 1 si se intento leer mas alla del final */
} BitRd;

static void br_init(BitRd *b, const unsigned char *nal, int len)
{
    int i, zeros = 0;

    b->len = 0;
    b->pos = 0;
    b->err = 0;
    for (i = 1; i < len && b->len < (int)sizeof(b->buf); ++i)
    {
        if (zeros >= 2 && nal[i] == 3)
        {
            zeros = 0;          /* 00 00 03: el 03 no es dato */
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        b->buf[b->len++] = nal[i];
    }
}

static unsigned br_u(BitRd *b, int n)
{
    unsigned v = 0;

    while (n-- > 0)
    {
        if (b->pos >= b->len * 8)
        {
            b->err = 1;
            return v;
        }
        v = (v << 1) | ((b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        ++b->pos;
    }
    return v;
}

static unsigned br_ue(BitRd *b)
{
    int zeros = 0;

    while (!b->err && br_u(b, 1) == 0)
    {
        if (++zeros > 31)
        {
            b->err = 1;
            return 0;
        }
    }
    if (b->err)
        return 0;
    return ((1u << zeros) - 1) + br_u(b, zeros);
}

static int br_se(BitRd *b)
{
    unsigned k = br_ue(b);
    return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

/* 1 si quedan datos antes del bit de parada (el ultimo 1 del RBSP) */
static int br_more_data(const BitRd *b)
{
    int last = b->len * 8 - 1;

    while (last >= 0 && !((b->buf[last >> 3] >> (7 - (last & 7))) & 1))
        --last;
    return b->pos < last;
}

static void br_skip_scaling(BitRd *b, int size)
{
    int j, last = 8, next = 8;

    for (j = 0; j < size && !b->err; ++j)
    {
        if (next != 0)
            next = (last + br_se(b) + 256) % 256;
        last = next == 0 ? last : next;
    }
}

static void br_skip_hrd(BitRd *b)
{
    unsigned i, n = br_ue(b) + 1;

    br_u(b, 8);                            /* bit_rate_scale, cpb_size_scale */
    for (i = 0; i < n && i < 32 && !b->err; ++i)
    {
        br_ue(b);
        br_ue(b);
        br_u(b, 1);
    }
    br_u(b, 20);                           /* cuatro longitudes de 5 bits */
}

/* Cuadros que el nivel permite guardar (MaxDpbMbs / tamaño, maximo 16) */
static int level_dpb_frames(int level, int mbs)
{
    static const int lv[]  = { 9, 10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52 };
    static const int dpb[] = { 396, 396, 900, 2376, 2376, 2376, 4752, 8100, 8100, 18000, 20480,
                               32768, 32768, 34816, 110400, 184320, 184320 };
    int i, n;

    if (mbs <= 0)
        return 0;
    for (i = 0; i < (int)(sizeof(lv) / sizeof(lv[0])); ++i)
    {
        if (lv[i] == level)
        {
            n = dpb[i] / mbs;
            return n > 16 ? 16 : n;
        }
    }
    return 0;
}

int avc_describe_params(const unsigned char *sps, int spsLen,
                        const unsigned char *pps, int ppsLen,
                        char *out, int outSize)
{
    BitRd b;
    int profile, level, chroma = 1, poc, refs, wMbs, hMap, frameOnly, i, n, pos;
    int hasRestrict = 0, reorder = -1, dpb = -1, timing = 0;
    unsigned units = 0, scale = 0;
    int cabac, t8x8 = 0, wp = 0, wb = 0, groups, l0 = 0, l1 = 0;

    if (outSize <= 0)
        return 0;
    out[0] = 0;
    if (!sps || spsLen < 4)
        return 0;

    br_init(&b, sps, spsLen);
    profile = (int)br_u(&b, 8);
    br_u(&b, 8);                           /* constraint flags */
    level = (int)br_u(&b, 8);
    br_ue(&b);                             /* seq_parameter_set_id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135)
    {
        chroma = (int)br_ue(&b);
        if (chroma == 3)
            br_u(&b, 1);
        br_ue(&b);                         /* bit_depth_luma_minus8 */
        br_ue(&b);                         /* bit_depth_chroma_minus8 */
        br_u(&b, 1);                       /* qpprime_y_zero_transform_bypass */
        if (br_u(&b, 1))                   /* seq_scaling_matrix_present */
        {
            n = chroma != 3 ? 8 : 12;
            for (i = 0; i < n && !b.err; ++i)
                if (br_u(&b, 1))
                    br_skip_scaling(&b, i < 6 ? 16 : 64);
        }
    }
    br_ue(&b);                             /* log2_max_frame_num_minus4 */
    poc = (int)br_ue(&b);
    if (poc == 0)
        br_ue(&b);                         /* log2_max_pic_order_cnt_lsb_minus4 */
    else if (poc == 1)
    {
        br_u(&b, 1);
        br_se(&b);
        br_se(&b);
        n = (int)br_ue(&b);
        for (i = 0; i < n && i < 256 && !b.err; ++i)
            br_se(&b);
    }
    refs = (int)br_ue(&b);
    br_u(&b, 1);                           /* gaps_in_frame_num_allowed */
    wMbs = (int)br_ue(&b) + 1;
    hMap = (int)br_ue(&b) + 1;
    frameOnly = (int)br_u(&b, 1);
    if (!frameOnly)
        br_u(&b, 1);                       /* mb_adaptive_frame_field */
    br_u(&b, 1);                           /* direct_8x8_inference */
    if (br_u(&b, 1))                       /* frame_cropping */
    {
        br_ue(&b);
        br_ue(&b);
        br_ue(&b);
        br_ue(&b);
    }
    if (br_u(&b, 1) && !b.err)             /* vui_parameters_present */
    {
        if (br_u(&b, 1))                   /* aspect_ratio_info */
            if (br_u(&b, 8) == 255)
                br_u(&b, 32);
        if (br_u(&b, 1))                   /* overscan_info */
            br_u(&b, 1);
        if (br_u(&b, 1))                   /* video_signal_type */
        {
            br_u(&b, 4);
            if (br_u(&b, 1))
                br_u(&b, 24);
        }
        if (br_u(&b, 1))                   /* chroma_loc_info */
        {
            br_ue(&b);
            br_ue(&b);
        }
        timing = (int)br_u(&b, 1);
        if (timing)
        {
            units = br_u(&b, 32);
            scale = br_u(&b, 32);
            br_u(&b, 1);
        }
        n = 0;
        if (br_u(&b, 1))                   /* nal_hrd */
        {
            br_skip_hrd(&b);
            n = 1;
        }
        if (br_u(&b, 1))                   /* vcl_hrd */
        {
            br_skip_hrd(&b);
            n = 1;
        }
        if (n)
            br_u(&b, 1);                   /* low_delay_hrd */
        br_u(&b, 1);                       /* pic_struct_present */
        hasRestrict = (int)br_u(&b, 1);
        if (hasRestrict)
        {
            br_u(&b, 1);
            br_ue(&b);
            br_ue(&b);
            br_ue(&b);
            br_ue(&b);
            reorder = (int)br_ue(&b);
            dpb = (int)br_ue(&b);
        }
    }

    pos = app(out, outSize, 0, "SPS: perfil %d nivel %d, %dx%d MB%s, ref %d, POC tipo %d",
              profile, level, wMbs, hMap * (2 - frameOnly), frameOnly ? "" : " (entrelazado)",
              refs, poc);
    if (b.err)
        pos = app(out, outSize, pos, ", (SPS cortado: puede faltar algo)");
    else if (hasRestrict)
        pos = app(out, outSize, pos, ", reorden %d, dpb %d", reorder, dpb);
    else
        pos = app(out, outSize, pos, ", sin limite de reorden (el nivel permite %d cuadros)",
                  level_dpb_frames(level, wMbs * hMap * (2 - frameOnly)));
    if (timing && units && !b.err)
        pos = app(out, outSize, pos, ", %.2f fps", (double)scale / (2.0 * units));

    if (pps && ppsLen >= 2)
    {
        br_init(&b, pps, ppsLen);
        br_ue(&b);                         /* pic_parameter_set_id */
        br_ue(&b);                         /* seq_parameter_set_id */
        cabac = (int)br_u(&b, 1);
        br_u(&b, 1);                       /* bottom_field_pic_order_in_frame_present */
        groups = (int)br_ue(&b) + 1;
        if (groups > 1)
            pos = app(out, outSize, pos, " | PPS: %s, %d grupos de rebanadas (FMO)",
                      cabac ? "CABAC" : "CAVLC", groups);
        else
        {
            l0 = (int)br_ue(&b) + 1;
            l1 = (int)br_ue(&b) + 1;
            wp = (int)br_u(&b, 1);
            wb = (int)br_u(&b, 2);
            br_se(&b);                     /* pic_init_qp_minus26 */
            br_se(&b);                     /* pic_init_qs_minus26 */
            br_se(&b);                     /* chroma_qp_index_offset */
            br_u(&b, 1);                   /* deblocking_filter_control_present */
            br_u(&b, 1);                   /* constrained_intra_pred */
            br_u(&b, 1);                   /* redundant_pic_cnt_present */
            if (!b.err && br_more_data(&b))
                t8x8 = (int)br_u(&b, 1);
            pos = app(out, outSize, pos, " | PPS: %s, 8x8 %s, ref activas %d/%d, pesos P %d B %d%s",
                      cabac ? "CABAC" : "CAVLC", t8x8 ? "si" : "no", l0, l1, wp, wb,
                      b.err ? " (PPS cortado)" : "");
        }
    }
    return pos;
}

int avc_describe_sample(const FmpTrack *t, const unsigned char *src, int size,
                        char *out, int outSize)
{
    static const char letters[5] = { 'P', 'B', 'I', 'p', 'i' };   /* p = SP, i = SI */
    char list[48];
    int in = 0, k, type, slices = 0, firstType = -1, firstRef = 0, lp = 0, lastT = -1, run = 0;
    int lenSize = t->nalLengthSize;
    unsigned n;
    BitRd b;

    if (outSize <= 0)
        return 0;
    out[0] = 0;
    list[0] = 0;
    if (lenSize < 1 || lenSize > 4)
        return 0;

    for (;;)
    {
        type = -1;
        if (in + lenSize <= size)
        {
            n = 0;
            for (k = 0; k < lenSize; ++k)
                n = (n << 8) | src[in + k];
            in += lenSize;
            if (n > 0 && n <= (unsigned)(size - in))
            {
                type = src[in] & 0x1F;
                if (type == 1 || type == 5)
                {
                    if (slices == 0)
                    {
                        br_init(&b, src + in, n < 24 ? (int)n : 24);
                        br_ue(&b);                 /* first_mb_in_slice */
                        k = (int)br_ue(&b);        /* slice_type */
                        firstType = b.err ? -1 : k % 5;
                        firstRef = (src[in] >> 5) & 3;
                    }
                    ++slices;
                }
                in += (int)n;
            }
        }

        /* la lista se escribe al cambiar de tipo ("1x4" = cuatro seguidos) */
        if (type != lastT || type < 0)
        {
            if (lastT >= 0)
            {
                if (run > 1)
                    lp = app(list, sizeof(list), lp, "%s%dx%d", lp ? "." : "", lastT, run);
                else
                    lp = app(list, sizeof(list), lp, "%s%d", lp ? "." : "", lastT);
            }
            lastT = type;
            run = 1;
        }
        else
            ++run;
        if (type < 0)
            break;
    }

    return app(out, outSize, 0, "%c%d %ds %s", firstType >= 0 ? letters[firstType] : '-',
               firstRef, slices, list);
}
