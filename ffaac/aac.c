/* aac.c - Decodificador de audio AAC (ver aac.h). C89.
 * Usa la API de libavcodec 52 (FFmpeg 0.6/0.7) de ffmpeg360. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "aac.h"

extern AVCodec ff_aac_decoder;

struct AacDec
{
    AVCodecContext *ctx;
    unsigned char  *in;      /* copia del cuadro con relleno al final */
    short          *out;     /* FFmpeg exige un buffer de salida de 192000 bytes */
    int             inCap;
    int             rate;
    int             channels;
};

static int g_registered = 0;

static void set_err(char *err, int errSize, const char *msg)
{
    if (err && errSize > 0)
    {
        strncpy(err, msg, errSize - 1);
        err[errSize - 1] = 0;
    }
}

AacDec *aac_open(const unsigned char *asc, int ascLen, int sampleRate, int channels,
                 char *err, int errSize)
{
    AacDec *d;

    if (!g_registered)
    {
        /* solo registramos el decodificador AAC: asi el enlazador no mete
         * los demas codecs de FFmpeg */
        avcodec_init();
        avcodec_register(&ff_aac_decoder);
        g_registered = 1;
    }

    d = (AacDec *)calloc(1, sizeof(AacDec));
    if (!d)
    {
        set_err(err, errSize, "Sin memoria");
        return NULL;
    }
    d->ctx = avcodec_alloc_context();
    if (!d->ctx)
    {
        free(d);
        set_err(err, errSize, "avcodec_alloc_context fallo");
        return NULL;
    }
    d->ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    d->ctx->codec_id = CODEC_ID_AAC;
    d->ctx->sample_rate = sampleRate;
    d->ctx->channels = channels;
    if (asc && ascLen > 0)
    {
        d->ctx->extradata = (uint8_t *)av_mallocz(ascLen + FF_INPUT_BUFFER_PADDING_SIZE);
        if (d->ctx->extradata)
        {
            memcpy(d->ctx->extradata, asc, ascLen);
            d->ctx->extradata_size = ascLen;
        }
    }
    if (avcodec_open(d->ctx, &ff_aac_decoder) < 0)
    {
        av_freep(&d->ctx->extradata);
        av_free(d->ctx);
        free(d);
        set_err(err, errSize, "El decodificador AAC no acepto la configuracion");
        return NULL;
    }
    d->out = (short *)av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);
    if (!d->out)
    {
        aac_close(d);
        set_err(err, errSize, "Sin memoria para el audio");
        return NULL;
    }
    d->rate = d->ctx->sample_rate;
    d->channels = d->ctx->channels;
    return d;
}

int aac_decode(AacDec *d, const unsigned char *frame, int len, short *pcm, int cap)
{
    AVPacket pkt;
    int outBytes, used, n;

    if (!d || len <= 0)
        return -1;
    if (len + FF_INPUT_BUFFER_PADDING_SIZE > d->inCap)
    {
        free(d->in);
        d->inCap = len + FF_INPUT_BUFFER_PADDING_SIZE + 1024;
        d->in = (unsigned char *)malloc(d->inCap);
        if (!d->in)
        {
            d->inCap = 0;
            return -1;
        }
    }
    memcpy(d->in, frame, len);
    memset(d->in + len, 0, FF_INPUT_BUFFER_PADDING_SIZE);

    av_init_packet(&pkt);
    pkt.data = d->in;
    pkt.size = len;
    outBytes = AVCODEC_MAX_AUDIO_FRAME_SIZE;
    used = avcodec_decode_audio3(d->ctx, (int16_t *)d->out, &outBytes, &pkt);
    if (used < 0)
        return -1;
    n = outBytes / (int)sizeof(short);
    if (n > cap)
        n = cap;
    memcpy(pcm, d->out, n * sizeof(short));
    if (d->ctx->sample_rate > 0)
        d->rate = d->ctx->sample_rate;
    if (d->ctx->channels > 0)
        d->channels = d->ctx->channels;
    return n;
}

int aac_rate(const AacDec *d)
{
    return d ? d->rate : 0;
}

int aac_channels(const AacDec *d)
{
    return d ? d->channels : 0;
}

void aac_close(AacDec *d)
{
    if (!d)
        return;
    avcodec_close(d->ctx);
    av_freep(&d->ctx->extradata);
    av_free(d->ctx);
    av_free(d->out);
    free(d->in);
    free(d);
}
