/* yt.c - Consultas a YouTube con la API InnerTube. C89.
 *
 * Es la misma API que usan las apps de YouTube: POST con JSON a
 * https://www.youtube.com/youtubei/v1/<endpoint>.
 *
 * - Busqueda (/search) con el cliente WEB: de la respuesta sacamos cada
 *   "videoRenderer" (o "lockupViewModel", el formato mas nuevo).
 * - Formatos (/player) con el cliente VISIONOS (la app de YouTube para
 *   Apple Vision Pro): devuelve URLs de video directas, sin firma cifrada
 *   ni token de origen. Es el cliente que usan VivoTube, yt-dlp y otros. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lang.h"
#include "version.h"
#include "yt.h"
#include "json.h"
#include "net.h"

/* Cliente que decimos ser. Si YouTube algun dia rechaza esta version,
 * se actualiza aqui. */
#define YT_CLIENT_NAME    "WEB"
#define YT_CLIENT_VERSION "2.20251101.00.00"
/* hl y gl: segun el idioma de la app (lang_hl, lang_gl) */

/* Cliente VISIONOS (numero 101). Datos tomados de los proyectos que ya lo
 * usan (yt-dlp y otros). Si YouTube lo cambia, se actualiza aqui. */
#define YT_VISION_NAME    "VISIONOS"
#define YT_VISION_ID      "101"
#define YT_VISION_VERSION "1.02"
#define YT_VISION_OSVER   "26.5.23O471"
#define YT_VISION_MODEL   "RealityDevice17,1"

#define YT_PLAYER_URL "https://www.youtube.com/youtubei/v1/player?prettyPrint=false"

/* Filtro "solo videos" (el mismo que usa yt-dlp para buscar) */
#define YT_SEARCH_PARAMS  "EgIQAQ%3D%3D"

#define YT_SEARCH_URL "https://www.youtube.com/youtubei/v1/search?prettyPrint=false"
#define YT_NEXT_URL   "https://www.youtube.com/youtubei/v1/next?prettyPrint=false"
#define YT_BROWSE_URL "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false"
#define VISITOR_PATH  "game:\\visitor.txt"


/* Copias de las ultimas respuestas, para revisarlas si algo sale mal */
#define DEBUG_DUMP_PATH   "game:\\debug_busqueda.json"
#define DEBUG_PLAYER_PATH "game:\\debug_player.json"
#define DEBUG_HLS_PATH    "game:\\debug_hls.m3u8"

/* Identificador de visitante que YouTube manda en cada respuesta. Sin el,
 * algunos videos responden LOGIN_REQUIRED en /player. Solo se usa desde el
 * hilo de trabajo, una tarea a la vez. */
static char g_visitorData[256] = "";

/* ------------------------------------------------------------------ */
/* Utilidades de texto                                                 */

/* Copia sin pasarse del tamaño y sin cortar un caracter UTF-8 a la mitad */
static void copy_utf8(char *dst, int size, const char *src)
{
    int n;

    if (size <= 0)
        return;
    if (!src)
    {
        dst[0] = 0;
        return;
    }
    n = (int)strlen(src);
    if (n >= size)
    {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            --n;   /* retroceder hasta el inicio de un caracter */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void append_utf8(char *dst, int size, const char *src)
{
    int len = (int)strlen(dst);
    copy_utf8(dst + len, size - len, src);
}

/* Texto de un objeto de YouTube: {"simpleText":"..."}, {"runs":[{"text":..}]}
 * o {"content":"..."} (formato nuevo). */
static void text_of(const JsonNode *node, char *out, int size)
{
    const JsonNode *runs, *r;
    const char *s;

    out[0] = 0;
    if (!node)
        return;

    s = json_str(json_get(node, "simpleText"));
    if (!s)
        s = json_str(json_get(node, "content"));
    if (s)
    {
        copy_utf8(out, size, s);
        return;
    }

    runs = json_get(node, "runs");
    if (runs && runs->type == JSON_ARRAY)
        for (r = runs->child; r; r = r->next)
            append_utf8(out, size, json_str(json_get(r, "text")));
}

/* Busca en profundidad el primer nodo con esa clave */
static const JsonNode *find_key(const JsonNode *node, const char *key)
{
    const JsonNode *c, *f;

    if (!node)
        return NULL;
    for (c = node->child; c; c = c->next)
    {
        if (c->key && strcmp(c->key, key) == 0)
            return c;
        f = find_key(c, key);
        if (f)
            return f;
    }
    return NULL;
}

/* Escapa texto para meterlo dentro de una cadena JSON */
static void json_escape(char *dst, int size, const char *src)
{
    int n = 0;
    unsigned char c;

    while (*src && n < size - 7)
    {
        c = (unsigned char)*src++;
        if (c == '"' || c == '\\')
        {
            dst[n++] = '\\';
            dst[n++] = (char)c;
        }
        else if (c < 0x20)
        {
            n += sprintf(dst + n, "\\u%04x", c);
        }
        else
        {
            dst[n++] = (char)c;
        }
    }
    dst[n] = 0;
}

/* ------------------------------------------------------------------ */
/* Lectura de la respuesta                                             */

typedef struct
{
    YtVideo    *out;
    int         max;
    int         count;
    const char *skip;    /* id que no se agrega (el video de origen), o NULL */
} Collector;

/* 1 si el id ya esta en la lista (los relacionados repiten videos en la
 * pantalla final, etc.) o es el que se salta */
static int already_have(const Collector *col, const char *id)
{
    int i;

    if (col->skip && strcmp(col->skip, id) == 0)
        return 1;
    for (i = 0; i < col->count; ++i)
        if (strcmp(col->out[i].id, id) == 0)
            return 1;
    return 0;
}

/* URL de imagen de YouTube: las de los avatares a veces vienen sin
 * "https:" ("//yt3.ggpht.com/..."). */
static void copy_img_url(char *dst, int size, const char *src)
{
    dst[0] = 0;
    if (!src || !src[0])
        return;
    if (src[0] == '/' && src[1] == '/')
    {
        if ((int)strlen(src) + 7 >= size)
            return;
        strcpy(dst, "https:");
        strcat(dst, src);
        return;
    }
    if ((int)strlen(src) >= size)
        return;   /* cortada no serviria */
    strcpy(dst, src);

    /* yt3.googleusercontent.com y yt3.ggpht.com sirven las mismas imagenes;
     * se usa la segunda, que es la que ya funciona en las listas */
    {
        static const char from[] = "://yt3.googleusercontent.com/";
        char *p = strstr(dst, from);
        if (p)
        {
            memmove(p + 3 + 13, p + sizeof(from) - 1 - 1, strlen(p + sizeof(from) - 1 - 1) + 1);
            memcpy(p + 3, "yt3.ggpht.com", 13);
        }
    }
}

/* El id del canal ("UC...") que haya en un nodo (un browseEndpoint) */
static void channel_id_of(const JsonNode *node, char *out, int size)
{
    const JsonNode *be = find_key(node, "browseEndpoint");
    const char *id = json_str(json_get(be, "browseId"));

    out[0] = 0;
    if (id && id[0] == 'U' && id[1] == 'C')
        copy_utf8(out, size, id);
}

/* De una lista "sources"/"thumbnails" [{url,width}], la mas chica que
 * tenga al menos want de ancho (o la mas grande si ninguna llega) */
static void pick_image(const JsonNode *list, int want, char *out, int size)
{
    const JsonNode *t, *best = NULL, *big = NULL;
    const JsonNode *w;
    int bw = 0, gw = 0, cw;

    out[0] = 0;
    if (!list || list->type != JSON_ARRAY)
        return;
    for (t = list->child; t; t = t->next)
    {
        w = json_get(t, "width");
        cw = (w && w->type == JSON_NUMBER) ? (int)w->num : 0;
        if (!json_str(json_get(t, "url")))
            continue;
        if (cw >= want && (!best || cw < bw))
        {
            best = t;
            bw = cw;
        }
        if (!big || cw > gw)
        {
            big = t;
            gw = cw;
        }
    }
    if (!best)
        best = big;
    if (best)
        copy_img_url(out, size, json_str(json_get(best, "url")));
}

/* Formato clasico: "videoRenderer": { ... } */
static void add_video_renderer(Collector *col, const JsonNode *vr)
{
    YtVideo *v;
    const char *id = json_str(json_get(vr, "videoId"));
    const JsonNode *n;

    if (!id || col->count >= col->max || already_have(col, id))
        return;

    v = &col->out[col->count];
    memset(v, 0, sizeof(*v));
    copy_utf8(v->id, sizeof(v->id), id);
    text_of(json_get(vr, "title"), v->title, sizeof(v->title));

    text_of(json_get(vr, "ownerText"), v->channel, sizeof(v->channel));
    if (!v->channel[0])
        text_of(json_get(vr, "longBylineText"), v->channel, sizeof(v->channel));
    if (!v->channel[0])
        text_of(json_get(vr, "shortBylineText"), v->channel, sizeof(v->channel));

    text_of(json_get(vr, "lengthText"), v->length, sizeof(v->length));
    if (!v->length[0])
    {
        /* a veces la duracion solo viene en la etiqueta sobre la miniatura */
        n = find_key(json_get(vr, "thumbnailOverlays"), "thumbnailOverlayTimeStatusRenderer");
        text_of(json_get(n, "text"), v->length, sizeof(v->length));
    }

    text_of(json_get(vr, "shortViewCountText"), v->views, sizeof(v->views));
    if (!v->views[0])
        text_of(json_get(vr, "viewCountText"), v->views, sizeof(v->views));

    text_of(json_get(vr, "publishedTimeText"), v->published, sizeof(v->published));

    /* canal: id y avatar */
    channel_id_of(json_get(vr, "ownerText"), v->channelId, sizeof(v->channelId));
    if (!v->channelId[0])
        channel_id_of(json_get(vr, "longBylineText"), v->channelId, sizeof(v->channelId));
    if (!v->channelId[0])
        channel_id_of(json_get(vr, "shortBylineText"), v->channelId, sizeof(v->channelId));
    n = find_key(json_get(vr, "channelThumbnailSupportedRenderers"), "thumbnails");
    if (!n)
        n = find_key(json_get(vr, "channelThumbnail"), "thumbnails");
    pick_image(n, 68, v->avatar, sizeof(v->avatar));

    if (v->title[0])
        col->count++;
}

/* Formato nuevo: "lockupViewModel": { contentId, metadata... } */
static void add_lockup(Collector *col, const JsonNode *lv)
{
    YtVideo *v;
    const char *id = json_str(json_get(lv, "contentId"));
    const char *type = json_str(json_get(lv, "contentType"));
    const JsonNode *meta, *rows, *badge;

    if (!id || col->count >= col->max || already_have(col, id))
        return;
    if (type && strcmp(type, "LOCKUP_CONTENT_TYPE_VIDEO") != 0)
        return;   /* listas, canales, etc. */

    v = &col->out[col->count];
    memset(v, 0, sizeof(*v));
    copy_utf8(v->id, sizeof(v->id), id);

    meta = json_path(lv, "metadata.lockupMetadataViewModel");
    text_of(json_get(meta, "title"), v->title, sizeof(v->title));

    rows = find_key(meta, "metadataRows");
    if (rows && rows->child && !rows->child->next)
    {
        /* una sola fila (videos dentro de la pagina de un canal): vistas y fecha */
        text_of(json_path(rows, "0.metadataParts.0.text"), v->views,     sizeof(v->views));
        text_of(json_path(rows, "0.metadataParts.1.text"), v->published, sizeof(v->published));
    }
    else
    {
        text_of(json_path(rows, "0.metadataParts.0.text"), v->channel,   sizeof(v->channel));
        text_of(json_path(rows, "1.metadataParts.0.text"), v->views,     sizeof(v->views));
        text_of(json_path(rows, "1.metadataParts.1.text"), v->published, sizeof(v->published));
    }

    badge = find_key(json_get(lv, "contentImage"), "thumbnailBadgeViewModel");
    copy_utf8(v->length, sizeof(v->length), json_str(json_get(badge, "text")));

    /* canal: id y avatar (vienen en los datos de abajo de la tarjeta) */
    channel_id_of(meta, v->channelId, sizeof(v->channelId));
    pick_image(json_path(find_key(meta, "avatarViewModel"), "image.sources"), 68,
               v->avatar, sizeof(v->avatar));

    if (v->title[0])
        col->count++;
}

/* Canal en los resultados de busqueda: "channelRenderer". En el formato
 * actual subscriberCountText trae el @canal y videoCountText los
 * suscriptores. */
static void add_channel_renderer(Collector *col, const JsonNode *cr)
{
    YtVideo *v;
    const char *cid = json_str(json_get(cr, "channelId"));
    char subs[64], handle[64];
    int i;

    if (!cid || col->count >= col->max)
        return;
    for (i = 0; i < col->count; ++i)
        if (col->out[i].kind == YT_KIND_CHANNEL && strcmp(col->out[i].channelId, cid) == 0)
            return;
    v = &col->out[col->count];
    memset(v, 0, sizeof(*v));
    v->kind = YT_KIND_CHANNEL;
    copy_utf8(v->channelId, sizeof(v->channelId), cid);
    text_of(json_get(cr, "title"), v->title, sizeof(v->title));
    copy_utf8(v->channel, sizeof(v->channel), v->title);
    text_of(json_get(cr, "subscriberCountText"), handle, sizeof(handle));
    text_of(json_get(cr, "videoCountText"), subs, sizeof(subs));
    if (handle[0] == '@')
    {
        copy_utf8(v->published, sizeof(v->published), handle);
        copy_utf8(v->views, sizeof(v->views), subs);
    }
    else
    {
        /* formato viejo: suscriptores y numero de videos */
        copy_utf8(v->views, sizeof(v->views), handle);
        copy_utf8(v->published, sizeof(v->published), subs);
    }
    pick_image(find_key(json_get(cr, "thumbnail"), "thumbnails"), 176, v->avatar, sizeof(v->avatar));
    if (v->title[0])
        col->count++;
}

/* Shorts: "reelItemRenderer" (viejo) y "shortsLockupViewModel" (nuevo) */
static void add_short(Collector *col, const char *id, const char *title, const char *views)
{
    YtVideo *v;

    if (!id || !id[0] || col->count >= col->max || already_have(col, id))
        return;
    v = &col->out[col->count];
    memset(v, 0, sizeof(*v));
    v->kind = YT_KIND_SHORT;
    copy_utf8(v->id, sizeof(v->id), id);
    copy_utf8(v->title, sizeof(v->title), title ? title : "");
    copy_utf8(v->views, sizeof(v->views), views ? views : "");
    copy_utf8(v->length, sizeof(v->length), "Shorts");
    if (v->title[0])
        col->count++;
}

static void add_reel_item(Collector *col, const JsonNode *ri)
{
    char title[256], views[64];

    text_of(json_get(ri, "headline"), title, sizeof(title));
    text_of(json_get(ri, "viewCountText"), views, sizeof(views));
    add_short(col, json_str(json_get(ri, "videoId")), title, views);
}

static void add_shorts_lockup(Collector *col, const JsonNode *sl)
{
    const JsonNode *ep = find_key(sl, "reelWatchEndpoint");
    const JsonNode *meta = json_get(sl, "overlayMetadata");
    char title[256], views[64];

    text_of(json_get(meta, "primaryText"), title, sizeof(title));
    text_of(json_get(meta, "secondaryText"), views, sizeof(views));
    add_short(col, json_str(json_get(ep, "videoId")), title, views);
}

static void collect(Collector *col, const JsonNode *node)
{
    const JsonNode *c;

    for (c = node->child; c && col->count < col->max; c = c->next)
    {
        if (c->key && c->type == JSON_OBJECT &&
            (strcmp(c->key, "videoRenderer") == 0 ||
             strcmp(c->key, "compactVideoRenderer") == 0 ||   /* relacionados (formato viejo) */
             strcmp(c->key, "gridVideoRenderer") == 0))       /* paginas de canales */
            add_video_renderer(col, c);
        else if (c->key && c->type == JSON_OBJECT && strcmp(c->key, "lockupViewModel") == 0)
            add_lockup(col, c);
        else if (c->key && c->type == JSON_OBJECT && strcmp(c->key, "channelRenderer") == 0)
            add_channel_renderer(col, c);
        else if (c->key && c->type == JSON_OBJECT && strcmp(c->key, "reelItemRenderer") == 0)
            add_reel_item(col, c);
        else if (c->key && c->type == JSON_OBJECT && strcmp(c->key, "shortsLockupViewModel") == 0)
            add_shorts_lockup(col, c);
        else if (c->child)
            collect(col, c);
    }
}

/* Guarda el visitorData nuevo (si cambio) para la proxima vez */
static void take_visitor(const JsonNode *root)
{
    const char *v = json_str(json_path(root, "responseContext.visitorData"));
    FILE *f;

    if (!v || !v[0] || strcmp(v, g_visitorData) == 0 || strlen(v) >= sizeof(g_visitorData))
        return;
    copy_utf8(g_visitorData, sizeof(g_visitorData), v);
    f = fopen(VISITOR_PATH, "wb");
    if (f)
    {
        fputs(g_visitorData, f);
        fclose(f);
    }
}

void yt_init(void)
{
    char buf[256];
    FILE *f = fopen(VISITOR_PATH, "rb");
    int n, i;

    if (!f)
        return;
    n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n <= 0)
        return;
    buf[n] = 0;
    /* solo caracteres de base64 url (sin espacios ni saltos) */
    for (i = 0; buf[i]; ++i)
    {
        char ch = buf[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '%' || ch == '='))
            break;
    }
    buf[i] = 0;
    if (i > 0)
        copy_utf8(g_visitorData, sizeof(g_visitorData), buf);
}

static void parse_channel_header(const JsonNode *root, YtChannel *ch);

/* Token de la siguiente pagina (el ultimo "continuationItemRenderer"
 * dentro de scope), o "" */
/* El ultimo nodo con esa clave (recorrido completo) */
static void find_last_key(const JsonNode *node, const char *key, const JsonNode **last)
{
    const JsonNode *c;

    if (!node)
        return;
    for (c = node->child; c; c = c->next)
    {
        if (c->key && strcmp(c->key, key) == 0)
            *last = c;
        find_last_key(c, key, last);
    }
}

static void find_token(const JsonNode *scope, char *out, int size)
{
    const JsonNode *c = NULL;
    const char *tok;

    /* 2026-11-23: el ULTIMO (antes se tomaba el primero; en la busqueda el
     * primero puede ser de un estante y la siguiente pagina no llegaba) */
    find_last_key(scope, "continuationItemRenderer", &c);
    tok = json_str(json_path(c, "continuationEndpoint.continuationCommand.token"));
    out[0] = 0;
    if (tok && (int)strlen(tok) < size)
        strcpy(out, tok);
}

/* Lee una pagina: agrega sus videos a col y deja en token el de la
 * siguiente. secondary: solo mirar los relacionados (la respuesta de /next
 * tambien trae comentarios con su propio token). ch (puede ser NULL): ahi
 * van los datos del canal. Devuelve -1 si el JSON no es valido. */
static int read_page(Collector *col, char *json, int secondary, char *token, int tokenSize,
                     YtChannel *ch)
{
    JsonDoc *doc;
    const JsonNode *scope;

    token[0] = 0;
    doc = json_parse(json);
    if (!doc)
        return -1;

    take_visitor(doc->root);
    if (ch)
        parse_channel_header(doc->root, ch);

    scope = secondary ? find_key(doc->root, "secondaryResults") : NULL;
    if (!scope)
        scope = doc->root;
    collect(col, scope);
    find_token(scope, token, tokenSize);

    json_free(doc);
    return col->count;
}

int yt_parse_search(char *json, YtVideo *out, int max)
{
    Collector col;
    char token[16];

    col.out = out;
    col.max = max;
    col.count = 0;
    col.skip = NULL;
    return read_page(&col, json, 0, token, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Peticion                                                            */

static void dump_debug(const char *path, const char *data, int len)
{
#if TB_BETA
    /* beta: sin archivos de depuracion */
    (void)path;
    (void)data;
    (void)len;
#else
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fwrite(data, 1, len, f);
        fclose(f);
    }
#endif
}

/* (fetch_list esta mas abajo) */
static int fetch_list(const char *url, const char *extra, const char *debugPath, const char *skip,
                      int secondary, YtVideo *out, int max, YtChannel *ch, char *err, int errSize);

int yt_search(const char *queryUtf8, YtVideo *out, int max, char *err, int errSize)
{
    char escaped[512];
    char extra[700];
    int count;

    json_escape(escaped, sizeof(escaped), queryUtf8);
    _snprintf(extra, sizeof(extra) - 1, "\"query\":\"%s\",\"params\":\"%s\"",
              escaped, YT_SEARCH_PARAMS);   /* el filtro lleva '%', va como argumento */
    extra[sizeof(extra) - 1] = 0;

    count = fetch_list(YT_SEARCH_URL, extra, DEBUG_DUMP_PATH, NULL, 0, out, max, NULL, err, errSize);
    if (count == 0)
    {
        _snprintf(err, errSize - 1, "Sin resultados");
        err[errSize - 1] = 0;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Formatos de un video (/player con VISIONOS)                         */

/* Numero de un nodo que puede venir como numero (123) o como cadena ("123") */
static double num_of(const JsonNode *n)
{
    if (!n)
        return 0.0;
    if (n->type == JSON_NUMBER)
        return n->num;
    if (n->type == JSON_STRING)
        return atof(n->str);
    return 0.0;
}

/* ---------------- Inicio: relacionados y categorias ---------------- */

static int is_true(const JsonNode *n)
{
    return n && n->type == JSON_BOOL && n->boolean;
}


/* POST a InnerTube con el cliente WEB (el mismo de la busqueda).
 * Devuelve 1 con la respuesta (status 200) en resp, 0 si fallo. */
static int web_post(const char *url, const char *extraJson, const char *debugPath,
                    HttpResponse *resp, char *err, int errSize)
{
    char headers[768];
    static char body[2048];   /* solo lo usa el hilo de trabajo, de uno en uno */
    int bodyLen;

    _snprintf(headers, sizeof(headers) - 1,
              "Content-Type: application/json\r\n"
              "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36\r\n"
              "Origin: https://www.youtube.com\r\n"
              "X-YouTube-Client-Name: 1\r\n"
              "X-YouTube-Client-Version: " YT_CLIENT_VERSION "\r\n"
              "%s%s%s",
              g_visitorData[0] ? "X-Goog-Visitor-Id: " : "", g_visitorData,
              g_visitorData[0] ? "\r\n" : "");
    headers[sizeof(headers) - 1] = 0;

    bodyLen = _snprintf(body, sizeof(body) - 1,
        "{\"context\":{\"client\":{\"clientName\":\"" YT_CLIENT_NAME "\","
        "\"clientVersion\":\"" YT_CLIENT_VERSION "\","
        "\"hl\":\"%s\",\"gl\":\"%s\"%s%s%s}},%s}",
        lang_hl(), lang_gl(),
        g_visitorData[0] ? ",\"visitorData\":\"" : "", g_visitorData,
        g_visitorData[0] ? "\"" : "", extraJson);
    if (bodyLen < 0)
    {
        _snprintf(err, errSize - 1, "Peticion demasiado larga");
        err[errSize - 1] = 0;
        return 0;
    }

    if (!http_request("POST", url, headers, body, bodyLen, resp, err, errSize))
        return 0;
    if (debugPath)
        dump_debug(debugPath, resp->body, resp->bodyLen);
    if (resp->status != 200)
    {
        _snprintf(err, errSize - 1, "YouTube respondio HTTP %d", resp->status);
        err[errSize - 1] = 0;
        http_free(resp);
        return 0;
    }
    return 1;
}

/* Pide una lista de videos y sus paginas siguientes hasta juntar max (o
 * hasta que YouTube no de mas). Devuelve cuantos junto, o -1 si fallo la
 * primera pagina. */
static int fetch_list(const char *url, const char *extra, const char *debugPath, const char *skip,
                      int secondary, YtVideo *out, int max, YtChannel *ch, char *err, int errSize)
{
    static char token[1024];
    static char cont[1100];
    Collector col;
    HttpResponse resp;
    int page, before, r, empty = 0;

    col.out = out;
    col.max = max;
    col.count = 0;
    col.skip = skip;
    token[0] = 0;

    for (page = 0; page < 6 && col.count < max; ++page)
    {
        if (page > 0)
        {
            if (!token[0])
                break;
            _snprintf(cont, sizeof(cont) - 1, "\"continuation\":\"%s\"", token);
            cont[sizeof(cont) - 1] = 0;
        }
        if (!web_post(url, page == 0 ? extra : cont, page == 0 ? debugPath : NULL, &resp, err, errSize))
        {
            if (page == 0)
                return -1;
            break;   /* las paginas extra son opcionales */
        }
        before = col.count;
        r = read_page(&col, resp.body, page == 0 ? secondary : 0, token, sizeof(token),
                      page == 0 ? ch : NULL);
        http_free(&resp);
        if (r < 0)
        {
            if (page == 0)
            {
                _snprintf(err, errSize - 1, "No se pudo leer la respuesta (JSON invalido)");
                err[errSize - 1] = 0;
                return -1;
            }
            break;
        }
        if (col.count > before)
            empty = 0;
        else if (++empty >= 2)
            break;   /* dos paginas seguidas sin videos nuevos */
    }
    return col.count;
}

/* Solo letras, numeros, - y _ (ids de video y de pagina) */
static void clean_id(const char *in, char *out, int size)
{
    int i, n = 0;

    for (i = 0; in && in[i] && n < size - 1; ++i)
    {
        char ch = in[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
            out[n++] = ch;
    }
    out[n] = 0;
}

int yt_related(const char *videoId, YtVideo *out, int max, char *err, int errSize)
{
    char id[16];
    char extra[96];

    clean_id(videoId, id, sizeof(id));
    _snprintf(extra, sizeof(extra) - 1, "\"videoId\":\"%s\"", id);
    extra[sizeof(extra) - 1] = 0;
    return fetch_list(YT_NEXT_URL, extra, "game:\\debug_inicio_relacionados.json", id, 1,
                      out, max, NULL, err, errSize);
}

int yt_browse(const char *browseId, YtVideo *out, int max, char *err, int errSize)
{
    char id[64];
    char extra[128];
    char path[64];

    clean_id(browseId, id, sizeof(id));

    _snprintf(extra, sizeof(extra) - 1, "\"browseId\":\"%s\"", id);
    extra[sizeof(extra) - 1] = 0;
    /* nombre corto para el archivo de depuracion (FATX: 42 caracteres) */
    _snprintf(path, sizeof(path) - 1, "game:\\debug_inicio_%.8s.json", id);
    path[sizeof(path) - 1] = 0;
    return fetch_list(YT_BROWSE_URL, extra, path, NULL, 0, out, max, NULL, err, errSize);
}

/* ---------------- Pagina de un canal ---------------- */

/* Texto de un "content" (formato nuevo) o de text_of (formato viejo) */
static void text_any(const JsonNode *node, char *out, int size)
{
    const char *c = json_str(find_key(node, "content"));

    if (c)
        copy_utf8(out, size, c);
    else
        text_of(node, out, size);
}

static void parse_channel_header(const JsonNode *root, YtChannel *ch)
{
    const JsonNode *md, *hdr, *rows, *r, *part, *c4;
    char buf[128];

    md = json_path(root, "metadata.channelMetadataRenderer");
    if (md)
    {
        copy_utf8(ch->title, sizeof(ch->title), json_str(json_get(md, "title")));
        copy_utf8(ch->description, sizeof(ch->description), json_str(json_get(md, "description")));
    }

    /* encabezado nuevo (2024+) */
    hdr = find_key(root, "pageHeaderViewModel");
    if (hdr)
    {
        if (!ch->title[0])
            text_any(json_get(hdr, "title"), ch->title, sizeof(ch->title));
        rows = find_key(json_get(hdr, "metadata"), "metadataRows");
        if (rows && rows->type == JSON_ARRAY)
            for (r = rows->child; r; r = r->next)
            {
                part = json_get(r, "metadataParts");
                for (part = part ? part->child : NULL; part; part = part->next)
                {
                    text_any(json_get(part, "text"), buf, sizeof(buf));
                    if (buf[0] == '@' && !ch->handle[0])
                        copy_utf8(ch->handle, sizeof(ch->handle), buf);
                    else if (strstr(buf, "suscriptor") || strstr(buf, "subscriber"))
                        copy_utf8(ch->subs, sizeof(ch->subs), buf);
                    else if (strstr(buf, "video"))
                        copy_utf8(ch->videosText, sizeof(ch->videosText), buf);
                }
            }
        if (!ch->banner[0])
            pick_image(json_path(find_key(hdr, "imageBannerViewModel"), "image.sources"), 1060,
                       ch->banner, sizeof(ch->banner));
        if (!ch->avatar[0])
            pick_image(find_key(json_get(hdr, "image"), "sources"), 160, ch->avatar, sizeof(ch->avatar));
        if (!ch->description[0])
            text_any(find_key(hdr, "descriptionPreviewViewModel"), ch->description, sizeof(ch->description));
    }

    /* encabezado viejo */
    c4 = find_key(root, "c4TabbedHeaderRenderer");
    if (c4)
    {
        if (!ch->title[0])
            copy_utf8(ch->title, sizeof(ch->title), json_str(json_get(c4, "title")));
        if (!ch->subs[0])
            text_of(json_get(c4, "subscriberCountText"), ch->subs, sizeof(ch->subs));
        if (!ch->handle[0])
            text_of(json_get(c4, "channelHandleText"), ch->handle, sizeof(ch->handle));
        if (!ch->videosText[0])
            text_of(json_get(c4, "videosCountText"), ch->videosText, sizeof(ch->videosText));
        if (!ch->banner[0])
            pick_image(json_path(c4, "banner.thumbnails"), 1060, ch->banner, sizeof(ch->banner));
        if (!ch->avatar[0])
            pick_image(json_path(c4, "avatar.thumbnails"), 160, ch->avatar, sizeof(ch->avatar));
    }

    /* el de los metadatos suele venir solo en 900 px: se pide mas chico */
    if (!ch->avatar[0] && md)
    {
        char *sz;
        pick_image(json_path(md, "avatar.thumbnails"), 160, ch->avatar, sizeof(ch->avatar));
        sz = strstr(ch->avatar, "=s");
        if (sz && sz[2] >= '0' && sz[2] <= '9')
        {
            char rest[320];
            char *e = sz + 2;
            while (*e >= '0' && *e <= '9')
                ++e;
            strcpy(rest, e);
            if ((int)(sz - ch->avatar) + 6 + (int)strlen(rest) < (int)sizeof(ch->avatar))
            {
                strcpy(sz, "=s176");
                strcat(sz, rest);
            }
        }
    }
}

int yt_channel(const char *channelId, YtChannel *c, char *err, int errSize)
{
    char extra[160];
    int n, i;

    memset(c, 0, sizeof(*c));
    clean_id(channelId, c->id, sizeof(c->id));
    if (!c->id[0])
    {
        _snprintf(err, errSize - 1, "Este video no trae el canal");
        err[errSize - 1] = 0;
        return 0;
    }

    /* pestana "Videos" (los mas recientes primero) */
    _snprintf(extra, sizeof(extra) - 1, "\"browseId\":\"%s\",\"params\":\"EgZ2aWRlb3PyBgQKAjoA\"", c->id);
    extra[sizeof(extra) - 1] = 0;
    n = fetch_list(YT_BROWSE_URL, extra, "game:\\debug_canal.json", NULL, 0,
                   c->videos, YT_LIST_MAX, c, err, errSize);
    if (n <= 0)
    {
        /* sin pestana de videos: la pagina principal del canal */
        _snprintf(extra, sizeof(extra) - 1, "\"browseId\":\"%s\"", c->id);
        extra[sizeof(extra) - 1] = 0;
        n = fetch_list(YT_BROWSE_URL, extra, "game:\\debug_canal.json", NULL, 0,
                       c->videos, YT_LIST_MAX, c, err, errSize);
    }
    if (n < 0)
        return 0;
    if (!c->title[0] && n == 0)
    {
        _snprintf(err, errSize - 1, "No se pudo leer el canal");
        err[errSize - 1] = 0;
        return 0;
    }

    /* los videos del canal no repiten su nombre ni su avatar */
    c->videoCount = n;
    for (i = 0; i < n; ++i)
    {
        if (!c->videos[i].channel[0])
            copy_utf8(c->videos[i].channel, sizeof(c->videos[i].channel), c->title);
        if (!c->videos[i].avatar[0])
            copy_utf8(c->videos[i].avatar, sizeof(c->videos[i].avatar), c->avatar);
        if (!c->videos[i].channelId[0])
            copy_utf8(c->videos[i].channelId, sizeof(c->videos[i].channelId), c->id);
    }
    return 1;
}

/* ---------------- Subtitulos (5d parte 2) ---------------- */

/* "es-419" -> "es" */
static void lang_prefix(const char *code, char *out, int size)
{
    int i;

    for (i = 0; code && code[i] && code[i] != '-' && code[i] != '_' && i < size - 1; ++i)
        out[i] = (char)((code[i] >= 'A' && code[i] <= 'Z') ? code[i] + 32 : code[i]);
    out[i] = 0;
}

static int caption_score(const JsonNode *t)
{
    const char *code = json_str(json_get(t, "languageCode"));
    const char *kind = json_str(json_get(t, "kind"));
    int score = 0;

    if (!kind || strcmp(kind, "asr") != 0)
        score += 10;   /* hechos por personas antes que los automaticos */
    if (code && (strcmp(code, "es-419") == 0 || strcmp(code, "es-MX") == 0 ||
                 strcmp(code, "en-US") == 0 || strcmp(code, "pt-BR") == 0 ||
                 strcmp(code, "ja") == 0 || strcmp(code, "ar") == 0))
        score += 5;
    return score;
}

/* 1 si YouTube puede traducir al idioma lang (lista translationLanguages) */
static int can_translate_to(const JsonNode *renderer, const char *lang)
{
    const JsonNode *list = json_get(renderer, "translationLanguages");
    const JsonNode *t;
    char pre[8];

    if (!list || list->type != JSON_ARRAY)
        return 0;
    for (t = list->child; t; t = t->next)
    {
        lang_prefix(json_str(json_get(t, "languageCode")), pre, sizeof(pre));
        if (strcmp(pre, lang) == 0)
            return 1;
    }
    return 0;
}

static void pick_captions(YtPlayer *p, const JsonNode *renderer)
{
    static const char *langs[5] = { "es", "en", "pt", "ja", "ar" };
    const char *names[5];
    const JsonNode *tracks, *t, *best, *src = NULL;
    const char *url;
    char pre[8];
    YtCaption *c;
    int k;

    names[0] = T(S_LANG_ES);
    names[1] = T(S_LANG_EN);
    names[2] = T(S_LANG_PT);
    names[3] = T(S_LANG_JA);
    names[4] = T(S_LANG_AR);
    p->captionCount = 0;
    tracks = json_get(renderer, "captionTracks");
    if (!tracks || tracks->type != JSON_ARRAY)
        return;

    /* de cual se traduce si falta un idioma: una hecha por personas si hay */
    for (t = tracks->child; t; t = t->next)
    {
        if (!is_true(json_get(t, "isTranslatable")) || !json_str(json_get(t, "baseUrl")))
            continue;
        if (!src || caption_score(t) > caption_score(src))
            src = t;
    }

    for (k = 0; k < 5; ++k)
    {
        best = NULL;
        for (t = tracks->child; t; t = t->next)
        {
            lang_prefix(json_str(json_get(t, "languageCode")), pre, sizeof(pre));
            if (strcmp(pre, langs[k]) != 0 || !json_str(json_get(t, "baseUrl")))
                continue;
            if (!best || caption_score(t) > caption_score(best))
                best = t;
        }

        c = &p->captions[p->captionCount];
        memset(c, 0, sizeof(*c));
        strcpy(c->lang, langs[k]);
        if (best)
        {
            const char *kind = json_str(json_get(best, "kind"));
            url = json_str(json_get(best, "baseUrl"));
            if (strlen(url) >= sizeof(c->url) - 16)
                continue;   /* no cabe con &fmt=json3 */
            strcpy(c->url, url);
            text_of(json_get(best, "name"), c->name, sizeof(c->name));
            if (!c->name[0])
                copy_utf8(c->name, sizeof(c->name), names[k]);
            c->isAuto = kind && strcmp(kind, "asr") == 0;
            p->captionCount++;
        }
        else if (src && can_translate_to(renderer, langs[k]))
        {
            url = json_str(json_get(src, "baseUrl"));
            if (strlen(url) >= sizeof(c->url) - 32)
                continue;
            sprintf(c->url, "%s&tlang=%s", url, langs[k]);
            _snprintf(c->name, sizeof(c->name) - 1, T(S_AUTOTRANS), names[k]);
            c->name[sizeof(c->name) - 1] = 0;
            c->translated = 1;
            p->captionCount++;
        }
    }
}

/* ---------------- Pistas de audio (5d) ---------------- */

#define MAX_CANDIDATES 40

/* 1 si el formato es audio AAC normal (sin "DRC", la version con el
 * volumen comprimido que YouTube agrega en algunos videos) */
static int is_plain_aac(const JsonNode *f)
{
    const char *mime = json_str(json_get(f, "mimeType"));

    return mime && strncmp(mime, "audio/mp4", 9) == 0 && strstr(mime, "mp4a") &&
           !is_true(json_get(f, "isDrc"));
}

static void track_from(const JsonNode *at, YtAudioTrack *t)
{
    int i;

    memset(t, 0, sizeof(*t));
    copy_utf8(t->id, sizeof(t->id), json_str(json_get(at, "id")));
    copy_utf8(t->name, sizeof(t->name), json_str(json_get(at, "displayName")));
    t->isOriginal = is_true(json_get(at, "audioIsDefault"));
    t->isAutoDubbed = is_true(json_get(at, "isAutoDubbed"));

    /* "es-US.4" -> "es" (en minusculas) */
    for (i = 0; t->id[i] && t->id[i] != '-' && t->id[i] != '.' && t->id[i] != '_' &&
                i < (int)sizeof(t->lang) - 1; ++i)
        t->lang[i] = (char)((t->id[i] >= 'A' && t->id[i] <= 'Z') ? t->id[i] + 32 : t->id[i]);
    t->lang[i] = 0;
}

/* Que tan buena es una pista para su idioma: la original gana; luego la
 * variante de la region (es de Latinoamerica, en de EE.UU., pt de Brasil);
 * luego un doblaje hecho por personas antes que uno automatico. */
static int track_score(const YtAudioTrack *t)
{
    int score = 0;

    if (t->isOriginal)
        score += 100;
    if (strstr(t->id, "es-419") || strstr(t->id, "es-US") || strstr(t->id, "es-MX") ||
        strstr(t->id, "en-US") || strstr(t->id, "pt-BR"))
        score += 10;
    if (!t->isAutoDubbed)
        score += 5;
    return score;
}

/* Lee todas las pistas de audio del video y se queda con hasta 6: la
 * original, espanol, ingles, portugues, japones y arabe, en ese orden y
 * sin repetir. */
static void pick_tracks(YtPlayer *p, const JsonNode *adaptive)
{
    static YtAudioTrack cand[MAX_CANDIDATES];
    static const char *langs[5] = { "es", "en", "pt", "ja", "ar" };
    YtAudioTrack t;
    const JsonNode *f, *at;
    int count = 0, orig = -1, best, i, k;

    p->trackCount = 0;
    if (!adaptive || adaptive->type != JSON_ARRAY)
        return;

    for (f = adaptive->child; f; f = f->next)
    {
        at = json_get(f, "audioTrack");
        if (!at || !is_plain_aac(f))
            continue;
        track_from(at, &t);
        if (!t.id[0])
            continue;
        for (i = 0; i < count; ++i)
            if (strcmp(cand[i].id, t.id) == 0)
                break;
        if (i == count && count < MAX_CANDIDATES)
            cand[count++] = t;
    }
    if (count == 0)
        return;

    for (i = 0; i < count && orig < 0; ++i)
        if (cand[i].isOriginal)
            orig = i;
    if (orig < 0)
        orig = 0;   /* ninguna marcada: la primera */
    cand[orig].isOriginal = 1;
    p->tracks[p->trackCount++] = cand[orig];

    for (k = 0; k < 5; ++k)
    {
        if (strcmp(cand[orig].lang, langs[k]) == 0)
            continue;   /* la original ya es de ese idioma */
        best = -1;
        for (i = 0; i < count; ++i)
            if (i != orig && strcmp(cand[i].lang, langs[k]) == 0 &&
                (best < 0 || track_score(&cand[i]) > track_score(&cand[best])))
                best = i;
        if (best >= 0 && p->trackCount < YT_MAX_TRACKS)
            p->tracks[p->trackCount++] = cand[best];
    }
}

/* Posicion de la pista con ese id entre las elegidas, -1 si no esta */
static int track_index(const YtPlayer *p, const JsonNode *audioTrack)
{
    const char *id = json_str(json_get(audioTrack, "id"));
    int i;

    for (i = 0; id && i < p->trackCount; ++i)
        if (strcmp(p->tracks[i].id, id) == 0)
            return i;
    return -1;
}

static void add_format(YtPlayer *p, const JsonNode *f)
{
    YtFormat *fmt;
    const JsonNode *at;
    const char *c, *end, *mime;
    int n, track = -1;

    if (p->formatCount >= YT_MAX_FORMATS)
        return;

    /* audio: solo AAC normal y solo de las pistas elegidas */
    mime = json_str(json_get(f, "mimeType"));
    if (mime && strncmp(mime, "audio/", 6) == 0)
    {
        if (!is_plain_aac(f))
            return;
        at = json_get(f, "audioTrack");
        if (at && p->trackCount > 0)
        {
            track = track_index(p, at);
            if (track < 0)
                return;
        }
    }

    fmt = &p->formats[p->formatCount];
    memset(fmt, 0, sizeof(*fmt));

    fmt->itag = (int)num_of(json_get(f, "itag"));
    copy_utf8(fmt->mime, sizeof(fmt->mime), json_str(json_get(f, "mimeType")));
    fmt->isVideo = (strncmp(fmt->mime, "video/", 6) == 0);
    fmt->isAudio = (strncmp(fmt->mime, "audio/", 6) == 0);

    /* codecs="avc1.4d401e" -> avc1.4d401e */
    c = strstr(fmt->mime, "codecs=\"");
    if (c)
    {
        c += 8;
        end = strchr(c, '"');
        n = end ? (int)(end - c) : (int)strlen(c);
        if (n > (int)sizeof(fmt->codecs) - 1)
            n = (int)sizeof(fmt->codecs) - 1;
        memcpy(fmt->codecs, c, n);
        fmt->codecs[n] = 0;
    }

    fmt->width         = (int)num_of(json_get(f, "width"));
    fmt->height        = (int)num_of(json_get(f, "height"));
    fmt->fps           = (int)num_of(json_get(f, "fps"));
    fmt->bitrate       = (int)num_of(json_get(f, "bitrate"));
    fmt->sampleRate    = (int)num_of(json_get(f, "audioSampleRate"));
    fmt->contentLength = num_of(json_get(f, "contentLength"));
    copy_utf8(fmt->quality, sizeof(fmt->quality), json_str(json_get(f, "qualityLabel")));

    fmt->initStart  = (int)num_of(json_path(f, "initRange.start"));
    fmt->initEnd    = (int)num_of(json_path(f, "initRange.end"));
    fmt->indexStart = (int)num_of(json_path(f, "indexRange.start"));
    fmt->indexEnd   = (int)num_of(json_path(f, "indexRange.end"));

    copy_utf8(fmt->url, sizeof(fmt->url), json_str(json_get(f, "url")));
    fmt->track = track;

    p->formatCount++;
}

int yt_parse_player(char *json, YtPlayer *p)
{
    JsonDoc *doc;
    const JsonNode *ps, *sd, *list, *f;
    const char *lists[2];
    int i;

    memset(p, 0, sizeof(*p));
    doc = json_parse(json);
    if (!doc)
        return 0;

    ps = json_get(doc->root, "playabilityStatus");
    copy_utf8(p->status, sizeof(p->status), json_str(json_get(ps, "status")));
    copy_utf8(p->reason, sizeof(p->reason), json_str(json_get(ps, "reason")));

    p->lengthSeconds = (int)num_of(json_path(doc->root, "videoDetails.lengthSeconds"));

    sd = json_get(doc->root, "streamingData");
    p->expiresIn = (int)num_of(json_get(sd, "expiresInSeconds"));
    copy_utf8(p->hlsUrl, sizeof(p->hlsUrl), json_str(json_get(sd, "hlsManifestUrl")));

    pick_tracks(p, json_get(sd, "adaptiveFormats"));
    pick_captions(p, json_path(doc->root, "captions.playerCaptionsTracklistRenderer"));

    lists[0] = "formats";           /* audio y video juntos (VISIONOS no trae) */
    lists[1] = "adaptiveFormats";   /* audio y video separados */
    for (i = 0; i < 2; ++i)
    {
        list = json_get(sd, lists[i]);
        if (list && list->type == JSON_ARRAY)
            for (f = list->child; f; f = f->next)
                add_format(p, f);
    }

    json_free(doc);
    return 1;
}

int yt_player(const char *videoId, YtPlayer *p, char *err, int errSize)
{
    char id[16];
    char visitor[300];
    char headers[768];
    char body[1024];
    int bodyLen, i, n;
    HttpResponse resp;

    /* el id solo puede tener letras, numeros, - y _ */
    for (i = 0, n = 0; videoId[i] && n < 15; ++i)
    {
        char ch = videoId[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
            id[n++] = ch;
    }
    id[n] = 0;

    visitor[0] = 0;
    if (g_visitorData[0])
        _snprintf(visitor, sizeof(visitor) - 1, ",\"visitorData\":\"%s\"", g_visitorData);
    visitor[sizeof(visitor) - 1] = 0;

    _snprintf(headers, sizeof(headers) - 1,
              "Content-Type: application/json\r\n"
              "User-Agent: " YT_VISION_UA "\r\n"
              "X-YouTube-Client-Name: " YT_VISION_ID "\r\n"
              "X-YouTube-Client-Version: " YT_VISION_VERSION "\r\n"
              "%s%s%s",
              g_visitorData[0] ? "X-Goog-Visitor-Id: " : "",
              g_visitorData,
              g_visitorData[0] ? "\r\n" : "");
    headers[sizeof(headers) - 1] = 0;

    bodyLen = _snprintf(body, sizeof(body) - 1,
        "{\"context\":{\"client\":{"
        "\"clientName\":\"" YT_VISION_NAME "\","
        "\"clientVersion\":\"" YT_VISION_VERSION "\","
        "\"deviceMake\":\"Apple\","
        "\"deviceModel\":\"" YT_VISION_MODEL "\","
        "\"osName\":\"visionOS\","
        "\"osVersion\":\"" YT_VISION_OSVER "\","
        "\"hl\":\"%s\",\"gl\":\"%s\"%s}},"
        "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}",
        lang_hl(), lang_gl(), visitor, id);
    if (bodyLen < 0)
    {
        _snprintf(err, errSize - 1, "Peticion demasiado larga");
        err[errSize - 1] = 0;
        return 0;
    }

    memset(p, 0, sizeof(*p));
    if (!http_request("POST", YT_PLAYER_URL, headers, body, bodyLen, &resp, err, errSize))
        return 0;

    dump_debug(DEBUG_PLAYER_PATH, resp.body, resp.bodyLen);

    if (resp.status != 200)
    {
        _snprintf(err, errSize - 1, "YouTube respondio HTTP %d", resp.status);
        err[errSize - 1] = 0;
        http_free(&resp);
        return 0;
    }

    if (!yt_parse_player(resp.body, p))
    {
        http_free(&resp);
        _snprintf(err, errSize - 1, "No se pudo leer la respuesta (JSON invalido)");
        err[errSize - 1] = 0;
        return 0;
    }
    http_free(&resp);

    if (strcmp(p->status, "OK") != 0)
    {
        _snprintf(err, errSize - 1, "%s: %s", p->status[0] ? p->status : "SIN ESTADO",
                  p->reason[0] ? p->reason : "sin motivo");
        err[errSize - 1] = 0;
        return 0;
    }
    if (p->formatCount == 0 && !p->hlsUrl[0])
    {
        _snprintf(err, errSize - 1, "La respuesta no trae formatos");
        err[errSize - 1] = 0;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Descargas de prueba                                                 */

static const char *media_kind(const unsigned char *b, int len)
{
    if (len >= 8 && memcmp(b + 4, "ftyp", 4) == 0)
        return "MP4 (ftyp)";
    if (len >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3)
        return "WebM";
    if (len >= 1 && b[0] == 0x47)
        return "MPEG-TS";
    return "desconocido";
}

int yt_probe_range(const char *url, char *out, int outSize)
{
    HttpResponse r;
    char err[160];
    int ok;

    static const char headers[] =
        "User-Agent: " YT_VISION_UA "\r\n"
        "Range: bytes=0-1023\r\n";

    if (!url || !url[0])
    {
        _snprintf(out, outSize - 1, "Este formato no trae URL directa");
        out[outSize - 1] = 0;
        return 0;
    }
    if (!http_request("GET", url, headers, NULL, 0, &r, err, sizeof(err)))
    {
        _snprintf(out, outSize - 1, "Error: %s", err);
        out[outSize - 1] = 0;
        return 0;
    }

    ok = (r.status == 206 || r.status == 200) && r.bodyLen > 0;
    _snprintf(out, outSize - 1, "HTTP %d  \xC2\xB7  %d bytes  \xC2\xB7  %s",
              r.status, r.bodyLen, media_kind((const unsigned char *)r.body, r.bodyLen));
    out[outSize - 1] = 0;
    http_free(&r);
    return ok;
}

int yt_probe_hls(const char *url, char *out, int outSize)
{
    HttpResponse r;
    char err[160];
    char res[24];
    char resList[256];
    const char *p, *q, *eol;
    int variants = 0, avc = 0, n, rl = 0;

    static const char headers[] = "User-Agent: " YT_VISION_UA "\r\n";

    if (!url || !url[0])
    {
        _snprintf(out, outSize - 1, "Este video no trae lista HLS");
        out[outSize - 1] = 0;
        return 0;
    }
    if (!http_request("GET", url, headers, NULL, 0, &r, err, sizeof(err)))
    {
        _snprintf(out, outSize - 1, "Error: %s", err);
        out[outSize - 1] = 0;
        return 0;
    }
    dump_debug(DEBUG_HLS_PATH, r.body, r.bodyLen);

    /* cada variante empieza con #EXT-X-STREAM-INF: ... RESOLUTION=WxH ... */
    resList[0] = 0;
    for (p = strstr(r.body, "#EXT-X-STREAM-INF:"); p; p = strstr(p + 1, "#EXT-X-STREAM-INF:"))
    {
        ++variants;
        eol = strchr(p, '\n');
        q = strstr(p, "RESOLUTION=");
        if (q && (!eol || q < eol))
        {
            q += 11;
            for (n = 0; n < (int)sizeof(res) - 1 && ((q[n] >= '0' && q[n] <= '9') || q[n] == 'x'); ++n)
                res[n] = q[n];
            res[n] = 0;
            if (rl + n + 2 < (int)sizeof(resList))
            {
                resList[rl++] = ' ';
                memcpy(resList + rl, res, n + 1);
                rl += n;
            }
        }
        q = strstr(p, "avc1");
        if (q && (!eol || q < eol))
            ++avc;
    }

    _snprintf(out, outSize - 1, "HTTP %d  \xC2\xB7  %d variantes, %d H.264  \xC2\xB7 %s",
              r.status, variants, avc, resList);
    out[outSize - 1] = 0;

    n = (r.status == 200 && variants > 0);
    http_free(&r);
    return n;
}
