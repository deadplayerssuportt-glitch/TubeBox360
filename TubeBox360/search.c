/* search.c - Teclado de busqueda propio. C89. Ver search.h.
 *
 * Las sugerencias salen de suggestqueries.google.com (las mismas que da
 * YouTube al escribir). Un hilo las pide 0.2 s despues del ultimo
 * teclazo, para no hacer una peticion por letra. */
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "search.h"
#include "ui.h"
#include "gfx.h"
#include "ui_assets.h"
#include "net.h"
#include "json.h"
#include "lang.h"
#include "sfx.h"

#ifndef INFINITE
#define INFINITE 0xFFFFFFFF
#endif

#define TEXT_MAX     100        /* bytes de lo escrito (UTF-8) */
#define SUG_MAX      10
#define SUG_LEN      128
#define RECENT_MAX   20
#define RECENT_PATH  "game:\\busquedas.txt"

/* Teclas especiales */
#define K_BKSP "\x01"
#define K_MODE "\x02"

typedef struct { const char *ins; const char *label; } Key;

/* [tabla][fila][columna]; las filas 2 y 3 tienen 7 teclas. Tablas: letras
 * (con N, o con C cedilla en portugues) y simbolos de cada idioma. */
enum { KT_LETTERS, KT_LETTERS_PT, KT_SYM_ES, KT_SYM_EN, KT_SYM_PT, KT_LETTERS_AR, KT_SYM_AR, KT_COUNT };

static const Key kKeys[KT_COUNT][4][8] = {
    {   /* letras */
        { {"a","A"}, {"b","B"}, {"c","C"}, {"d","D"}, {"e","E"}, {"f","F"}, {"g","G"}, {K_BKSP,""} },
        { {"h","H"}, {"i","I"}, {"j","J"}, {"k","K"}, {"l","L"}, {"m","M"}, {"n","N"}, {K_MODE,"&123"} },
        { {"\xC3\xB1","\xC3\x91"}, {"o","O"}, {"p","P"}, {"q","Q"}, {"r","R"}, {"s","S"}, {"t","T"}, {NULL,NULL} },
        { {"u","U"}, {"v","V"}, {"w","W"}, {"x","X"}, {"y","Y"}, {"z","Z"}, {"-","-"}, {NULL,NULL} }
    },
    {   /* letras (portugues: C cedilla en lugar de N) */
        { {"a","A"}, {"b","B"}, {"c","C"}, {"d","D"}, {"e","E"}, {"f","F"}, {"g","G"}, {K_BKSP,""} },
        { {"h","H"}, {"i","I"}, {"j","J"}, {"k","K"}, {"l","L"}, {"m","M"}, {"n","N"}, {K_MODE,"&123"} },
        { {"\xC3\xA7","\xC3\x87"}, {"o","O"}, {"p","P"}, {"q","Q"}, {"r","R"}, {"s","S"}, {"t","T"}, {NULL,NULL} },
        { {"u","U"}, {"v","V"}, {"w","W"}, {"x","X"}, {"y","Y"}, {"z","Z"}, {"-","-"}, {NULL,NULL} }
    },
    {   /* simbolos (espanol) */
        { {"1","1"}, {"2","2"}, {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {K_BKSP,""} },
        { {"8","8"}, {"9","9"}, {"0","0"}, {"&","&"}, {"#","#"}, {"(","("}, {")",")"}, {K_MODE,"ABC"} },
        { {"\xC3\xA1","\xC3\x81"}, {"\xC3\xA9","\xC3\x89"}, {"\xC3\xAD","\xC3\x8D"}, {"\xC3\xB3","\xC3\x93"},
          {"\xC3\xBA","\xC3\x9A"}, {"\xC3\xBC","\xC3\x9C"}, {"\xC2\xBF","\xC2\xBF"}, {NULL,NULL} },
        { {"@","@"}, {".","."}, {",",","}, {":",":"}, {"!","!"}, {"?","?"}, {"'","'"}, {NULL,NULL} }
    },
    {   /* simbolos (ingles) */
        { {"1","1"}, {"2","2"}, {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {K_BKSP,""} },
        { {"8","8"}, {"9","9"}, {"0","0"}, {"&","&"}, {"#","#"}, {"(","("}, {")",")"}, {K_MODE,"ABC"} },
        { {"@","@"}, {".","."}, {",",","}, {":",":"}, {"!","!"}, {"?","?"}, {"'","'"}, {NULL,NULL} },
        { {"+","+"}, {"*","*"}, {"/","/"}, {"$","$"}, {"%","%"}, {"\"","\""}, {"_","_"}, {NULL,NULL} }
    },
    {   /* simbolos (portugues) */
        { {"1","1"}, {"2","2"}, {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {K_BKSP,""} },
        { {"8","8"}, {"9","9"}, {"0","0"}, {"&","&"}, {"#","#"}, {"(","("}, {")",")"}, {K_MODE,"ABC"} },
        { {"\xC3\xA1","\xC3\x81"}, {"\xC3\xA2","\xC3\x82"}, {"\xC3\xA3","\xC3\x83"}, {"\xC3\xA0","\xC3\x80"},
          {"\xC3\xA9","\xC3\x89"}, {"\xC3\xAA","\xC3\x8A"}, {"\xC3\xAD","\xC3\x8D"}, {NULL,NULL} },
        { {"\xC3\xB3","\xC3\x93"}, {"\xC3\xB4","\xC3\x94"}, {"\xC3\xB5","\xC3\x95"}, {"\xC3\xBA","\xC3\x9A"},
          {".","."}, {",",","}, {"?","?"}, {NULL,NULL} }
    },
    {   /* letras arabes (las 28 basicas) */
        { {"\xD8\xA7","\xD8\xA7"}, {"\xD8\xA8","\xD8\xA8"}, {"\xD8\xAA","\xD8\xAA"}, {"\xD8\xAB","\xD8\xAB"}, {"\xD8\xAC","\xD8\xAC"}, {"\xD8\xAD","\xD8\xAD"}, {"\xD8\xAE","\xD8\xAE"}, {K_BKSP,""} },
        { {"\xD8\xAF","\xD8\xAF"}, {"\xD8\xB0","\xD8\xB0"}, {"\xD8\xB1","\xD8\xB1"}, {"\xD8\xB2","\xD8\xB2"}, {"\xD8\xB3","\xD8\xB3"}, {"\xD8\xB4","\xD8\xB4"}, {"\xD8\xB5","\xD8\xB5"}, {K_MODE,"ABC"} },
        { {"\xD8\xB6","\xD8\xB6"}, {"\xD8\xB7","\xD8\xB7"}, {"\xD8\xB8","\xD8\xB8"}, {"\xD8\xB9","\xD8\xB9"}, {"\xD8\xBA","\xD8\xBA"}, {"\xD9\x81","\xD9\x81"}, {"\xD9\x82","\xD9\x82"}, {NULL,NULL} },
        { {"\xD9\x83","\xD9\x83"}, {"\xD9\x84","\xD9\x84"}, {"\xD9\x85","\xD9\x85"}, {"\xD9\x86","\xD9\x86"}, {"\xD9\x87","\xD9\x87"}, {"\xD9\x88","\xD9\x88"}, {"\xD9\x8A","\xD9\x8A"}, {NULL,NULL} }
    },
    {   /* simbolos (arabe: vuelve a las letras arabes) */
        { {"1","1"}, {"2","2"}, {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {K_BKSP,""} },
        { {"8","8"}, {"9","9"}, {"0","0"}, {"&","&"}, {"#","#"}, {"(","("}, {")",")"}, {K_MODE,"\xD8\xA3\xD8\xA8\xD8\xAC"} },
        { {"@","@"}, {".","."}, {"\xD8\x8C","\xD8\x8C"}, {":",":"}, {"!","!"}, {"\xD8\x9F","\xD8\x9F"}, {"'","'"}, {NULL,NULL} },
        { {"+","+"}, {"*","*"}, {"/","/"}, {"$","$"}, {"%","%"}, {"\"","\""}, {"_","_"}, {NULL,NULL} }
    }
};

/* La tabla que toca segun el modo y el idioma */
static int key_table(int mode)
{
    /* arabe: letras arabes -> letras latinas -> simbolos -> letras arabes */
    if (lang_get() == LANG_AR)
        return mode == 0 ? KT_LETTERS_AR : mode == 1 ? KT_LETTERS : KT_SYM_AR;
    if (!mode)
        return lang_get() == LANG_PT ? KT_LETTERS_PT : KT_LETTERS;
    return lang_get() == LANG_PT ? KT_SYM_PT : lang_get() == LANG_ES ? KT_SYM_ES : KT_SYM_EN;
}

/* Cuantos modos tiene el teclado (letras/simbolos; en arabe tambien latinas) */
static int mode_count(void)
{
    return lang_get() == LANG_AR ? 3 : 2;
}

static const char *bottom_label(int i)
{
    return T(i == 0 ? S_KB_SPACE : i == 1 ? S_KB_CLEAR : S_KB_SEARCH);
}

/* Medidas (pantalla de 1280x720) */
#define SUG_W    380.0f
#define SUG_Y    124.0f
#define SUG_H    40.0f
#define SUG_STEP 48.0f
#define KEY_X    560.0f
#define KEY_Y    124.0f
#define KEY_W    68.0f
#define KEY_H    58.0f
#define SPC_X    (KEY_X + 7.0f * KEY_W + 10.0f)
#define SPC_W    130.0f
#define BOT_Y    (KEY_Y + 4.0f * KEY_H + 14.0f)
#define BOT_H    52.0f

enum { AREA_SUGG, AREA_KEYS, AREA_BOTTOM };

static char g_text[TEXT_MAX + 4] = "";
static char g_query[TEXT_MAX + 4] = "";
static int  g_mode = 0;           /* 0 letras, 1 simbolos */
static int  g_area = AREA_KEYS;
static int  g_row = 0, g_col = 0, g_bot = 0, g_sug = 0;

static char g_recent[RECENT_MAX][SUG_LEN];
static int  g_recentCount = 0;

/* Sugerencias: las escribe el hilo y las lee el principal (con candado) */
static CRITICAL_SECTION g_cs;
static HANDLE        g_ev = NULL;
static HANDLE        g_thread = NULL;
static char          g_wantQ[TEXT_MAX + 4];
static char          g_res[SUG_MAX][SUG_LEN];
static int           g_resCount = 0;

/* lo que se ve en la columna de sugerencias en este cuadro */
static char g_view[SUG_MAX][SUG_LEN];
static int  g_viewCount = 0;
static int  g_viewRecent = 0;

/* ------------------------------------------------------------------ */
/* Texto                                                               */

/* Si no es UTF-8 valido (la respuesta vino en Latin-1), lo convierte */
static void to_utf8(const char *src, char *dst, int size)
{
    const unsigned char *s = (const unsigned char *)src;
    int i = 0, ok = 1, n;

    while (*s && ok)
    {
        if (*s < 0x80)
            n = 0;
        else if ((*s & 0xE0) == 0xC0)
            n = 1;
        else if ((*s & 0xF0) == 0xE0)
            n = 2;
        else if ((*s & 0xF8) == 0xF0)
            n = 3;
        else
            break;
        ++s;
        while (n-- > 0)
        {
            if ((*s & 0xC0) != 0x80)
            {
                ok = 0;
                break;
            }
            ++s;
        }
    }
    ok = ok && !*s;
    s = (const unsigned char *)src;
    while (*s && i < size - 3)
    {
        if (ok || *s < 0x80)
            dst[i++] = (char)*s;
        else
        {
            dst[i++] = (char)(0xC0 | (*s >> 6));
            dst[i++] = (char)(0x80 | (*s & 0x3F));
        }
        ++s;
    }
    dst[i] = 0;
}

static void text_changed(void)
{
    if (!g_thread)
        return;
    EnterCriticalSection(&g_cs);
    strcpy(g_wantQ, g_text);
    LeaveCriticalSection(&g_cs);
    SetEvent(g_ev);
}

static void type_str(const char *s)
{
    if (strlen(g_text) + strlen(s) > TEXT_MAX)
        return;
    strcat(g_text, s);
    text_changed();
}

static void backspace(void)
{
    int n = (int)strlen(g_text);

    if (n == 0)
        return;
    --n;
    while (n > 0 && ((unsigned char)g_text[n] & 0xC0) == 0x80)
        --n;   /* letra de varios bytes (n, acentos) */
    g_text[n] = 0;
    text_changed();
}

/* ------------------------------------------------------------------ */
/* Busquedas recientes                                                 */

static void recent_load(void)
{
    char line[SUG_LEN];
    FILE *f = fopen(RECENT_PATH, "rb");
    int n;

    g_recentCount = 0;
    if (!f)
        return;
    while (g_recentCount < RECENT_MAX && fgets(line, sizeof(line), f))
    {
        n = (int)strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (n > 0)
            strcpy(g_recent[g_recentCount++], line);
    }
    fclose(f);
}

static void recent_add(const char *q)
{
    int i, k;
    FILE *f;

    for (i = 0; i < g_recentCount; ++i)
        if (strcmp(g_recent[i], q) == 0)
            break;
    if (i == g_recentCount && g_recentCount < RECENT_MAX)
        ++g_recentCount;
    if (i >= g_recentCount)
        i = g_recentCount - 1;
    for (k = i; k > 0; --k)
        strcpy(g_recent[k], g_recent[k - 1]);
    strncpy(g_recent[0], q, SUG_LEN - 1);
    g_recent[0][SUG_LEN - 1] = 0;

    f = fopen(RECENT_PATH, "wb");
    if (!f)
        return;
    for (i = 0; i < g_recentCount; ++i)
    {
        fputs(g_recent[i], f);
        fputs("\r\n", f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Hilo de sugerencias                                                 */

static void url_encode(const char *src, char *dst, int size)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *s = (const unsigned char *)src;
    int i = 0;

    while (*s && i < size - 4)
    {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
            *s == '-' || *s == '_' || *s == '.' || *s == '~')
            dst[i++] = (char)*s;
        else
        {
            dst[i++] = '%';
            dst[i++] = hex[*s >> 4];
            dst[i++] = hex[*s & 15];
        }
        ++s;
    }
    dst[i] = 0;
}

/* Respuesta: window.google.ac.h(["q",[["sug",0,[..]],...],{...}])
 * (o ["q",["sug",...]] segun el cliente). Devuelve cuantas saco. */
static int parse_suggestions(char *body, int len, char out[][SUG_LEN], int max)
{
    char *a, *b;
    JsonDoc *doc;
    JsonNode *list, *it;
    const char *s;
    int n = 0;

    body[len] = 0;
    a = strchr(body, '[');
    b = strrchr(body, ']');
    if (!a || !b || b < a)
        return 0;
    b[1] = 0;
    doc = json_parse(a);
    if (!doc)
        return 0;
    list = json_at(doc->root, 1);
    for (it = list ? list->child : NULL; it && n < max; it = it->next)
    {
        s = it->type == JSON_STRING ? it->str : json_str(json_at(it, 0));
        if (s && s[0])
            to_utf8(s, out[n++], SUG_LEN);
    }
    json_free(doc);
    return n;
}

static DWORD WINAPI sugg_thread(LPVOID param)
{
    static char res[SUG_MAX][SUG_LEN];
    char q[TEXT_MAX + 4];
    char enc[TEXT_MAX * 3 + 4];
    char url[640];
    char err[160];
    char *body;
    HttpResponse r;
    int n;

    static const char headers[] = "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n";

    (void)param;
    for (;;)
    {
        WaitForSingleObject(g_ev, INFINITE);
        Sleep(200);   /* junta los teclazos seguidos */
        EnterCriticalSection(&g_cs);
        strcpy(q, g_wantQ);
        LeaveCriticalSection(&g_cs);
        if (!q[0])
            continue;
        url_encode(q, enc, sizeof(enc));
        _snprintf(url, sizeof(url) - 1,
                  "https://suggestqueries.google.com/complete/search?client=youtube&ds=yt&hl=%s&gl=%s&oe=utf-8&q=%s",
                  lang_code(), lang_gl(), enc);
        url[sizeof(url) - 1] = 0;
        if (!http_request("GET", url, headers, NULL, 0, &r, err, sizeof(err)))
            continue;
        n = 0;
        if (r.status == 200 && r.bodyLen > 0)
        {
            body = (char *)malloc(r.bodyLen + 1);
            if (body)
            {
                memcpy(body, r.body, r.bodyLen);
                n = parse_suggestions(body, r.bodyLen, res, SUG_MAX);
                free(body);
            }
        }
        http_free(&r);
        EnterCriticalSection(&g_cs);
        memcpy(g_res, res, sizeof(g_res));
        g_resCount = n;
        LeaveCriticalSection(&g_cs);
    }
    return 0;   /* no se llega: el hilo vive mientras corre la app */
}

void search_init(void)
{
    recent_load();
    InitializeCriticalSection(&g_cs);
    g_ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (g_ev)
        g_thread = CreateThread(NULL, 64 * 1024, sugg_thread, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread)
    {
        XSetThreadProcessor(g_thread, 4);   /* junto a las miniaturas */
        ResumeThread(g_thread);
    }
}

/* ------------------------------------------------------------------ */
/* Lo que muestra la columna de sugerencias                            */

static void update_view(void)
{
    int i;

    if (!g_text[0])
    {
        g_viewRecent = 1;
        g_viewCount = g_recentCount < SUG_MAX ? g_recentCount : SUG_MAX;
        for (i = 0; i < g_viewCount; ++i)
            strcpy(g_view[i], g_recent[i]);
    }
    else
    {
        g_viewRecent = 0;
        if (g_thread)
        {
            EnterCriticalSection(&g_cs);
            g_viewCount = g_resCount;
            memcpy(g_view, g_res, sizeof(g_view));
            LeaveCriticalSection(&g_cs);
        }
        else
            g_viewCount = 0;
    }
    if (g_sug >= g_viewCount)
        g_sug = g_viewCount > 0 ? g_viewCount - 1 : 0;
    if (g_area == AREA_SUGG && g_viewCount == 0)
        g_area = AREA_KEYS;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */

void search_open(void)
{
    g_area = AREA_KEYS;
    g_row = g_col = 0;
    g_bot = 0;
    g_sug = 0;
    update_view();
}

const char *search_text(void)
{
    return g_text;
}

const char *search_query(void)
{
    return g_query;
}

static int row_cols(int row)
{
    return row < 2 ? 8 : 7;
}

static int submit(const char *q)
{
    int i = 0;

    while (q[i] == ' ')
        ++i;
    if (!q[i])
        return SEARCH_NONE;
    strncpy(g_query, q + i, TEXT_MAX);
    g_query[TEXT_MAX] = 0;
    recent_add(g_query);
    sfx_play(SFX_CLICK);    /* Buscar o una sugerencia: clic de mouse */
    return SEARCH_SUBMIT;
}

/* La tecla de abajo que queda debajo de la columna col, y al reves */
static int bottom_of_col(int col)
{
    return col < 3 ? 0 : col < 6 ? 1 : 2;
}

static int col_of_bottom(int b)
{
    return b == 0 ? 1 : b == 1 ? 4 : 6;
}

static int press_key(void)
{
    const Key *k;

    if (g_area == AREA_BOTTOM)
    {
        if (g_bot == 0)
            type_str(" ");
        else if (g_bot == 1)
        {
            g_text[0] = 0;
            text_changed();
        }
        else
            return submit(g_text);
        sfx_play(SFX_KEY);
        return SEARCH_NONE;
    }
    if (g_area == AREA_SUGG)
        return g_sug < g_viewCount ? submit(g_view[g_sug]) : SEARCH_NONE;

    k = &kKeys[key_table(g_mode)][g_row][g_col];
    if (!k->ins)
        return SEARCH_NONE;
    sfx_play(SFX_KEY);      /* cada tecla: sonido de teclado mecanico */
    if (strcmp(k->ins, K_BKSP) == 0)
        backspace();
    else if (strcmp(k->ins, K_MODE) == 0)
        g_mode = (g_mode + 1) % mode_count();
    else
        type_str(k->ins);
    return SEARCH_NONE;
}

static int move_input(unsigned short pressed, int stickDir);

/* Cualquier cambio de la tecla o sugerencia elegida suena como "mover" */
int search_input(unsigned short pressed, int stickDir)
{
    int a, r, c, b, sg, res;

    update_view();      /* que las sugerencias que llegaron no cuenten como movimiento */
    a = g_area; r = g_row; c = g_col; b = g_bot; sg = g_sug;
    res = move_input(pressed, stickDir);

    if (res == SEARCH_NONE && (a != g_area || r != g_row || c != g_col || b != g_bot || sg != g_sug))
        sfx_play(SFX_MOVE);
    return res;
}

static int move_input(unsigned short pressed, int stickDir)
{
    int left = (pressed & XINPUT_GAMEPAD_DPAD_LEFT) || stickDir < 0;
    int right = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || stickDir > 0;
    int up = (pressed & XINPUT_GAMEPAD_DPAD_UP) != 0;
    int down = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) != 0;

    update_view();

    /* atajos */
    if (pressed & XINPUT_GAMEPAD_B)
        return SEARCH_BACK;
    if (pressed & XINPUT_GAMEPAD_BACK)
        return SEARCH_MENU;     /* Back: menu lateral */
    if (pressed & XINPUT_GAMEPAD_X)
    {
        backspace();
        sfx_play(SFX_KEY);
    }
    if (pressed & XINPUT_GAMEPAD_Y)
    {
        type_str(" ");
        sfx_play(SFX_KEY);
    }
    if (pressed & XINPUT_GAMEPAD_START)
        return submit(g_text);
    if (pressed & XINPUT_GAMEPAD_A)
        return press_key();

    switch (g_area)
    {
    case AREA_SUGG:
        if (up && g_sug > 0)
            --g_sug;
        if (down && g_sug < g_viewCount - 1)
            ++g_sug;
        if (right)
        {
            g_area = AREA_KEYS;
            g_row = (int)((SUG_Y + g_sug * SUG_STEP - KEY_Y) / KEY_H + 0.5f);
            if (g_row > 3)
            {
                g_area = AREA_BOTTOM;
                g_bot = 0;
            }
            if (g_row < 0)
                g_row = 0;
            g_col = 0;
        }
        break;

    case AREA_KEYS:
        if (up && g_row > 0)
            --g_row;
        if (down)
        {
            if (g_row < 3)
                ++g_row;
            else
            {
                g_area = AREA_BOTTOM;
                g_bot = bottom_of_col(g_col);
                break;
            }
        }
        if (g_col >= row_cols(g_row))
            g_col = row_cols(g_row) - 1;
        if (right && g_col < row_cols(g_row) - 1)
            ++g_col;
        if (left)
        {
            if (g_col > 0)
                --g_col;
            else if (g_viewCount > 0)
            {
                g_area = AREA_SUGG;
                g_sug = g_row * 2 < g_viewCount ? g_row * 2 : g_viewCount - 1;
            }
        }
        break;

    case AREA_BOTTOM:
        if (up)
        {
            g_area = AREA_KEYS;
            g_row = 3;
            g_col = col_of_bottom(g_bot);
            break;
        }
        if (right && g_bot < 2)
            ++g_bot;
        if (left)
        {
            if (g_bot > 0)
                --g_bot;
            else if (g_viewCount > 0)
            {
                g_area = AREA_SUGG;
                g_sug = g_viewCount - 1;
            }
        }
        break;
    }
    return SEARCH_NONE;
}

/* ------------------------------------------------------------------ */
/* Dibujo                                                              */

static void draw_label(float x, float y, float w, float h, float scale, unsigned int color, const char *s)
{
    gfx_text(x + (w - gfx_text_width(scale, s)) * 0.5f, y + (h - 24.0f * scale) * 0.5f, scale, color, s);
}

void search_draw(float x0)
{
    char line[SUG_LEN];
    int r, c, on, i;
    float x, y, w;
    const Key *k;

    update_view();

    /* sugerencias (o recientes) */
    if (g_viewCount == 0)
        gfx_text(x0, SUG_Y + 8.0f, 0.95f, COL_DIM,
                 g_text[0] ? T(S_NO_SUGG) : T(S_TYPE_TO_SEARCH));
    for (i = 0; i < g_viewCount; ++i)
    {
        on = g_area == AREA_SUGG && i == g_sug;
        y = SUG_Y + i * SUG_STEP;
        gfx_fit_text(g_view[i], SUG_W - 62.0f, 0.95f, line, sizeof(line));
        w = gfx_text_width(0.95f, line) + 62.0f;
        ui_round_rect(x0, y, w, SUG_H, 10.0f, on ? COL_ACCENT : COL_CHIP);
        gfx_icon(g_viewRecent ? UI_ICON_M_HISTORY : UI_ICON_M_SEARCH, x0 + 12.0f, y + 9.0f, 22.0f,
                 on ? COL_WHITE : COL_SOFT);
        gfx_text(x0 + 44.0f, y + 9.0f, 0.95f, on ? COL_WHITE : COL_TEXT, line);
    }

    /* teclas */
    for (r = 0; r < 4; ++r)
        for (c = 0; c < row_cols(r); ++c)
        {
            k = &kKeys[key_table(g_mode)][r][c];
            on = g_area == AREA_KEYS && r == g_row && c == g_col;
            y = KEY_Y + r * KEY_H;
            if (c == 7)
            {
                x = SPC_X;
                w = SPC_W;
                ui_round_rect(x + 3.0f, y + 4.0f, w - 6.0f, KEY_H - 8.0f, 10.0f, on ? COL_ACCENT : COL_CHIP);
                if (strcmp(k->ins, K_BKSP) == 0)
                    gfx_icon(UI_ICON_K_BACKSPACE, x + (w - 32.0f) * 0.5f, y + (KEY_H - 32.0f) * 0.5f, 32.0f,
                             on ? COL_WHITE : COL_TEXT);
                else
                    draw_label(x, y, w, KEY_H, 1.0f, on ? COL_WHITE : COL_TEXT, k->label);
                continue;
            }
            x = KEY_X + c * KEY_W;
            if (on)
                ui_round_rect(x + 3.0f, y + 4.0f, KEY_W - 6.0f, KEY_H - 8.0f, 10.0f, COL_ACCENT);
            draw_label(x, y, KEY_W, KEY_H, 1.25f, on ? COL_WHITE : COL_TEXT, k->label);
        }

    /* ESPACIO, BORRAR, BUSCAR */
    x = KEY_X;
    for (i = 0; i < 3; ++i)
    {
        w = i < 2 ? 196.0f : SPC_X + SPC_W - KEY_X - 2.0f * 208.0f;
        on = g_area == AREA_BOTTOM && i == g_bot;
        ui_round_rect(x + 3.0f, BOT_Y, w - 6.0f, BOT_H, 10.0f, on ? COL_ACCENT : COL_CHIP);
        draw_label(x, BOT_Y, w, BOT_H, 1.0f, on ? COL_WHITE : COL_TEXT, bottom_label(i));
        x += 208.0f;
    }
}
