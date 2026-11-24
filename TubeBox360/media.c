/* media.c - Descarga y analisis de pistas (ver media.h). C89. */

#include <stdio.h>
#include <string.h>
#include "media.h"
#include "lang.h"
#include "net.h"

static void set_err(char *err, int errSize, const char *msg)
{
    if (err && errSize > 0)
    {
        strncpy(err, msg, errSize - 1);
        err[errSize - 1] = 0;
    }
}

/* GET con Range: bytes=start-end. Devuelve 1 si llego 206 (o 200). */
int media_fetch_range(const char *url, double start, double end, HttpResponse *r,
                       char *err, int errSize)
{
    char headers[256];

    _snprintf(headers, sizeof(headers) - 1,
              "User-Agent: " YT_VISION_UA "\r\n"
              "Range: bytes=%.0f-%.0f\r\n", start, end);
    headers[sizeof(headers) - 1] = 0;

    if (!http_request("GET", url, headers, NULL, 0, r, err, errSize))
        return 0;

    if (r->status != 206 && r->status != 200)
    {
        _snprintf(err, errSize - 1, "googlevideo respondio HTTP %d", r->status);
        err[errSize - 1] = 0;
        http_free(r);
        return 0;
    }
    return 1;
}

static void load_stream(const YtPlayer *p, int idx, MediaStream *s)
{
    const YtFormat *f;
    const FmpSegment *seg;
    HttpResponse r;

    s->fmtIndex = idx;
    if (idx < 0 || idx >= p->formatCount)
    {
        set_err(s->err, sizeof(s->err), "No hay formato para esta pista");
        return;
    }
    f = &p->formats[idx];
    s->itag = f->itag;

    if (!f->url[0])
    {
        set_err(s->err, sizeof(s->err), "El formato no trae URL directa");
        return;
    }
    if (f->indexEnd <= f->initStart)
    {
        set_err(s->err, sizeof(s->err), "El formato no trae initRange/indexRange");
        return;
    }

    /* 1. encabezado + indice: estan juntos al inicio del archivo */
    if (!media_fetch_range(f->url, f->initStart, f->indexEnd, &r, s->err, sizeof(s->err)))
        return;
    s->bytes += r.bodyLen;
    if (!fmp_parse_init((const unsigned char *)r.body, r.bodyLen, f->initStart,
                        &s->track, s->err, sizeof(s->err)))
    {
        http_free(&r);
        return;
    }
    http_free(&r);

    /* 2. primer fragmento (moof + mdat) */
    seg = &s->track.segments[0];
    if (!media_fetch_range(f->url, seg->offset, seg->offset + seg->size - 1, &r, s->err, sizeof(s->err)))
        return;
    s->bytes += r.bodyLen;
    if (!fmp_parse_fragment((const unsigned char *)r.body, r.bodyLen, &s->track,
                            &s->frag, s->err, sizeof(s->err)))
    {
        http_free(&r);
        return;
    }
    http_free(&r);

    s->ok = 1;
}

int media_load_init(const YtFormat *f, FmpTrack *t, unsigned *bytes, char *err, int errSize)
{
    HttpResponse r;
    int ok;

    memset(t, 0, sizeof(*t));
    if (!f->url[0])
    {
        set_err(err, errSize, "El formato no trae URL directa");
        return 0;
    }
    if (f->indexEnd <= f->initStart)
    {
        set_err(err, errSize, "El formato no trae initRange/indexRange");
        return 0;
    }
    if (!media_fetch_range(f->url, f->initStart, f->indexEnd, &r, err, errSize))
        return 0;
    if (bytes)
        *bytes += r.bodyLen;
    ok = fmp_parse_init((const unsigned char *)r.body, r.bodyLen, f->initStart, t, err, errSize);
    http_free(&r);
    return ok;
}

int g_prefAudio = PREF_AUDIO_APP;
int g_prefSubs = PREF_SUBS_AUTO;

static const char *pref_lang(int pref)
{
    switch (pref)
    {
    case PREF_AUDIO_ES: return "es";
    case PREF_AUDIO_EN: return "en";
    case PREF_AUDIO_PT: return "pt";
    case PREF_AUDIO_JA: return "ja";
    case PREF_AUDIO_AR: return "ar";
    default:            return APP_AUDIO_LANG;
    }
}

int media_default_track(const YtPlayer *p)
{
    int i;
    const char *want;

    if (p->trackCount <= 0)
        return -1;
    if (g_prefAudio == PREF_AUDIO_ORIGINAL)
    {
        for (i = 0; i < p->trackCount; ++i)
            if (p->tracks[i].isOriginal)
                return i;
        return 0;
    }
    want = pref_lang(g_prefAudio);
    for (i = 0; i < p->trackCount; ++i)
        if (strcmp(p->tracks[i].lang, want) == 0)
            return i;
    return 0;   /* la original va primero */
}

int media_default_caption(const YtPlayer *p, int track)
{
    int i;
    const char *want;

    if (g_prefSubs == PREF_SUBS_OFF)
        return -1;
    if (g_prefSubs != PREF_SUBS_AUTO)
    {
        /* un idioma fijo: siempre que el video los tenga */
        want = g_prefSubs == PREF_SUBS_ES ? "es" : g_prefSubs == PREF_SUBS_EN ? "en" :
               g_prefSubs == PREF_SUBS_PT ? "pt" : g_prefSubs == PREF_SUBS_JA ? "ja" : "ar";
        for (i = 0; i < p->captionCount; ++i)
            if (strcmp(p->captions[i].lang, want) == 0)
                return i;
        return -1;
    }
    /* automaticos: solo si el audio no es en el idioma de la app */
    if (track < 0 || track >= p->trackCount ||
        strcmp(p->tracks[track].lang, APP_AUDIO_LANG) == 0)
        return -1;
    for (i = 0; i < p->captionCount; ++i)
        if (strcmp(p->captions[i].lang, APP_AUDIO_LANG) == 0)
            return i;
    return -1;
}

void media_track_name(const YtPlayer *p, int track, char *out, int size)
{
    const YtAudioTrack *t;

    if (track < 0 || track >= p->trackCount)
    {
        _snprintf(out, size - 1, "%s", T(S_SINGLE_TRACK_C));
        out[size - 1] = 0;
        return;
    }
    t = &p->tracks[track];
    if (t->isAutoDubbed)
        _snprintf(out, size - 1, T(S_AUTODUB), t->name[0] ? t->name : t->id);
    else if (t->isOriginal && !strstr(t->name, "original"))
        _snprintf(out, size - 1, T(S_ORIGINAL), t->name[0] ? t->name : t->id);
    else
        _snprintf(out, size - 1, "%s", t->name[0] ? t->name : t->id);
    out[size - 1] = 0;
}

int media_pick_audio(const YtPlayer *p, int track)
{
    int i, first = -1;

    for (i = 0; i < p->formatCount; ++i)
    {
        if (!p->formats[i].isAudio || strncmp(p->formats[i].codecs, "mp4a", 4) != 0)
            continue;
        if (track >= 0 && p->formats[i].track != track)
            continue;
        if (p->formats[i].itag == 140)
            return i;
        if (first < 0)
            first = i;
    }
    return first;
}

int media_probe(const YtPlayer *p, int videoIdx, MediaProbe *m, char *err, int errSize)
{
    memset(m, 0, sizeof(*m));
    m->video.fmtIndex = -1;
    m->audio.fmtIndex = -1;

    if (videoIdx < 0 || videoIdx >= p->formatCount || !p->formats[videoIdx].isVideo ||
        strncmp(p->formats[videoIdx].codecs, "avc1", 4) != 0)
    {
        if (videoIdx >= 0 && videoIdx < p->formatCount)
            m->video.itag = p->formats[videoIdx].itag;
        set_err(m->video.err, sizeof(m->video.err), "Elige un formato de video H.264 (en blanco)");
        set_err(m->audio.err, sizeof(m->audio.err), "No analizado");
        set_err(err, errSize, m->video.err);
        return 0;
    }

    load_stream(p, videoIdx, &m->video);
    load_stream(p, media_pick_audio(p, media_default_track(p)), &m->audio);
    m->totalBytes = m->video.bytes + m->audio.bytes;

    if (!m->video.ok || !m->audio.ok)
    {
        set_err(err, errSize, !m->video.ok ? m->video.err : m->audio.err);
        return 0;
    }
    return 1;
}

void media_free(MediaProbe *m)
{
    fmp_free(&m->video.track);
    fmp_free(&m->audio.track);
}
