// sfx.cpp - Sonidos de la interfaz (ver sfx.h). C++ porque XAudio2 se usa
// con metodos; se expone en C.
//
// Cada sonido tiene 2 voces de XAudio2 que se turnan: si se repite rapido
// (teclear, recorrer la lista con la palanca) el anterior termina solo y el
// nuevo empieza en la otra voz, sin cortes.
//
// Los sintetizados son mono, 44.1 kHz, 16 bits. En la 360 el PCM va en el
// orden de bytes de la consola (big-endian): como se calculan aqui como
// short, ya quedan asi. Los .wav vienen en little-endian y se voltean.

#include <xtl.h>
#include <xaudio2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "audio.h"
#include "sfx.h"
#include "ui_assets.h"

#define RATE      44100
#define VOICES    2
#define MAX_LEN   (RATE / 2)        // medio segundo como mucho (sintetizados)
#define WAV_MAX   (RATE * 2 * 2)    // 2 s estereo como mucho (archivos)
#define VOLUME    0.55f

struct Sound
{
    const short          *pcm;
    int                   frames;
    int                   channels;
    int                   rate;
    IXAudio2SourceVoice  *voice[VOICES];
    int                   next;
};

static Sound  g_snd[SFX_COUNT];
static short  g_keyVar[3][MAX_LEN];   // 3 variantes de la tecla (suena menos robotico)
static int    g_keyLen[3];
static int    g_keyTurn = 0;
static int    g_ok = 0;
static int    g_on = 1;
static int    g_playedFrame = 0;
static unsigned int g_seed = 0x1234567u;

static const char *kFiles[SFX_COUNT] = {
    "mover", "elegir", "volver", "menu_abrir", "menu_cerrar", "tecla", "clic",
    "fila", "resultados", "salir"
};

// Los del usuario (ui_assets.c); el resto se sintetiza
static int embedded(int id, Sound *s)
{
#define USE(n) s->pcm = g_snd_##n##_pcm; s->frames = g_snd_##n##_frames; \
               s->channels = g_snd_##n##_channels; s->rate = g_snd_##n##_rate; return 1
    switch (id)
    {
    case SFX_MOVE:       USE(mover);
    case SFX_ROW:        USE(fila);
    case SFX_MENU_OPEN:
    case SFX_MENU_CLOSE: USE(menu);
    case SFX_RESULTS:    USE(resultados);
    case SFX_EXIT:       USE(salir);
    }
#undef USE
    return 0;
}

// ------------------------------------------------------------ sintesis

static float frand(void)            // ruido blanco en [-1, 1]
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)((g_seed >> 9) & 0x7FFF) / 16383.5f - 1.0f;
}

// Filtro pasa-banda (biquad RBJ) para el ruido de los barridos y clics
struct Biquad
{
    float b0, b1, b2, a1, a2, x1, x2, y1, y2;
    void bandpass(float f, float q)
    {
        float w = 6.2831853f * f / RATE, al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
        b0 = al / a0; b1 = 0.0f; b2 = -al / a0;
        a1 = -2.0f * cosf(w) / a0; a2 = (1.0f - al) / a0;
    }
    void highpass(float f, float q)
    {
        float w = 6.2831853f * f / RATE, al = sinf(w) / (2.0f * q), cw = cosf(w), a0 = 1.0f + al;
        b0 = (1.0f + cw) * 0.5f / a0; b1 = -(1.0f + cw) / a0; b2 = b0;
        a1 = -2.0f * cw / a0; a2 = (1.0f - al) / a0;
    }
    float run(float x)
    {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    void reset() { x1 = x2 = y1 = y2 = 0.0f; }
};

static void to_pcm(const float *f, int n, short *out, float gain)
{
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
        if (fabsf(f[i]) > peak)
            peak = fabsf(f[i]);
    float k = peak > 0.0f ? gain * 32000.0f / peak : 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float v = f[i] * k;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (short)v;
    }
}

static float g_tmp[MAX_LEN];

// Tono con caida exponencial y ataque suave (campanita)
static void add_tone(float *buf, int n, float t0, float freq, float freqEnd, float dur, float decay, float amp)
{
    int s0 = (int)(t0 * RATE), len = (int)(dur * RATE);
    float ph = 0.0f;
    for (int i = 0; i < len && s0 + i < n; ++i)
    {
        float t = (float)i / RATE, f = freq + (freqEnd - freq) * ((float)i / len);
        float env = (1.0f - expf(-t / 0.002f)) * expf(-t / decay);
        float tail = (float)(len - i) / (0.010f * RATE);   // se apaga en los ultimos 10 ms (sin chasquido)
        if (tail < 1.0f)
            env *= tail;
        ph += 6.2831853f * f / RATE;
        buf[s0 + i] += amp * env * (sinf(ph) + 0.25f * sinf(2.0f * ph));
    }
}

// Ruido filtrado cuya frecuencia central va de f0 a f1 (barrido "swoosh")
static void add_sweep(float *buf, int n, float dur, float f0, float f1, float amp)
{
    Biquad bq, bq2; bq.reset(); bq2.reset();   // dos pasadas: el barrido suena menos a siseo
    int len = (int)(dur * RATE);
    for (int i = 0; i < len && i < n; ++i)
    {
        float x = (float)i / len;
        if ((i & 31) == 0)
        {
            bq.bandpass(f0 * powf(f1 / f0, x), 3.0f);
            bq2.bandpass(f0 * powf(f1 / f0, x), 3.0f);
        }
        float env = sinf(3.1415926f * powf(x, 0.7f));   // sube y baja
        buf[i] += amp * env * bq2.run(bq.run(frand()));
    }
}

// Clic corto: chasquido de ruido agudo + resonancia
static void add_click(float *buf, int n, float t0, float hp, float res, float resDecay, float amp)
{
    Biquad h; h.reset(); h.highpass(hp, 0.7f);
    int s0 = (int)(t0 * RATE);
    float ph = 0.0f;
    for (int i = 0; i < (int)(0.05f * RATE) && s0 + i < n; ++i)
    {
        float t = (float)i / RATE;
        float noise = h.run(frand()) * expf(-t / 0.0012f);
        ph += 6.2831853f * res / RATE;
        float body = sinf(ph) * expf(-t / resDecay) * 0.6f;
        buf[s0 + i] += amp * (noise + body);
    }
}

static int make(int id, float durSec)
{
    int n = (int)(durSec * RATE);
    if (n > MAX_LEN)
        n = MAX_LEN;
    memset(g_tmp, 0, sizeof(float) * n);
    switch (id)
    {
    case SFX_MOVE:          // "tic" suave y grave: no cansa al recorrer
        add_tone(g_tmp, n, 0.0f, 1500.0f, 1150.0f, 0.045f, 0.012f, 0.8f);
        add_tone(g_tmp, n, 0.0f, 420.0f, 380.0f, 0.050f, 0.018f, 0.5f);
        break;
    case SFX_SELECT:        // dos notas que suben (campanita)
        add_tone(g_tmp, n, 0.000f, 1046.5f, 1046.5f, 0.12f, 0.045f, 0.8f);
        add_tone(g_tmp, n, 0.055f, 1568.0f, 1568.0f, 0.16f, 0.060f, 0.9f);
        break;
    case SFX_BACK:          // dos notas que bajan
        add_tone(g_tmp, n, 0.000f, 1174.7f, 1174.7f, 0.10f, 0.040f, 0.8f);
        add_tone(g_tmp, n, 0.050f, 784.0f, 784.0f, 0.14f, 0.055f, 0.8f);
        break;
    case SFX_MENU_OPEN:     // barrido que sube + brillo al final
        add_sweep(g_tmp, n, 0.24f, 380.0f, 3200.0f, 1.0f);
        add_tone(g_tmp, n, 0.17f, 2093.0f, 2093.0f, 0.12f, 0.050f, 0.18f);
        break;
    case SFX_MENU_CLOSE:    // barrido que baja
        add_sweep(g_tmp, n, 0.20f, 3000.0f, 420.0f, 1.0f);
        break;
    case SFX_CLICK:         // clic de mouse: al bajar y, mas suave, al soltar
        add_click(g_tmp, n, 0.000f, 2500.0f, 3600.0f, 0.004f, 1.0f);
        add_click(g_tmp, n, 0.075f, 3000.0f, 4200.0f, 0.003f, 0.45f);
        break;
    }
    return n;
}

// Tecla mecanica: chasquido del mecanismo, golpe ("thock") de la tecla al
// fondo y un clic chico al regresar. v = variante (cambia tono y tiempos).
static int make_key(int v, short *out)
{
    static const float pitch[3] = { 1.0f, 0.93f, 1.07f };
    int n = (int)(0.11f * RATE);
    float p = pitch[v];
    memset(g_tmp, 0, sizeof(float) * n);
    add_click(g_tmp, n, 0.000f, 3500.0f * p, 2600.0f * p, 0.006f, 1.0f);    // clic del interruptor
    add_tone(g_tmp, n, 0.006f, 210.0f * p, 170.0f * p, 0.05f, 0.012f, 0.9f); // golpe al fondo
    add_click(g_tmp, n, 0.008f, 1200.0f * p, 1150.0f * p, 0.010f, 0.6f);
    add_click(g_tmp, n, 0.062f + 0.006f * v, 4000.0f * p, 3000.0f * p, 0.003f, 0.35f); // al soltar
    to_pcm(g_tmp, n, out, 0.85f);
    return n;
}

// ------------------------------------------------------------ archivos .wav

static unsigned int le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24); }
static unsigned int le16(const unsigned char *p) { return p[0] | (p[1] << 8); }

static int load_wav(int id)
{
    char path[64];
    unsigned char hdr[12], ck[8], fmt[16];
    int channels = 0, rate = 0, bits = 0;
    FILE *f;

    _snprintf(path, sizeof(path) - 1, "game:\\sonidos\\%s.wav", kFiles[id]);
    path[sizeof(path) - 1] = 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
    {
        fclose(f);
        return 0;
    }
    while (fread(ck, 1, 8, f) == 8)
    {
        unsigned int size = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4) && size >= 16)
        {
            if (fread(fmt, 1, 16, f) != 16)
                break;
            if (le16(fmt) != 1)          // solo PCM
                break;
            channels = le16(fmt + 2);
            rate = (int)le32(fmt + 4);
            bits = le16(fmt + 14);
            fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR);
        }
        else if (!memcmp(ck, "data", 4) && bits == 16 && (channels == 1 || channels == 2) && rate > 0)
        {
            int count = (int)(size / 2);
            if (count > WAV_MAX)
                count = WAV_MAX;
            short *pcm = (short *)malloc(count * sizeof(short));
            if (!pcm)
                break;
            count = (int)fread(pcm, 2, count, f);
            fclose(f);
            for (int i = 0; i < count; ++i)     // little-endian del archivo -> orden de la consola
            {
                unsigned short u = (unsigned short)pcm[i];
                const unsigned char *b = (const unsigned char *)&u;
                pcm[i] = (short)(b[0] | (b[1] << 8));
            }
            g_snd[id].pcm = pcm;
            g_snd[id].channels = channels;
            g_snd[id].rate = rate;
            g_snd[id].frames = count / channels;
            OutputDebugStringA("[sfx] usando ");
            OutputDebugStringA(path);
            OutputDebugStringA("\n");
            return 1;
        }
        else
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);
    }
    fclose(f);
    return 0;
}

// ------------------------------------------------------------ voces

static int make_voices(Sound *s, IXAudio2 *xa)
{
    WAVEFORMATEX wf;

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)s->channels;
    wf.nSamplesPerSec = (DWORD)s->rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(s->channels * 2);
    wf.nAvgBytesPerSec = (DWORD)(s->rate * s->channels * 2);
    for (int v = 0; v < VOICES; ++v)
    {
        if (FAILED(xa->CreateSourceVoice(&s->voice[v], &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
        {
            s->voice[v] = NULL;
            return 0;
        }
        s->voice[v]->SetVolume(VOLUME);
    }
    return 1;
}

extern "C" int sfx_init(void)
{
    static const float dur[SFX_COUNT] = { 0.06f, 0.24f, 0.21f, 0.30f, 0.22f, 0.11f, 0.13f, 0.06f, 0.24f, 0.24f };
    IXAudio2 *xa = (IXAudio2 *)au_engine();
    static short synth[SFX_COUNT][MAX_LEN];
    int id;

    if (!xa)
        return 0;
    for (int v = 0; v < 3; ++v)
        g_keyLen[v] = make_key(v, g_keyVar[v]);
    for (id = 0; id < SFX_COUNT; ++id)
    {
        Sound *s = &g_snd[id];
        if (!load_wav(id) && !embedded(id, s))
        {
            if (id == SFX_KEY)
            {
                s->pcm = g_keyVar[0];
                s->frames = g_keyLen[0];
            }
            else
            {
                s->frames = make(id, dur[id]);
                static const float gain[SFX_COUNT] = { 0.45f, 0.6f, 0.6f, 0.7f, 0.65f, 0.85f, 0.7f, 0.45f, 0.6f, 0.6f };
                to_pcm(g_tmp, s->frames, synth[id], gain[id]);
                s->pcm = synth[id];
            }
            s->channels = 1;
            s->rate = RATE;
        }
        if (!make_voices(s, xa))
            return 0;
    }
    g_ok = 1;
    OutputDebugStringA("[sfx] sonidos de la interfaz listos\n");
    return 1;
}

extern "C" void sfx_play(int id)
{
    Sound *s;
    IXAudio2SourceVoice *v;
    XAUDIO2_BUFFER b;
    const short *pcm;
    int frames;

    if (!g_ok || !g_on || id < 0 || id >= SFX_COUNT)
        return;
    g_playedFrame = 1;
    s = &g_snd[id];
    pcm = s->pcm;
    frames = s->frames;
    if (id == SFX_KEY && s->pcm == g_keyVar[0])     // sintetizada: turna las 3 variantes
    {
        g_keyTurn = (g_keyTurn + 1 + (int)(GetTickCount() & 1)) % 3;
        pcm = g_keyVar[g_keyTurn];
        frames = g_keyLen[g_keyTurn];
    }
    v = s->voice[s->next];
    s->next = (s->next + 1) % VOICES;
    v->Stop(0, XAUDIO2_COMMIT_NOW);
    v->FlushSourceBuffers();
    memset(&b, 0, sizeof(b));
    b.AudioBytes = (UINT32)(frames * s->channels * sizeof(short));
    b.pAudioData = (const BYTE *)pcm;
    b.Flags = XAUDIO2_END_OF_STREAM;
    if (SUCCEEDED(v->SubmitSourceBuffer(&b)))
        v->Start(0, XAUDIO2_COMMIT_NOW);
}

extern "C" void sfx_enable(int on) { g_on = on ? 1 : 0; }
extern "C" int  sfx_enabled(void)  { return g_on; }
extern "C" void sfx_frame(void)    { g_playedFrame = 0; }
extern "C" int  sfx_played(void)   { return g_playedFrame; }
