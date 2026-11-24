/* audio.h - Salida de sonido con XAudio2 (interfaz en C).
 *
 * Recibe PCM de 16 bits intercalado (lo que da aac.c) y lo reproduce.
 * Tambien da el reloj del video: el segundo del audio que se esta oyendo.
 *
 * Uso: au_init() una vez; au_open() por video; un hilo llama au_write();
 * el hilo principal llama au_play() y au_clock(); au_close() al final. */
#ifndef AUDIO_H
#define AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

int    au_init(void);                 /* 1 si XAudio2 arranco */
void  *au_engine(void);               /* IXAudio2* (para sfx.cpp), NULL si no arranco */
const char *au_status(void);

/* basePts: segundo del video en que empieza la primera muestra */
int    au_open(int rate, int channels, double basePts);
int    au_is_open(void);

/* Agrega muestras (count = muestras x canales). 1 si las tomo, 0 si la
 * cola esta llena (esperar y repetir), -1 si esta cerrado. */
int    au_write(const short *pcm, int count);

/* Manda lo que quede a medio llenar (al terminar el audio) */
void   au_flush(void);

double au_buffered(void);             /* segundos en cola sin oir */
void   au_play(int on);               /* 1 = sonar, 0 = pausa */
double au_clock(void);                /* basePts + lo ya oido */
void   au_close(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
