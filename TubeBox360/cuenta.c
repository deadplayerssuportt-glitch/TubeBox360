/* cuenta.c - Cuenta de Google: OAuth 2.0 para TV + YouTube Data API v3.
 * Ver cuenta.h. C89.
 *
 *   Iniciar sesion (flujo "dispositivo"):
 *     POST https://oauth2.googleapis.com/device/code   client_id, scope
 *       -> device_code, user_code, verification_url, expires_in, interval
 *     POST https://oauth2.googleapis.com/token  (cada "interval" segundos)
 *       grant_type=urn:ietf:params:oauth:grant-type:device_code
 *       -> authorization_pending / slow_down / access_denied / expired_token
 *       -> o access_token (1 h) + refresh_token (se guarda)
 *   Renovar: POST .../token  grant_type=refresh_token
 *   Cerrar:  POST https://oauth2.googleapis.com/revoke?token=...
 *
 *   Datos (Authorization: Bearer <access_token>), costo en cuota:
 *     channels?part=snippet&mine=true             1   nombre y foto
 *     subscriptions?part=snippet&mine=true        1   canales suscritos
 *     playlistItems?playlistId=UU...              1   videos nuevos (x10)
 *     videos?part=contentDetails,statistics       1   duracion y vistas
 *     videos/getRating?id=                        1
 *     videos/rate?id=&rating=like|dislike|none   50
 *   (La cuota por omision de un proyecto es 10 000 unidades al dia, para
 *   todos los usuarios juntos.)
 *
 * Nada de esto pasa por el registro de red con datos sensibles: los tokens
 * van en el cuerpo del POST o en el encabezado, y net_log solo anota la
 * direccion. */

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cuenta.h"
#include "version.h"
#include "net.h"
#include "json.h"
#include "lang.h"

#ifndef TB_GOOGLE_CLIENT_ID
#define TB_GOOGLE_CLIENT_ID ""
#endif
#ifndef TB_GOOGLE_CLIENT_SECRET
#define TB_GOOGLE_CLIENT_SECRET ""
#endif

#ifndef CUENTA_PATH
#define CUENTA_PATH  "game:\\cuenta.ini"
#endif
#ifndef OAUTH_BASE
#define OAUTH_BASE   "https://oauth2.googleapis.com"
#endif
#define OAUTH_DEVICE OAUTH_BASE "/device/code"
#define OAUTH_TOKEN  OAUTH_BASE "/token"
#define OAUTH_REVOKE OAUTH_BASE "/revoke"
#ifndef YT_API
#define YT_API       "https://www.googleapis.com/youtube/v3"
#endif
#define SCOPE_ENC    "https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fyoutube"
#define FORM_HDR     "Content-Type: application/x-www-form-urlencoded\r\n"

#define SUBS_FEED_CHANNELS 10   /* canales de los que se leen videos nuevos */
#define SUBS_PER_CHANNEL   5

/* ------------------------------------------------------------------ */
/* Estado (protegido por g_cs)                                          */

static CRITICAL_SECTION g_cs;
static int   g_csInit = 0;

static volatile LONG g_state = CUENTA_OFF;
static char  g_refresh[512];
static char  g_access[2048];
static DWORD g_accessUntil = 0;      /* GetTickCount en que vence */
static int   g_hasAccess = 0;
static char  g_name[128];
static char  g_avatar[320];
static char  g_handle[128];          /* "@usuario" (snippet.customUrl) */
static int   g_profileChecked = 0;   /* sesiones guardadas sin @: se lee una vez */

static int   g_msgId = 0;            /* texto de lang (S_...) o 0 */
static char  g_msgArg[160];

/* inicio de sesion */
static HANDLE         g_loginThread = NULL;
static volatile long  g_loginCancel = 0;
static char  g_userCode[32];
static char  g_verifyUrl[128];
static DWORD g_codeUntil = 0;

/* calificaciones */
#define RATE_CACHE 8
typedef struct { char id[16]; int rating; } RateEntry;
static RateEntry g_rates[RATE_CACHE];
static int       g_rateNext = 0;
static char      g_wantGet[16];      /* pedir getRating de este */
static char      g_wantSet[16];      /* mandar rate de este */
static int       g_wantSetVal = 0, g_wantSetOld = 0;
/* suscripciones a canales (boton Suscribirse) */
#define SUB_CACHE 8
typedef struct { char ch[32]; int state; char subId[64]; } SubEntry;
static SubEntry  g_subsC[SUB_CACHE];
static int       g_subNext = 0;
static char      g_wantSubGet[32];   /* revisar si esta suscrito */
static char      g_wantSubSet[32];   /* suscribir / cancelar */
static int       g_wantSubVal = 0;
static volatile LONG g_subVersion = 0;
static HANDLE    g_rateThread = NULL;
static HANDLE    g_rateEvent = NULL;
static volatile long g_quit = 0;

static void lock(void)   { EnterCriticalSection(&g_cs); }
static void unlock(void) { LeaveCriticalSection(&g_cs); }

static void set_msg(int id, const char *arg)
{
    lock();
    g_msgId = id;
    strncpy(g_msgArg, arg ? arg : "", sizeof(g_msgArg) - 1);
    g_msgArg[sizeof(g_msgArg) - 1] = 0;
    unlock();
}

static void copy(char *dst, int size, const char *src)
{
    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = 0;
}

/* %XX para el cuerpo de un formulario */
static void url_enc(const char *src, char *dst, int size)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;

    for (; *src && n < size - 4; ++src)
    {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            dst[n++] = (char)c;
        else
        {
            dst[n++] = '%';
            dst[n++] = hex[c >> 4];
            dst[n++] = hex[c & 15];
        }
    }
    dst[n] = 0;
}

static int client_ok(void)
{
    return TB_GOOGLE_CLIENT_ID[0] != 0;
}

/* ------------------------------------------------------------------ */
/* Archivo game:\cuenta.ini                                             */

static void save_file(void)
{
    FILE *f = fopen(CUENTA_PATH, "w");
    if (!f)
        return;
    lock();
    fprintf(f, "; TubeBox360 - sesion de Google. NO compartas este archivo.\n");
    fprintf(f, "refresh=%s\nnombre=%s\navatar=%s\nhandle=%s\n", g_refresh, g_name, g_avatar, g_handle);
    unlock();
    fclose(f);
}

static void load_file(void)
{
    char line[1024];
    FILE *f = fopen(CUENTA_PATH, "r");

    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        int n = (int)strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (strncmp(line, "refresh=", 8) == 0)
            copy(g_refresh, sizeof(g_refresh), line + 8);
        else if (strncmp(line, "nombre=", 7) == 0)
            copy(g_name, sizeof(g_name), line + 7);
        else if (strncmp(line, "avatar=", 7) == 0)
            copy(g_avatar, sizeof(g_avatar), line + 7);
        else if (strncmp(line, "handle=", 7) == 0)
        {
            copy(g_handle, sizeof(g_handle), line + 7);
            g_profileChecked = 1;
        }
    }
    fclose(f);
}

static void forget_session(void)
{
    lock();
    g_refresh[0] = g_access[0] = 0;
    g_hasAccess = 0;
    g_name[0] = g_avatar[0] = g_handle[0] = 0;
    g_profileChecked = 0;
    InterlockedExchange(&g_state, client_ok() ? CUENTA_OFF : CUENTA_NOCLIENT);
    unlock();
    DeleteFileA(CUENTA_PATH);
}

/* ------------------------------------------------------------------ */
/* Tokens                                                              */

/* POST de formulario a Google. Devuelve el estado HTTP (0 sin respuesta)
 * y deja el cuerpo en r (liberar con http_free si status > 0). */
static int post_form(const char *url, const char *body, HttpResponse *r, char *err, int errSize)
{
    if (!http_request("POST", url, FORM_HDR, body, (int)strlen(body), r, err, errSize))
        return 0;
    return r->status;
}

/* Lee access_token / expires_in / refresh_token de una respuesta de token */
static int take_tokens(char *body)
{
    JsonDoc *doc = json_parse(body);
    const char *at, *rt;
    JsonNode *ex;
    int ok = 0;

    if (!doc)
        return 0;
    at = json_str(json_get(doc->root, "access_token"));
    rt = json_str(json_get(doc->root, "refresh_token"));
    ex = json_get(doc->root, "expires_in");
    if (at && at[0])
    {
        int secs = ex && ex->type == JSON_NUMBER ? (int)ex->num : 3600;
        lock();
        copy(g_access, sizeof(g_access), at);
        if (rt && rt[0])
            copy(g_refresh, sizeof(g_refresh), rt);
        /* se renueva 2 min antes de que venza */
        g_accessUntil = GetTickCount() + (DWORD)(secs > 300 ? secs - 120 : secs / 2) * 1000UL;
        g_hasAccess = 1;
        unlock();
        ok = 1;
    }
    json_free(doc);
    return ok;
}

static void json_error(char *body, char *out, int size)
{
    JsonDoc *doc = json_parse(body);
    const char *e = NULL;

    out[0] = 0;
    if (!doc)
        return;
    e = json_str(json_get(doc->root, "error"));
    if (!e)   /* errores de la API de datos: {"error":{"message":..,"errors":[{"reason":..}]}} */
    {
        e = json_str(json_path(doc->root, "error.errors.0.reason"));
        if (!e)
            e = json_str(json_path(doc->root, "error.message"));
    }
    copy(out, size, e);
    json_free(doc);
}

/* Renueva el access token con el refresh token. 1 = bien. */
static int refresh_access(char *err, int errSize)
{
    char body[1400], enc[1100], why[96];
    HttpResponse r;
    int st;

    lock();
    url_enc(g_refresh, enc, sizeof(enc));
    unlock();
    if (!enc[0])
    {
        copy(err, errSize, "sin sesion");
        return 0;
    }
    _snprintf(body, sizeof(body) - 1, "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
              TB_GOOGLE_CLIENT_ID, TB_GOOGLE_CLIENT_SECRET, enc);
    body[sizeof(body) - 1] = 0;
    st = post_form(OAUTH_TOKEN, body, &r, err, errSize);
    if (st == 0)
        return 0;
    if (st == 200 && take_tokens(r.body))
    {
        http_free(&r);
        return 1;
    }
    json_error(r.body, why, sizeof(why));
    http_free(&r);
    net_log("[cuenta] no se pudo renovar la sesion: HTTP %d %s", st, why);
    if (strcmp(why, "invalid_grant") == 0)
    {
        /* el permiso se quito o vencio (proyecto en "Prueba": 7 dias) */
        forget_session();
        set_msg(S_G_SESSION_END, NULL);
        copy(err, errSize, T(S_G_SESSION_END));
    }
    else
        _snprintf(err, errSize - 1, "token HTTP %d %s", st, why), err[errSize - 1] = 0;
    return 0;
}

/* "Authorization: Bearer ...\r\n" (renueva si hace falta) */
static int auth_header(char *hdr, int size, char *err, int errSize)
{
    int need;

    lock();
    need = !g_hasAccess || (LONG)(GetTickCount() - g_accessUntil) >= 0;
    unlock();
    if (need && !refresh_access(err, errSize))
        return 0;
    lock();
    _snprintf(hdr, size - 1, "Authorization: Bearer %s\r\n", g_access);
    hdr[size - 1] = 0;
    unlock();
    return 1;
}

/* GET/POST a la API de datos con la sesion. Si el token ya no sirve (401)
 * se renueva una vez y se repite. Devuelve el estado HTTP (0 = sin
 * respuesta); con status > 0 hay que liberar r. */
static int api_call_body(const char *method, const char *url, const char *json, HttpResponse *r,
                         char *err, int errSize)
{
    char hdr[2300];
    int attempt;

    for (attempt = 0; attempt < 2; ++attempt)
    {
        if (!auth_header(hdr, sizeof(hdr) - 64, err, errSize))
            return 0;
        if (json)
            strcat(hdr, "Content-Type: application/json\r\n");
        if (!http_request(method, url, hdr, json ? json : (method[0] == 'P' ? "" : NULL),
                          json ? (int)strlen(json) : 0, r, err, errSize))
            return 0;
        if (r->status != 401 || attempt == 1)
            break;
        http_free(r);
        lock();
        g_hasAccess = 0;
        unlock();
    }
    if (r->status >= 400)
    {
        char why[96];
        json_error(r->body, why, sizeof(why));
        if (strstr(why, "quota") || strstr(why, "Quota"))
            copy(err, errSize, T(S_G_QUOTA));
        else
            _snprintf(err, errSize - 1, "API HTTP %d %s", r->status, why), err[errSize - 1] = 0;
        net_log("[cuenta] %s %.70s -> HTTP %d %s", method, url, r->status, why);
    }
    return r->status;
}

static int api_call(const char *method, const char *url, HttpResponse *r, char *err, int errSize)
{
    return api_call_body(method, url, NULL, r, err, errSize);
}

/* Nombre y foto del canal de la cuenta */
static void read_profile(void)
{
    char url[256], err[160];
    HttpResponse r;
    JsonDoc *doc;

    _snprintf(url, sizeof(url) - 1, "%s/channels?part=snippet&mine=true", YT_API);
    url[sizeof(url) - 1] = 0;
    if (api_call("GET", url, &r, err, sizeof(err)) == 0)
        return;
    if (r.status == 200 && (doc = json_parse(r.body)) != NULL)
    {
        JsonNode *sn = json_path(doc->root, "items.0.snippet");
        const char *t = json_str(json_get(sn, "title"));
        const char *h = json_str(json_get(sn, "customUrl"));   /* "@usuario" */
        const char *a = json_str(json_path(sn, "thumbnails.medium.url"));
        if (!a)
            a = json_str(json_path(sn, "thumbnails.default.url"));
        lock();
        copy(g_name, sizeof(g_name), t ? t : "Google");
        copy(g_avatar, sizeof(g_avatar), a);
        g_handle[0] = 0;
        if (h && h[0])
            _snprintf(g_handle, sizeof(g_handle) - 1, "%s%s", h[0] == '@' ? "" : "@", h);
        g_handle[sizeof(g_handle) - 1] = 0;
        g_profileChecked = 1;
        unlock();
        json_free(doc);
    }
    http_free(&r);
}

/* ------------------------------------------------------------------ */
/* Inicio de sesion                                                    */

static void login_fail(const char *why)
{
    InterlockedExchange(&g_state, CUENTA_OFF);
    set_msg(S_G_FAIL, why);
    net_log("[cuenta] inicio de sesion: %s", why);
}

static DWORD WINAPI login_thread(LPVOID param)
{
    char body[1200], enc[600], device[512], err[160], why[96];
    HttpResponse r;
    JsonDoc *doc;
    int st, interval = 5, expires = 900;
    DWORD next;

    (void)param;
    InterlockedExchange(&g_state, CUENTA_ASKING);

    _snprintf(body, sizeof(body) - 1, "client_id=%s&scope=%s", TB_GOOGLE_CLIENT_ID, SCOPE_ENC);
    body[sizeof(body) - 1] = 0;
    st = post_form(OAUTH_DEVICE, body, &r, err, sizeof(err));
    if (g_loginCancel)
    {
        if (st)
            http_free(&r);
        InterlockedExchange(&g_state, CUENTA_OFF);
        return 0;
    }
    if (st == 0)
    {
        login_fail(err);
        return 0;
    }
    device[0] = 0;
    if (st == 200 && (doc = json_parse(r.body)) != NULL)
    {
        JsonNode *n;
        const char *u = json_str(json_get(doc->root, "verification_url"));
        if (!u)
            u = json_str(json_get(doc->root, "verification_uri"));
        copy(device, sizeof(device), json_str(json_get(doc->root, "device_code")));
        lock();
        copy(g_userCode, sizeof(g_userCode), json_str(json_get(doc->root, "user_code")));
        copy(g_verifyUrl, sizeof(g_verifyUrl), u ? u : "https://www.google.com/device");
        unlock();
        if ((n = json_get(doc->root, "interval")) != NULL && n->type == JSON_NUMBER && n->num >= 1)
            interval = (int)n->num;
        if ((n = json_get(doc->root, "expires_in")) != NULL && n->type == JSON_NUMBER && n->num > 30)
            expires = (int)n->num;
        json_free(doc);
    }
    else
        json_error(r.body, why, sizeof(why));
    if (!device[0] || !g_userCode[0])
    {
        _snprintf(err, sizeof(err) - 1, "HTTP %d %s", st, st == 200 ? "" : why);
        err[sizeof(err) - 1] = 0;
        http_free(&r);
        login_fail(err);
        return 0;
    }
    http_free(&r);

    g_codeUntil = GetTickCount() + (DWORD)expires * 1000UL;
    InterlockedExchange(&g_state, CUENTA_CODE);
    net_log("[cuenta] codigo listo (vence en %d s, cada %d s)", expires, interval);

    url_enc(device, enc, sizeof(enc));
    next = GetTickCount() + (DWORD)interval * 1000UL;
    for (;;)
    {
        /* esperar el intervalo en pedacitos (para poder cancelar) */
        while (!g_loginCancel && (LONG)(GetTickCount() - next) < 0)
            Sleep(100);
        if (g_loginCancel)
        {
            InterlockedExchange(&g_state, CUENTA_OFF);
            return 0;
        }
        if ((LONG)(GetTickCount() - g_codeUntil) >= 0)
        {
            InterlockedExchange(&g_state, CUENTA_OFF);
            set_msg(S_G_EXPIRED, NULL);
            return 0;
        }
        _snprintf(body, sizeof(body) - 1,
                  "client_id=%s&client_secret=%s&device_code=%s"
                  "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
                  TB_GOOGLE_CLIENT_ID, TB_GOOGLE_CLIENT_SECRET, enc);
        body[sizeof(body) - 1] = 0;
        st = post_form(OAUTH_TOKEN, body, &r, err, sizeof(err));
        next = GetTickCount() + (DWORD)interval * 1000UL;
        if (st == 0)
            continue;   /* sin red un momento: se vuelve a intentar */
        if (st == 200 && take_tokens(r.body))
        {
            http_free(&r);
            break;
        }
        json_error(r.body, why, sizeof(why));
        http_free(&r);
        if (strcmp(why, "authorization_pending") == 0)
            continue;
        if (strcmp(why, "slow_down") == 0)
        {
            interval += 5;
            next += 5000UL;
            continue;
        }
        if (strcmp(why, "access_denied") == 0)
        {
            InterlockedExchange(&g_state, CUENTA_OFF);
            set_msg(S_G_DENIED, NULL);
            return 0;
        }
        if (strcmp(why, "expired_token") == 0)
        {
            InterlockedExchange(&g_state, CUENTA_OFF);
            set_msg(S_G_EXPIRED, NULL);
            return 0;
        }
        _snprintf(err, sizeof(err) - 1, "HTTP %d %s", st, why);
        err[sizeof(err) - 1] = 0;
        login_fail(err);
        return 0;
    }

    InterlockedExchange(&g_state, CUENTA_FINISH);
    read_profile();
    if (g_loginCancel)
    {
        forget_session();
        return 0;
    }
    save_file();
    lock();
    g_rateNext = 0;
    memset(g_rates, 0, sizeof(g_rates));
    memset(g_subsC, 0, sizeof(g_subsC));
    unlock();
    InterlockedExchange(&g_state, CUENTA_ON);
    set_msg(S_G_DONE, g_name);
    net_log("[cuenta] sesion iniciada");
    return 0;
}

void cuenta_login_start(void)
{
    int st = (int)g_state;

    if (!g_csInit || !client_ok() || st == CUENTA_ASKING || st == CUENTA_CODE || st == CUENTA_FINISH)
        return;
    if (g_loginThread)
    {
        WaitForSingleObject(g_loginThread, 3000);
        CloseHandle(g_loginThread);
        g_loginThread = NULL;
    }
    cuenta_clear_msg();
    g_userCode[0] = g_verifyUrl[0] = 0;
    g_loginCancel = 0;
    InterlockedExchange(&g_state, CUENTA_ASKING);
    g_loginThread = CreateThread(NULL, 256 * 1024, login_thread, NULL, 0, NULL);
    if (!g_loginThread)
        login_fail("CreateThread");
}

void cuenta_login_cancel(void)
{
    g_loginCancel = 1;   /* el hilo regresa a CUENTA_OFF en <= 0.1 s (o al terminar la peticion) */
    if (g_state == CUENTA_ASKING || g_state == CUENTA_CODE)
        InterlockedExchange(&g_state, CUENTA_OFF);
}

void cuenta_code(char *code, int codeSize, char *url, int urlSize, int *secsLeft)
{
    LONG left;

    lock();
    copy(code, codeSize, g_userCode);
    copy(url, urlSize, g_verifyUrl);
    unlock();
    /* "https://www.google.com/device" -> "google.com/device" (mas facil de leer) */
    if (strncmp(url, "https://", 8) == 0)
        memmove(url, url + 8, strlen(url + 8) + 1);
    if (strncmp(url, "www.", 4) == 0)
        memmove(url, url + 4, strlen(url + 4) + 1);
    left = (LONG)(g_codeUntil - GetTickCount());
    if (secsLeft)
        *secsLeft = left > 0 ? (int)(left / 1000) : 0;
}

/* ------------------------------------------------------------------ */
/* Cerrar sesion                                                       */

static DWORD WINAPI revoke_thread(LPVOID param)
{
    char *tok = (char *)param;
    char body[1200], enc[1100], err[120];
    HttpResponse r;

    url_enc(tok, enc, sizeof(enc));
    _snprintf(body, sizeof(body) - 1, "token=%s", enc);
    body[sizeof(body) - 1] = 0;
    if (post_form(OAUTH_REVOKE, body, &r, err, sizeof(err)))
    {
        net_log("[cuenta] permiso revocado (HTTP %d)", r.status);
        http_free(&r);
    }
    free(tok);
    return 0;
}

void cuenta_logout(void)
{
    char *tok;
    HANDLE h;

    if (!g_csInit)
        return;
    cuenta_login_cancel();
    lock();
    tok = g_refresh[0] ? (char *)malloc(strlen(g_refresh) + 1) : NULL;
    if (tok)
        strcpy(tok, g_refresh);
    unlock();
    forget_session();
    lock();
    memset(g_rates, 0, sizeof(g_rates));
    memset(g_subsC, 0, sizeof(g_subsC));
    unlock();
    if (tok)
    {
        h = CreateThread(NULL, 128 * 1024, revoke_thread, tok, 0, NULL);
        if (h)
            CloseHandle(h);
        else
            free(tok);
    }
    net_log("[cuenta] sesion cerrada");
}

/* ------------------------------------------------------------------ */
/* Suscripciones                                                       */

typedef struct { YtVideo v; char at[32]; } FeedItem;

/* "PT1H2M3S" -> "1:02:03";  "PT4M5S" -> "4:05" */
static void iso_duration(const char *s, char *out, int size)
{
    int h = 0, m = 0, sec = 0, v = 0;

    out[0] = 0;
    if (!s || strncmp(s, "P", 1) != 0)
        return;
    if (strcmp(s, "P0D") == 0)
        return;   /* en vivo */
    for (; *s; ++s)
    {
        if (*s >= '0' && *s <= '9')
            v = v * 10 + (*s - '0');
        else
        {
            if (*s == 'D') h += v * 24;
            else if (*s == 'H') h += v;
            else if (*s == 'M') m = v;
            else if (*s == 'S') sec = v;
            v = 0;
        }
    }
    if (h > 0)
        _snprintf(out, size - 1, "%d:%02d:%02d", h, m, sec);
    else
        _snprintf(out, size - 1, "%d:%02d", m, sec);
    out[size - 1] = 0;
}

/* 1234567 -> "1.2 M"; 12345 -> "12 K" */
static void short_count(const char *s, char *out, int size)
{
    double n = s ? atof(s) : 0.0;

    if (n >= 1e9)
        _snprintf(out, size - 1, "%.1f B", n / 1e9);
    else if (n >= 1e6)
        _snprintf(out, size - 1, n >= 1e7 ? "%.0f M" : "%.1f M", n / 1e6);
    else if (n >= 1e3)
        _snprintf(out, size - 1, n >= 1e4 ? "%.0f K" : "%.1f K", n / 1e3);
    else
        _snprintf(out, size - 1, "%.0f", n);
    out[size - 1] = 0;
}

/* "2026-11-20T15:00:00Z" -> "20 nov. 2026" (o "2026年11月20日") */
static void iso_date(const char *s, char *out, int size)
{
    int y = 0, mo = 0, d = 0;

    out[0] = 0;
    if (!s || sscanf(s, "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12)
        return;
    if (lang_get() == LANG_JA)
        _snprintf(out, size - 1, "%d\xE5\xB9\xB4%s%d\xE6\x97\xA5", y, lang_month(mo - 1), d);
    else if (lang_get() == LANG_EN)
        _snprintf(out, size - 1, "%s %d, %d", lang_month(mo - 1), d, y);
    else
        _snprintf(out, size - 1, "%d %s %d", d, lang_month(mo - 1), y);
    out[size - 1] = 0;
}

static int cmp_feed(const void *a, const void *b)
{
    return strcmp(((const FeedItem *)b)->at, ((const FeedItem *)a)->at);   /* el mas nuevo primero */
}

/* duracion y vistas de los videos del feed (un solo pedido, 50 max) */
static void fill_details(FeedItem *items, int n)
{
    char url[1024], err[160];
    HttpResponse r;
    JsonDoc *doc;
    JsonNode *it;
    int i, len;

    if (n <= 0)
        return;
    len = _snprintf(url, sizeof(url) - 1, "%s/videos?part=contentDetails,statistics&maxResults=50&id=", YT_API);
    for (i = 0; i < n && len < (int)sizeof(url) - 20; ++i)
        len += _snprintf(url + len, sizeof(url) - 1 - len, "%s%s", i ? "," : "", items[i].v.id);
    url[sizeof(url) - 1] = 0;
    if (api_call("GET", url, &r, err, sizeof(err)) == 0)
        return;
    if (r.status == 200 && (doc = json_parse(r.body)) != NULL)
    {
        JsonNode *arr = json_get(doc->root, "items");
        for (it = arr ? arr->child : NULL; it; it = it->next)
        {
            const char *id = json_str(json_get(it, "id"));
            for (i = 0; id && i < n; ++i)
            {
                char cnt[32];
                const char *vc;
                if (strcmp(items[i].v.id, id) != 0)
                    continue;
                iso_duration(json_str(json_path(it, "contentDetails.duration")), items[i].v.length,
                             sizeof(items[i].v.length));
                vc = json_str(json_path(it, "statistics.viewCount"));
                if (vc)
                {
                    short_count(vc, cnt, sizeof(cnt));
                    _snprintf(items[i].v.views, sizeof(items[i].v.views) - 1, T(S_N_VIEWS), cnt);
                    items[i].v.views[sizeof(items[i].v.views) - 1] = 0;
                }
                break;
            }
        }
        json_free(doc);
    }
    http_free(&r);
}

int cuenta_subscriptions(YtVideo *vids, int maxVids, int *nVids,
                         YtVideo *chans, int maxChans, int *nChans,
                         char *err, int errSize)
{
    char url[512];
    HttpResponse r;
    JsonDoc *doc;
    JsonNode *it, *arr;
    FeedItem *feed;
    int nf = 0, i, c, st;

    *nVids = *nChans = 0;
    if (g_state != CUENTA_ON)
    {
        copy(err, errSize, T(S_SUBS_NEED));
        return 0;
    }

    /* 1. canales suscritos, primero los que tienen actividad nueva */
    _snprintf(url, sizeof(url) - 1, "%s/subscriptions?part=snippet&mine=true&maxResults=50&order=unread", YT_API);
    url[sizeof(url) - 1] = 0;
    st = api_call("GET", url, &r, err, errSize);
    if (st == 0)
        return 0;
    if (st != 200 || (doc = json_parse(r.body)) == NULL)
    {
        if (st == 200)
            copy(err, errSize, "JSON");
        http_free(&r);
        return 0;
    }
    arr = json_get(doc->root, "items");
    for (it = arr ? arr->child : NULL; it && *nChans < maxChans; it = it->next)
    {
        JsonNode *sn = json_get(it, "snippet");
        const char *id = json_str(json_path(sn, "resourceId.channelId"));
        const char *t = json_str(json_get(sn, "title"));
        const char *a = json_str(json_path(sn, "thumbnails.medium.url"));
        YtVideo *v;
        if (!id || !t)
            continue;
        if (!a)
            a = json_str(json_path(sn, "thumbnails.default.url"));
        v = &chans[*nChans];
        memset(v, 0, sizeof(*v));
        v->kind = YT_KIND_CHANNEL;
        copy(v->title, sizeof(v->title), t);
        copy(v->channel, sizeof(v->channel), t);
        copy(v->channelId, sizeof(v->channelId), id);
        copy(v->avatar, sizeof(v->avatar), a);
        copy(v->views, sizeof(v->views), T(S_SUBSCRIBED));
        ++*nChans;
    }
    json_free(doc);
    http_free(&r);

    /* 2. videos nuevos de los primeros canales (lista "UU..." = subidos) */
    feed = (FeedItem *)malloc(sizeof(FeedItem) * SUBS_FEED_CHANNELS * SUBS_PER_CHANNEL);
    if (!feed)
        return 1;
    for (c = 0; c < *nChans && c < SUBS_FEED_CHANNELS; ++c)
    {
        const char *cid = chans[c].channelId;
        if (strncmp(cid, "UC", 2) != 0)
            continue;
        _snprintf(url, sizeof(url) - 1, "%s/playlistItems?part=snippet,contentDetails&maxResults=%d&playlistId=UU%s",
                  YT_API, SUBS_PER_CHANNEL, cid + 2);
        url[sizeof(url) - 1] = 0;
        st = api_call("GET", url, &r, err, errSize);
        if (st == 0)
            continue;
        if (st == 200 && (doc = json_parse(r.body)) != NULL)
        {
            arr = json_get(doc->root, "items");
            for (it = arr ? arr->child : NULL; it && nf < SUBS_FEED_CHANNELS * SUBS_PER_CHANNEL; it = it->next)
            {
                const char *vid = json_str(json_path(it, "contentDetails.videoId"));
                const char *at = json_str(json_path(it, "contentDetails.videoPublishedAt"));
                const char *t = json_str(json_path(it, "snippet.title"));
                FeedItem *f;
                if (!vid || !t || strlen(vid) >= sizeof(f->v.id))
                    continue;
                if (!at)
                    at = json_str(json_path(it, "snippet.publishedAt"));
                if (!at)
                    continue;   /* video privado o borrado */
                f = &feed[nf++];
                memset(f, 0, sizeof(*f));
                f->v.kind = YT_KIND_VIDEO;
                copy(f->v.id, sizeof(f->v.id), vid);
                copy(f->v.title, sizeof(f->v.title), t);
                copy(f->v.channel, sizeof(f->v.channel), chans[c].title);
                copy(f->v.channelId, sizeof(f->v.channelId), cid);
                copy(f->v.avatar, sizeof(f->v.avatar), chans[c].avatar);
                copy(f->at, sizeof(f->at), at);
                iso_date(at, f->v.published, sizeof(f->v.published));
            }
            json_free(doc);
        }
        http_free(&r);
    }
    err[0] = 0;   /* un canal que fallo no estropea el resto */

    qsort(feed, nf, sizeof(FeedItem), cmp_feed);
    if (nf > maxVids)
        nf = maxVids;
    fill_details(feed, nf);
    for (i = 0; i < nf; ++i)
        vids[i] = feed[i].v;
    *nVids = nf;
    free(feed);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Me gusta / no me gusta                                              */

static RateEntry *find_rate(const char *id)
{
    int i;
    for (i = 0; i < RATE_CACHE; ++i)
        if (g_rates[i].id[0] && strcmp(g_rates[i].id, id) == 0)
            return &g_rates[i];
    return NULL;
}

static void put_rate(const char *id, int rating)
{
    RateEntry *e = find_rate(id);
    if (!e)
    {
        e = &g_rates[g_rateNext];
        g_rateNext = (g_rateNext + 1) % RATE_CACHE;
        copy(e->id, sizeof(e->id), id);
    }
    e->rating = rating;
}

static int rating_from(const char *s)
{
    if (!s)
        return RATE_NONE;
    if (strcmp(s, "like") == 0)
        return RATE_LIKE;
    if (strcmp(s, "dislike") == 0)
        return RATE_DISLIKE;
    return RATE_NONE;
}

static SubEntry *find_sub(const char *ch)
{
    int i;
    for (i = 0; i < SUB_CACHE; ++i)
        if (g_subsC[i].ch[0] && strcmp(g_subsC[i].ch, ch) == 0)
            return &g_subsC[i];
    return NULL;
}

static SubEntry *put_sub(const char *ch, int state)
{
    SubEntry *e = find_sub(ch);
    if (!e)
    {
        e = &g_subsC[g_subNext];
        g_subNext = (g_subNext + 1) % SUB_CACHE;
        memset(e, 0, sizeof(*e));
        copy(e->ch, sizeof(e->ch), ch);
    }
    e->state = state;
    return e;
}

/* Suscribir (POST subscriptions) o cancelar (DELETE subscriptions?id=) */
static void do_sub_action(const char *ch, int want)
{
    char url[300], body[256], subId[64], err[160];
    HttpResponse r;
    JsonDoc *doc;
    SubEntry *e;
    int st;

    if (want == SUB_YES)
    {
        _snprintf(url, sizeof(url) - 1, "%s/subscriptions?part=snippet", YT_API);
        url[sizeof(url) - 1] = 0;
        _snprintf(body, sizeof(body) - 1,
                  "{\"snippet\":{\"resourceId\":{\"kind\":\"youtube#channel\",\"channelId\":\"%s\"}}}", ch);
        body[sizeof(body) - 1] = 0;
        st = api_call_body("POST", url, body, &r, err, sizeof(err));
        if (st == 200 && (doc = json_parse(r.body)) != NULL)
        {
            lock();
            e = put_sub(ch, SUB_YES);
            copy(e->subId, sizeof(e->subId), json_str(json_get(doc->root, "id")));
            unlock();
            json_free(doc);
            InterlockedIncrement(&g_subVersion);
        }
        else
        {
            if (st == 400 && r.body && strstr(r.body, "subscriptionDuplicate"))
            {
                lock();
                put_sub(ch, SUB_YES);   /* ya estaba suscrito */
                unlock();
            }
            else
            {
                lock();
                put_sub(ch, SUB_NO);
                unlock();
                set_msg(S_SUB_FAIL, err);
            }
        }
        if (st > 0)
            http_free(&r);
        return;
    }

    lock();
    e = find_sub(ch);
    copy(subId, sizeof(subId), e ? e->subId : "");
    unlock();
    if (!subId[0])
    {
        /* no se sabe el id de la suscripcion: buscarlo */
        _snprintf(url, sizeof(url) - 1, "%s/subscriptions?part=id&mine=true&forChannelId=%s", YT_API, ch);
        url[sizeof(url) - 1] = 0;
        st = api_call("GET", url, &r, err, sizeof(err));
        if (st == 200 && (doc = json_parse(r.body)) != NULL)
        {
            copy(subId, sizeof(subId), json_str(json_path(doc->root, "items.0.id")));
            json_free(doc);
        }
        if (st > 0)
            http_free(&r);
    }
    if (!subId[0])
    {
        lock();
        put_sub(ch, SUB_NO);
        unlock();
        return;
    }
    _snprintf(url, sizeof(url) - 1, "%s/subscriptions?id=%s", YT_API, subId);
    url[sizeof(url) - 1] = 0;
    st = api_call("DELETE", url, &r, err, sizeof(err));
    if (st > 0)
        http_free(&r);
    lock();
    if (st == 204 || st == 200 || st == 404)
    {
        e = put_sub(ch, SUB_NO);
        e->subId[0] = 0;
        InterlockedIncrement(&g_subVersion);
    }
    else
    {
        put_sub(ch, SUB_YES);
        set_msg(S_SUB_FAIL, err);
    }
    unlock();
}

static void do_sub_check(const char *ch)
{
    char url[300], err[160];
    HttpResponse r;
    JsonDoc *doc;

    _snprintf(url, sizeof(url) - 1, "%s/subscriptions?part=id&mine=true&forChannelId=%s", YT_API, ch);
    url[sizeof(url) - 1] = 0;
    if (api_call("GET", url, &r, err, sizeof(err)) == 0)
        return;
    if (r.status == 200 && (doc = json_parse(r.body)) != NULL)
    {
        const char *id = json_str(json_path(doc->root, "items.0.id"));
        lock();
        if (!g_wantSubSet[0] || strcmp(g_wantSubSet, ch) != 0)   /* no pisar un clic reciente */
        {
            SubEntry *e = put_sub(ch, id ? SUB_YES : SUB_NO);
            copy(e->subId, sizeof(e->subId), id);
        }
        unlock();
        json_free(doc);
    }
    http_free(&r);
}

static DWORD WINAPI rate_thread(LPVOID param)
{
    char id[16], url[256], err[160];
    HttpResponse r;
    JsonDoc *doc;
    int val, old, st;

    (void)param;
    while (!g_quit)
    {
        WaitForSingleObject(g_rateEvent, 1000);
        if (g_quit)
            break;
        if (g_state != CUENTA_ON)
            continue;

        /* sesion guardada de antes (sin @usuario): leer el perfil una vez */
        if (!g_profileChecked)
        {
            g_profileChecked = 1;
            read_profile();
            save_file();
        }

        /* suscribirse / cancelar (lo pidio el usuario) */
        {
            char ch[32];
            int want;
            lock();
            copy(ch, sizeof(ch), g_wantSubSet);
            want = g_wantSubVal;
            g_wantSubSet[0] = 0;
            unlock();
            if (ch[0])
            {
                do_sub_action(ch, want);
                continue;
            }
            lock();
            copy(ch, sizeof(ch), g_wantSubGet);
            g_wantSubGet[0] = 0;
            unlock();
            if (ch[0])
                do_sub_check(ch);
        }

        /* primero lo que el usuario pidio (calificar) */
        lock();
        copy(id, sizeof(id), g_wantSet);
        val = g_wantSetVal;
        old = g_wantSetOld;
        g_wantSet[0] = 0;
        unlock();
        if (id[0])
        {
            _snprintf(url, sizeof(url) - 1, "%s/videos/rate?id=%s&rating=%s", YT_API, id,
                      val == RATE_LIKE ? "like" : val == RATE_DISLIKE ? "dislike" : "none");
            url[sizeof(url) - 1] = 0;
            st = api_call("POST", url, &r, err, sizeof(err));
            if (st > 0)
                http_free(&r);
            if (st != 204 && st != 200)
            {
                lock();
                put_rate(id, old);   /* regresa como estaba */
                unlock();
                set_msg(S_RATE_FAIL, err);
            }
            continue;
        }

        lock();
        copy(id, sizeof(id), g_wantGet);
        g_wantGet[0] = 0;
        unlock();
        if (!id[0])
            continue;
        _snprintf(url, sizeof(url) - 1, "%s/videos/getRating?id=%s", YT_API, id);
        url[sizeof(url) - 1] = 0;
        st = api_call("GET", url, &r, err, sizeof(err));
        if (st == 0)
            continue;
        if (st == 200 && (doc = json_parse(r.body)) != NULL)
        {
            int v = rating_from(json_str(json_path(doc->root, "items.0.rating")));
            lock();
            if (!g_wantSet[0] || strcmp(g_wantSet, id) != 0)   /* (no pisar un clic que acaba de llegar) */
                put_rate(id, v);
            unlock();
            json_free(doc);
        }
        http_free(&r);
    }
    return 0;
}

void cuenta_sub_request(const char *channelId)
{
    if (!g_csInit || g_state != CUENTA_ON || !channelId || strncmp(channelId, "UC", 2) != 0)
        return;
    lock();
    if (!find_sub(channelId) && strcmp(g_wantSubGet, channelId) != 0)
    {
        copy(g_wantSubGet, sizeof(g_wantSubGet), channelId);
        if (g_rateEvent)
            SetEvent(g_rateEvent);
    }
    unlock();
}

int cuenta_sub_state(const char *channelId)
{
    SubEntry *e;
    int v = SUB_UNKNOWN;

    if (!g_csInit || !channelId || g_state != CUENTA_ON)
        return SUB_UNKNOWN;
    lock();
    e = find_sub(channelId);
    if (e)
        v = e->state;
    unlock();
    return v;
}

void cuenta_sub_set(const char *channelId, int subscribe)
{
    if (!g_csInit || g_state != CUENTA_ON || !channelId || strncmp(channelId, "UC", 2) != 0)
        return;
    lock();
    put_sub(channelId, subscribe ? SUB_YES : SUB_NO);   /* se ve al instante */
    copy(g_wantSubSet, sizeof(g_wantSubSet), channelId);
    g_wantSubVal = subscribe ? SUB_YES : SUB_NO;
    if (g_rateEvent)
        SetEvent(g_rateEvent);
    unlock();
}

int cuenta_sub_version(void)
{
    return (int)g_subVersion;
}

void cuenta_rating_request(const char *videoId)
{
    if (!g_csInit || g_state != CUENTA_ON || !videoId || !videoId[0])
        return;
    lock();
    if (!find_rate(videoId))
    {
        copy(g_wantGet, sizeof(g_wantGet), videoId);
        if (g_rateEvent)
            SetEvent(g_rateEvent);
    }
    unlock();
}

int cuenta_rating(const char *videoId)
{
    RateEntry *e;
    int v = RATE_UNKNOWN;

    if (!g_csInit || !videoId)
        return RATE_UNKNOWN;
    lock();
    e = find_rate(videoId);
    if (e)
        v = e->rating;
    unlock();
    return v;
}

void cuenta_rate(const char *videoId, int rating)
{
    RateEntry *e;
    int old = RATE_NONE;

    if (!g_csInit || g_state != CUENTA_ON || !videoId || !videoId[0])
        return;
    lock();
    e = find_rate(videoId);
    if (e && e->rating != RATE_UNKNOWN)
        old = e->rating;
    put_rate(videoId, rating);
    copy(g_wantSet, sizeof(g_wantSet), videoId);
    g_wantSetVal = rating;
    g_wantSetOld = old;
    if (g_rateEvent)
        SetEvent(g_rateEvent);
    unlock();
}

/* ------------------------------------------------------------------ */

void cuenta_init(void)
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = 1;
    }
    if (!client_ok())
    {
        InterlockedExchange(&g_state, CUENTA_NOCLIENT);
        return;
    }
    load_file();
    InterlockedExchange(&g_state, g_refresh[0] ? CUENTA_ON : CUENTA_OFF);
    if (g_refresh[0])
        net_log("[cuenta] hay una sesion guardada");
    g_quit = 0;
    g_rateEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_rateThread = CreateThread(NULL, 128 * 1024, rate_thread, NULL, 0, NULL);
}

void cuenta_shutdown(void)
{
    if (!g_csInit)
        return;
    g_loginCancel = 1;
    g_quit = 1;
    if (g_rateEvent)
        SetEvent(g_rateEvent);
    if (g_rateThread)
    {
        WaitForSingleObject(g_rateThread, 2000);
        CloseHandle(g_rateThread);
        g_rateThread = NULL;
    }
    if (g_loginThread)
    {
        WaitForSingleObject(g_loginThread, 1000);
        CloseHandle(g_loginThread);
        g_loginThread = NULL;
    }
}

int cuenta_state(void)
{
    return (int)g_state;
}

int cuenta_signed_in(void)
{
    return g_state == CUENTA_ON;
}

const char *cuenta_name(void)
{
    return g_state == CUENTA_ON ? g_name : "";
}

const char *cuenta_handle(void)
{
    return g_state == CUENTA_ON ? g_handle : "";
}

const char *cuenta_avatar(void)
{
    return g_state == CUENTA_ON ? g_avatar : "";
}

const char *cuenta_msg(void)
{
    static char buf[320];
    int id;
    char arg[160];

    if (!g_csInit)
        return "";
    lock();
    id = g_msgId;
    copy(arg, sizeof(arg), g_msgArg);
    unlock();
    if (!id)
        return "";
    if (strstr(T(id), "%s"))
        _snprintf(buf, sizeof(buf) - 1, T(id), arg);
    else
        _snprintf(buf, sizeof(buf) - 1, "%s", T(id));
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

void cuenta_clear_msg(void)
{
    if (!g_csInit)
        return;
    lock();
    g_msgId = 0;
    g_msgArg[0] = 0;
    unlock();
}
