/* yt.h - Consultas a YouTube (API InnerTube). C89. */
#ifndef YT_H
#define YT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- Busqueda ---------------- */

typedef struct
{
    char id[16];         /* videoId, p. ej. "dQw4w9WgXcQ" */
    char title[256];
    char channel[128];
    char length[16];     /* "12:34", vacio si es en vivo */
    char views[64];      /* "1.2 M de vistas" */
    char published[48];  /* "hace 3 años" */
    char channelId[32];  /* "UC...", vacio si no vino */
    char avatar[320];    /* URL del avatar del canal, vacia si no vino */
    int  kind;           /* YT_KIND_VIDEO, YT_KIND_CHANNEL (busqueda) o YT_KIND_SHORT */
} YtVideo;

/* Que es cada resultado. En un canal (resultados de busqueda): title y
 * channel = nombre, channelId, avatar, views = suscriptores, published =
 * @canal; id queda vacio. */
enum { YT_KIND_VIDEO, YT_KIND_CHANNEL, YT_KIND_SHORT };

/* Cuantos videos se piden por lista (busqueda, relacionados, canal).
 * YouTube manda unos 20 por pagina; se piden las siguientes paginas
 * (continuation) hasta juntar estos. */
#define YT_LIST_MAX 50

/* Busca videos (hasta max, pidiendo varias paginas). Devuelve cuantos
 * encontro (0..max), o -1 si hubo error
 * (el mensaje queda en err). Bloquea: llamala desde un hilo aparte.
 * Tambien guarda el visitorData de YouTube para usarlo en yt_player. */
int yt_search(const char *queryUtf8, YtVideo *out, int max, char *err, int errSize);

/* Solo la parte de leer la respuesta (separada para poder probarla en PC).
 * Modifica json. Devuelve cuantos videos lleno, o -1 si el JSON no es valido.
 * Sirve para cualquier respuesta de InnerTube que traiga videos (busqueda,
 * relacionados, paginas de categorias). */
int yt_parse_search(char *json, YtVideo *out, int max);

/* ---------------- Inicio: recomendados sin iniciar sesion ---------------- */

/* Sin sesion, el inicio de YouTube viene vacio ("Busca algo para empezar"),
 * asi que el Inicio de la app se arma con:
 *   - yt_related: los videos que YouTube sugiere junto a un video (los de
 *     la columna derecha). Con los ultimos vistos sale "Porque viste...".
 *   - yt_browse: paginas de categorias de YouTube (Musica, Gaming...).
 * Las dos devuelven cuantos videos encontraron (0..max) o -1 si hubo error,
 * y bloquean: llamalas desde un hilo aparte. */
int yt_related(const char *videoId, YtVideo *out, int max, char *err, int errSize);
int yt_browse(const char *browseId, YtVideo *out, int max, char *err, int errSize);

/* ---------------- Pagina de un canal ---------------- */

typedef struct
{
    char    id[32];            /* "UC..." */
    char    title[128];
    char    handle[64];        /* "@canal" */
    char    subs[64];          /* "1,2 M de suscriptores" */
    char    videosText[64];    /* "523 videos" */
    char    description[600];
    char    avatar[320];       /* URL (unos 176 px) */
    char    banner[480];       /* URL (unos 1060 px de ancho) */
    int     videoCount;
    YtVideo videos[YT_LIST_MAX];   /* los mas recientes (pestana Videos) */
} YtChannel;

/* Pide la pagina del canal: datos de arriba y sus videos mas recientes.
 * Devuelve 1 si salio bien, 0 si no (motivo en err). Bloquea. */
int yt_channel(const char *channelId, YtChannel *c, char *err, int errSize);

/* visitorData (identifica a la consola ante YouTube sin sesion). Se guarda
 * en game:\visitor.txt para usar el mismo entre una vez y otra.
 * yt_init lo carga; llamar una vez al arrancar. */
void yt_init(void);

/* ---------------- Formatos de un video ---------------- */

/* User-Agent del cliente VISIONOS (Safari en Mac). Tambien se usa al
 * descargar el video desde googlevideo. */
#define YT_VISION_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) " \
                     "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"

#define YT_MAX_FORMATS 48
#define YT_URL_SIZE    3072

/* Pistas de audio (paso 5d). Algunos videos traen el audio original y
 * doblajes (muchos hechos con IA por YouTube). La app solo se queda con
 * hasta 4: la original del video, espanol, ingles y portugues (sin
 * repetir: si la original es espanol, quedan 3). */
#define YT_MAX_TRACKS  6     /* original + es, en, pt, ja, ar */

/* Subtitulos (5d parte 2): mismos idiomas que el audio (espanol, ingles,
 * portugues). Si el video no los trae en un idioma pero si se pueden
 * traducir, se agrega la traduccion automatica de YouTube. */
#define YT_MAX_CAPTIONS 5     /* es, en, pt, ja, ar */

typedef struct
{
    char lang[8];        /* "es", "en", "pt" */
    char name[80];       /* "Espanol (generado automaticamente)" */
    int  isAuto;         /* reconocimiento de voz de YouTube (kind "asr") */
    int  translated;     /* traducido por YouTube (&tlang=) */
    char url[640];       /* baseUrl (+ &tlang=xx); al bajar se agrega &fmt=json3 */
} YtCaption;

typedef struct
{
    char id[24];         /* "es-US.4", "en-US.10"... */
    char lang[8];        /* "es", "en", "pt"... (antes del primer - o .) */
    char name[64];       /* lo que muestra YouTube: "Ingles (US)" */
    int  isOriginal;     /* audioIsDefault: el audio original del video */
    int  isAutoDubbed;   /* doblaje automatico (IA) */
} YtAudioTrack;

typedef struct
{
    int    itag;
    char   mime[96];       /* video/mp4; codecs="avc1.4d401e" */
    char   codecs[48];     /* avc1.4d401e */
    int    isVideo;
    int    isAudio;
    int    width, height, fps;
    int    bitrate;        /* bits por segundo */
    int    sampleRate;     /* audio, Hz */
    char   quality[16];    /* "360p" */
    double contentLength;  /* bytes (double porque puede pasar de 2 GB) */
    int    initStart, initEnd;    /* rango del encabezado (moov) */
    int    indexStart, indexEnd;  /* rango del indice (sidx) */
    int    track;                 /* audio: posicion en YtPlayer.tracks, -1 si el video tiene una sola pista */
    char   url[YT_URL_SIZE];      /* vacio si viene cifrada */
} YtFormat;

typedef struct
{
    char     status[32];    /* "OK", "LOGIN_REQUIRED", "UNPLAYABLE"... */
    char     reason[256];
    int      lengthSeconds;
    int      expiresIn;     /* segundos que duran las URLs */
    char     hlsUrl[YT_URL_SIZE];
    int      trackCount;    /* 0 si el video tiene una sola pista de audio */
    YtAudioTrack tracks[YT_MAX_TRACKS];   /* la original primero, luego es, en, pt, ja, ar */
    int      captionCount;  /* 0 si no hay subtitulos en esos idiomas */
    YtCaption captions[YT_MAX_CAPTIONS];  /* en orden: es, en, pt, ja, ar */
    int      formatCount;
    YtFormat formats[YT_MAX_FORMATS];
} YtPlayer;

/* Pide los formatos de un video con el cliente VISIONOS. Devuelve 1 si el
 * video se puede reproducir (status "OK"), 0 si no (mensaje en err).
 * Del audio solo guarda los AAC (mp4a) de las pistas elegidas (ver
 * YT_MAX_TRACKS); Opus y las versiones "DRC" se descartan porque la
 * consola no las usa y llenarian la lista.
 * p queda lleno aunque falle, para mostrar status y reason. */
int yt_player(const char *videoId, YtPlayer *p, char *err, int errSize);

/* Solo la lectura de la respuesta de /player. Modifica json. 1 = JSON valido. */
int yt_parse_player(char *json, YtPlayer *p);

/* Descarga de prueba: pide los primeros 1024 bytes de la URL y describe
 * lo que llego en out ("HTTP 206 · 1024 bytes · MP4"). 1 = bien. */
int yt_probe_range(const char *url, char *out, int outSize);

/* Descarga la lista HLS y resume las variantes en out. 1 = bien. */
int yt_probe_hls(const char *url, char *out, int outSize);

#ifdef __cplusplus
}
#endif

#endif /* YT_H */
