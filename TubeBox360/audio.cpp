// audio.cpp - Salida de sonido con XAudio2 (ver audio.h).
// C++ porque XAudio2 se usa con metodos; se expone en C.
//
// Cola: POOL buffers de BUF_FRAMES muestras cada uno, en anillo. Se llena
// uno a la vez; cuando esta lleno se manda a XAudio2. Como XAudio2 los
// termina en el mismo orden en que se mandan, basta con saber cuantos
// siguen en cola (BuffersQueued) para saber cuales estan libres.
//
// En la Xbox 360 el PCM de 16 bits va en el orden de bytes de la consola
// (big-endian), que es justo como lo entrega FFmpeg alli.

#include <xtl.h>
#include <xaudio2.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"

#define POOL         32       // buffers
#define BUF_FRAMES   4096     // muestras por canal en cada buffer (~93 ms a 44.1 kHz)
#define MAX_CHANNELS 2

static IXAudio2               *g_xa;
static IXAudio2MasteringVoice *g_master;
static IXAudio2SourceVoice    *g_voice;
static char                    g_status[96] = "Audio sin iniciar";

static short         g_buf[POOL][BUF_FRAMES * MAX_CHANNELS];
static int           g_fillIdx;           // buffer que se esta llenando
static int           g_fillCount;         // valores ya puestos en ese buffer
static int           g_rate, g_channels;
static double        g_basePts;
static volatile LONG g_submittedFrames;   // muestras por canal ya mandadas
static int           g_playing;
// SamplesPlayed podria reiniciarse al volver a Start despues de una pausa;
// si pasa, sumamos lo oido antes para que el reloj no brinque hacia atras.
static UINT64        g_samplesAtStop;
static UINT64        g_samplesOffset;

static void set_status(const char *s)
{
    strncpy(g_status, s, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = 0;
    OutputDebugStringA("[audio] ");
    OutputDebugStringA(s);
    OutputDebugStringA("\n");
}

extern "C" int au_init(void)
{
    HRESULT hr;
    char msg[96];

    // XAudio2 hace su trabajo en un hilo de hardware fijo. El de por omision
    // choca con nuestro hilo de dibujo (5) o con el decodificador (4) y el
    // sonido sale "robotico" y lento. El hilo 0 queda libre al reproducir.
    hr = XAudio2Create(&g_xa, 0, XboxThread0);
    if (FAILED(hr))
    {
        _snprintf(msg, sizeof(msg) - 1, "XAudio2Create fallo (%08lX)", (unsigned long)hr);
        msg[sizeof(msg) - 1] = 0;
        set_status(msg);
        g_xa = NULL;
        return 0;
    }
    hr = g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS,
                                    XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL);
    if (FAILED(hr))
    {
        _snprintf(msg, sizeof(msg) - 1, "CreateMasteringVoice fallo (%08lX)", (unsigned long)hr);
        msg[sizeof(msg) - 1] = 0;
        set_status(msg);
        g_xa->Release();
        g_xa = NULL;
        return 0;
    }
    set_status("XAudio2 listo");
    return 1;
}

extern "C" void *au_engine(void)
{
    return g_xa;
}

extern "C" const char *au_status(void)
{
    return g_status;
}

extern "C" int au_is_open(void)
{
    return g_voice != NULL;
}

extern "C" int au_open(int rate, int channels, double basePts)
{
    WAVEFORMATEX wf;
    HRESULT hr;
    char msg[96];

    if (!g_xa || g_voice)
        return g_voice != NULL;
    if (channels < 1 || channels > MAX_CHANNELS || rate <= 0)
    {
        set_status("Formato de audio no soportado");
        return 0;
    }

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = (DWORD)(rate * channels * 2);

    hr = g_xa->CreateSourceVoice(&g_voice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr))
    {
        _snprintf(msg, sizeof(msg) - 1, "CreateSourceVoice fallo (%08lX)", (unsigned long)hr);
        msg[sizeof(msg) - 1] = 0;
        set_status(msg);
        g_voice = NULL;
        return 0;
    }

    g_rate = rate;
    g_channels = channels;
    g_basePts = basePts;
    g_fillIdx = 0;
    g_fillCount = 0;
    g_submittedFrames = 0;
    g_playing = 0;
    g_samplesAtStop = 0;
    g_samplesOffset = 0;

    _snprintf(msg, sizeof(msg) - 1, "Audio PCM %d Hz, %d canales", rate, channels);
    msg[sizeof(msg) - 1] = 0;
    set_status(msg);
    return 1;
}

static int queued_buffers(void)
{
    XAUDIO2_VOICE_STATE st;
    g_voice->GetState(&st);
    return (int)st.BuffersQueued;
}

static int submit_fill(void)
{
    XAUDIO2_BUFFER b;

    if (g_fillCount <= 0)
        return 1;
    memset(&b, 0, sizeof(b));
    b.AudioBytes = (UINT32)(g_fillCount * sizeof(short));
    b.pAudioData = (const BYTE *)g_buf[g_fillIdx];
    if (FAILED(g_voice->SubmitSourceBuffer(&b)))
        return 0;
    InterlockedExchangeAdd(&g_submittedFrames, g_fillCount / g_channels);
    g_fillIdx = (g_fillIdx + 1) % POOL;
    g_fillCount = 0;
    return 1;
}

extern "C" int au_write(const short *pcm, int count)
{
    const int cap = BUF_FRAMES * g_channels;
    int n;

    if (!g_voice)
        return -1;

    // Hay que tener lugar para todo, para no dejar el cuadro a medias.
    // Peor caso: llenar el actual y usar uno mas.
    if (queued_buffers() >= POOL - 2)
        return 0;

    while (count > 0)
    {
        n = cap - g_fillCount;
        if (n > count)
            n = count;
        memcpy(&g_buf[g_fillIdx][g_fillCount], pcm, n * sizeof(short));
        g_fillCount += n;
        pcm += n;
        count -= n;
        if (g_fillCount == cap && !submit_fill())
            return -1;
    }
    return 1;
}

extern "C" void au_flush(void)
{
    if (g_voice)
        submit_fill();
}

static double played_seconds(void)
{
    XAUDIO2_VOICE_STATE st;

    if (!g_voice || g_rate <= 0)
        return 0.0;
    g_voice->GetState(&st);
    return (double)(g_samplesOffset + st.SamplesPlayed) / g_rate;
}

extern "C" double au_buffered(void)
{
    double sent;

    if (!g_voice || g_rate <= 0)
        return 0.0;
    sent = (double)g_submittedFrames / g_rate;
    return sent - played_seconds() + (double)(g_fillCount / g_channels) / g_rate;
}

extern "C" void au_play(int on)
{
    XAUDIO2_VOICE_STATE st;

    if (!g_voice || on == g_playing)
        return;
    if (on)
    {
        g_voice->Start(0, XAUDIO2_COMMIT_NOW);
        g_voice->GetState(&st);
        if (st.SamplesPlayed < g_samplesAtStop)
            g_samplesOffset += g_samplesAtStop;
    }
    else
    {
        g_voice->Stop(0, XAUDIO2_COMMIT_NOW);
        g_voice->GetState(&st);
        g_samplesAtStop = st.SamplesPlayed;
    }
    g_playing = on;
}

extern "C" double au_clock(void)
{
    return g_basePts + played_seconds();
}

extern "C" void au_close(void)
{
    if (!g_voice)
        return;
    g_voice->Stop(0, XAUDIO2_COMMIT_NOW);
    g_voice->FlushSourceBuffers();
    g_voice->DestroyVoice();   // espera a que XAudio2 suelte los buffers
    g_voice = NULL;
    g_playing = 0;
    g_fillCount = 0;
    set_status("Audio cerrado");
}
