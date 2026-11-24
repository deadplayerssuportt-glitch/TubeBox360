/* fmp.h - Lector de MP4 fragmentado (el formato DASH de YouTube). C89.
 *
 * Un archivo de YouTube se ve asi:
 *   ftyp | moov | sidx | moof mdat | moof mdat | ...
 *   - moov: datos del codec (para H.264: SPS y PPS; para AAC: su config)
 *   - sidx: indice, dice donde empieza cada fragmento y cuanto dura
 *   - moof: tabla de muestras del fragmento (tamaño, duracion, si es clave)
 *   - mdat: las muestras (cuadros de video o bloques de audio)
 *
 * Este modulo solo lee bytes en memoria; no usa la red. */
#ifndef FMP_H
#define FMP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    double   offset;     /* posicion en el archivo (double: puede pasar de 2 GB) */
    unsigned size;       /* bytes del fragmento (moof + mdat) */
    unsigned duration;   /* en unidades de sidxTimescale */
    int      startsWithKey;
} FmpSegment;

typedef struct
{
    int      isVideo;
    int      isAudio;
    unsigned trackId;
    unsigned timescale;          /* unidades por segundo de las muestras */
    double   durationSec;        /* 0 si el moov no la trae */
    unsigned defaultSampleDuration;
    unsigned defaultSampleSize;
    unsigned defaultSampleFlags;

    /* video H.264 */
    int           width, height;
    int           profile, level;    /* 77 = Main, 30 = nivel 3.0 */
    int           nalLengthSize;     /* bytes del tamaño antes de cada NAL (casi siempre 4) */
    unsigned char sps[256];
    int           spsLen;
    unsigned char pps[128];
    int           ppsLen;

    /* audio AAC */
    int           audioObjectType;   /* 2 = AAC LC, 5 = HE-AAC */
    int           sampleRate;
    int           channels;
    unsigned char asc[16];           /* AudioSpecificConfig */
    int           ascLen;

    /* indice */
    unsigned    sidxTimescale;
    double      firstTimeSec;
    int         segmentCount;
    FmpSegment *segments;            /* malloc; liberar con fmp_free */
} FmpTrack;

typedef struct
{
    unsigned sampleCount;
    unsigned totalBytes;      /* suma de tamaños de las muestras */
    double   startSec;        /* tiempo del primer cuadro */
    double   durationSec;
    int      keyframes;
    unsigned firstSampleSize;
    int      firstSampleOffset;   /* posicion de la primera muestra dentro del buffer */
    int      nalTypes[16];        /* tipos de NAL del primer cuadro (solo video) */
    int      nalCount;
} FmpFragment;

/* Una muestra (un cuadro de video o un bloque de audio) dentro de un fragmento */
typedef struct
{
    int      offset;   /* posicion dentro del buffer del fragmento */
    unsigned size;
    double   dts;      /* tiempo de decodificacion, segundos */
    double   pts;      /* tiempo de presentacion, segundos (difiere con cuadros B) */
    int      key;      /* 1 si es cuadro clave */
} FmpSample;

/* Lee ftyp/moov/sidx. buf debe contener al menos el moov y el sidx.
 * fileOffset: posicion de buf[0] dentro del archivo (normalmente 0).
 * Devuelve 1 si encontro una pista H.264 o AAC; si no, 0 y un mensaje en err. */
int  fmp_parse_init(const unsigned char *buf, int len, double fileOffset,
                    FmpTrack *t, char *err, int errSize);

/* Lee un fragmento (moof + mdat) completo que esta en buf. */
int  fmp_parse_fragment(const unsigned char *buf, int len, const FmpTrack *t,
                        FmpFragment *f, char *err, int errSize);

/* Llena out con las muestras de un fragmento completo (moof + mdat) en
 * orden de decodificacion. Devuelve cuantas lleno, o -1 si el fragmento
 * no es valido o una muestra se sale del buffer. */
int  fmp_fragment_samples(const unsigned char *buf, int len, const FmpTrack *t,
                          FmpSample *out, int cap);

void fmp_free(FmpTrack *t);

/* Nombre del perfil H.264 ("Main", "High"...) */
const char *fmp_profile_name(int profile);

#ifdef __cplusplus
}
#endif

#endif /* FMP_H */
