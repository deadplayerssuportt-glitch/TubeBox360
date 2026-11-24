/* aac.h - Decodificador de audio AAC (envoltura propia sobre FFmpeg). C89.
 *
 * El audio de YouTube (itag 140) es AAC LC. La Xbox no lo decodifica por
 * hardware, asi que usamos el decodificador AAC de FFmpeg (LGPL 2.1+, la
 * version para Xbox 360 de ffmpeg360). Solo se enlaza ese decodificador.
 *
 * Salida: PCM de 16 bits con signo, canales intercalados (L R L R ...),
 * en el orden de bytes de la maquina. */
#ifndef AAC_H
#define AAC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AacDec AacDec;

/* asc: AudioSpecificConfig del MP4 (FmpTrack.asc). Devuelve NULL si falla. */
AacDec *aac_open(const unsigned char *asc, int ascLen, int sampleRate, int channels,
                 char *err, int errSize);

/* Decodifica un cuadro AAC (una muestra del fragmento MP4).
 * pcm: destino; cap: cuantos valores de 16 bits caben (conviene 8192).
 * Devuelve cuantos valores escribio (muestras x canales), 0 si el cuadro
 * no dio sonido, -1 si estaba dañado. */
int  aac_decode(AacDec *d, const unsigned char *frame, int len, short *pcm, int cap);

int  aac_rate(const AacDec *d);
int  aac_channels(const AacDec *d);
void aac_close(AacDec *d);

#ifdef __cplusplus
}
#endif

#endif /* AAC_H */
