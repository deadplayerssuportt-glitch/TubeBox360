/* avc.h - Conversion de cuadros H.264 de MP4 a Annex B. C89.
 *
 * En MP4 cada NAL (pedazo de un cuadro) va precedido por su tamaño
 * (normalmente 4 bytes). El decodificador de la consola quiere Annex B:
 * cada NAL precedido por el codigo de inicio 00 00 00 01. Ademas, antes
 * del primer cuadro clave hay que mandarle el SPS y el PPS (la
 * configuracion del video), y solo esa vez.
 *
 * Cada cuadro sale asi: AUD | [SPS | PPS] | NALs de la muestra.
 * El AUD (delimitador de cuadro) no cambia la imagen; solo le marca al
 * decodificador donde empieza cada cuadro. */
#ifndef AVC_H
#define AVC_H

#include "fmp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Convierte una muestra. Si withHeaders es 1, agrega SPS y PPS despues del AUD.
 * Si dst es NULL solo calcula cuantos bytes hacen falta.
 * Devuelve los bytes escritos (o necesarios), o -1 si la muestra esta mal. */
int avc_to_annexb(const FmpTrack *t, const unsigned char *src, int size,
                  int withHeaders, unsigned char *dst, int dstCap);

/* Para el diagnostico de atascos. Describe una muestra de MP4 (sin
 * convertir) en pocas letras: tipo de la primera rebanada (I, P o B, o
 * '-' si no trae), su nal_ref_idc (0 = nadie la usa de referencia),
 * cuantas rebanadas trae y los tipos de NAL en orden ("6x2" = dos SEI).
 * Ejemplo: "B0 1s 6.1". Devuelve la longitud del texto. */
int avc_describe_sample(const FmpTrack *t, const unsigned char *src, int size,
                        char *out, int outSize);

/* Para el diagnostico: lo del SPS y el PPS que puede importarle al
 * decodificador (referencias, reordenamiento, CABAC, 8x8, pesos...).
 * sps y pps sin codigo de inicio (como vienen en el moov). */
int avc_describe_params(const unsigned char *sps, int spsLen,
                        const unsigned char *pps, int ppsLen,
                        char *out, int outSize);

#ifdef __cplusplus
}
#endif

#endif /* AVC_H */
