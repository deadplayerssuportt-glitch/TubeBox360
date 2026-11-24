/* ffstubs.c - Piezas vacias para enlazar solo el decodificador AAC de FFmpeg.
 *
 * dsputil.c y utils.c de libavcodec hacen referencia a funciones de video
 * (IDCT, H.263, VP3, RV30/40...) y de imagenes que el audio nunca usa. En vez
 * de compilar cientos de archivos de video, aqui van versiones vacias. El
 * decodificador AAC no llama ninguna de ellas (probado en PC: el audio sale
 * identico al de FFmpeg completo). No incluye encabezados de FFmpeg a
 * proposito, para que los tipos no choquen. */

#include <stdio.h>
#include <string.h>
#include <math.h>

size_t av_strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s), c;
    if (n)
    {
        c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}
int av_expr_parse_and_eval(double *r) { *r = 0; return 0; }
int av_get_bits_per_sample_fmt(int f) { return f == 0 ? 8 : f == 1 ? 16 : f == 2 ? 32 : f == 3 ? 32 : 64; }
const char *av_get_sample_fmt_name(int f) { (void)f; return "s16"; }
void av_image_check_size(void) { }
void av_image_copy_plane(void) { }
void av_image_fill_linesizes(void) { }
void av_image_fill_pointers(void) { }
void av_opt_set_defaults(void) { }
void av_picture_copy(void) { }
char av_pix_fmt_descriptors[8192];
size_t av_strlcat(char *d, const char *s, size_t n)
{
    size_t l = strlen(d);
    return l + av_strlcpy(d + l, s, n > l ? n - l : 0);
}
void avcodec_get_chroma_sub_sample(void) { }
void avcodec_get_pix_fmt_name(void) { }
void colored_fputs(int level, const char *s) { (void)level; (void)s; }
void ff_ac3_downmix_c(void) { }
void ff_add_png_paeth_prediction(void) { }
void ff_bink_idct_add_c(void) { }
void ff_bink_idct_c(void) { }
void ff_bink_idct_put_c(void) { }
void ff_ea_idct_put_c(void) { }
void ff_faanidct(void) { }
void ff_faanidct_add(void) { }
void ff_faanidct_put(void) { }
unsigned char ff_h263_loop_filter_strength[32];
void ff_h264_lowres_idct_add_10_c(void) { }
void ff_h264_lowres_idct_add_8_c(void) { }
void ff_h264_lowres_idct_add_9_c(void) { }
void ff_h264_lowres_idct_put_10_c(void) { }
void ff_h264_lowres_idct_put_8_c(void) { }
void ff_h264_lowres_idct_put_9_c(void) { }
void ff_intrax8dsp_init(void) { }
void ff_is_hwaccel_pix_fmt(void) { }
void ff_mlp_init(void) { }
void ff_rv30dsp_init(void) { }
void ff_rv40dsp_init(void) { }
void ff_set_systematic_pal2(void) { }
void ff_shrink22(void) { }
void ff_shrink44(void) { }
void ff_shrink88(void) { }
void ff_simple_idct(void) { }
void ff_simple_idct_add(void) { }
void ff_simple_idct_put(void) { }
void ff_vp3_h_loop_filter_c(void) { }
void ff_vp3_idct_add_c(void) { }
void ff_vp3_idct_c(void) { }
void ff_vp3_idct_dc_add_c(void) { }
void ff_vp3_idct_put_c(void) { }
void ff_vp3_v_loop_filter_c(void) { }
void j_rev_dct(void) { }
void j_rev_dct1(void) { }
void j_rev_dct2(void) { }
void j_rev_dct4(void) { }
void ptw32_processTerminate(void) { }
void av_parse_video_rate(void) { }
void av_parse_video_size(void) { }
void dsputil_init_ppc(void) { }
void ff_xb_h264_chroma_init(void) { }
void ff_xb_h264_qpel_init(void) { }

/* --- Solo en la version Xbox de ffmpeg360 --- */

/* utils.c pide la tabla de formatos de imagen (solo video) */
void *get_av_pix_fmt_descriptors(void) { return av_pix_fmt_descriptors; }
void avpicture_init_pixfmtinfo(void) { }

/* El compilador de VS2010 no trae cbrtf; aacdec.c la usa para armar una
 * tabla al iniciar. */
float cbrtf(float x)
{
    return x < 0.0f ? -(float)pow(-x, 1.0 / 3.0) : (float)pow(x, 1.0 / 3.0);
}
