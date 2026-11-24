/* ffstubs_ctx.c - avcodec_alloc_context() minimo para el decodificador AAC.
 *
 * El original (options.c) arrastra todo el sistema de opciones de FFmpeg.
 * El decodificador AAC solo necesita un contexto en ceros con estos pocos
 * valores. aac.c llena el resto (tipo, codec, frecuencia, canales). */

#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"

AVCodecContext *avcodec_alloc_context(void)
{
    AVCodecContext *c = (AVCodecContext *)av_mallocz(sizeof(AVCodecContext));
    if (!c)
        return 0;
    c->codec_type = AVMEDIA_TYPE_UNKNOWN;
    c->codec_id = CODEC_ID_NONE;
    c->time_base.num = 0;
    c->time_base.den = 1;
    c->sample_fmt = AV_SAMPLE_FMT_NONE;
    c->thread_count = 1;
    c->error_recognition = 1;
    return c;
}
