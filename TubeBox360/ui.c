/* ui.c - Tema, ayudas de botones, perfil y menu lateral. C89.
 * Ver ui.h. */
#include <xtl.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "ui.h"
#include "presence.h"
#include "cuenta.h"
#include "thumb.h"
#include "version.h"
#include "gfx.h"
#include "ui_assets.h"
#include "lang.h"

/* ------------------------------------------------------------------ */
/* Tema: el oscuro es el gris del YouTube TV clasico                   */

static const UiTheme kDark  = { 0xFF242424, 0xFF1A1A1A, 0xFF3A3A3A, 0xFFFFFFFF, 0xFFD0D0D0, 0xFF9A9A9A,
                                0xF52C2C2C, 0xFF1A1A1A, 0xFF333333, 0xFF3A3A3A, 0xFF1A1A1A, 0xFF383838 };
static const UiTheme kLight = { 0xFFF1F1F1, 0xFFFFFFFF, 0xFFDCDCDC, 0xFF212121, 0xFF333333, 0xFF767676,
                                0xF8FFFFFF, 0xFFE0E0E0, 0xFFDADADA, 0xFFE2E2E2, 0xFFFFFFFF, 0xFFE4E4E4 };

const UiTheme *g_th = &kDark;
int            g_lightTheme = 0;

void ui_set_theme(int light)
{
    g_lightTheme = light ? 1 : 0;
    g_th = g_lightTheme ? &kLight : &kDark;
}

/* ------------------------------------------------------------------ */
/* Ayudas de botones                                                   */

#define HINT_ICON  28.0f
#define HINT_SCALE 0.95f
#define HINT_GAP   26.0f

static int button_icon(const char *name, int len)
{
    static const struct { const char *n; int icon; } k[] = {
        { "A", UI_ICON_PAD_A }, { "B", UI_ICON_PAD_B }, { "X", UI_ICON_PAD_X }, { "Y", UI_ICON_PAD_Y },
        { "LB", UI_ICON_PAD_LB }, { "RB", UI_ICON_PAD_RB }, { "START", UI_ICON_PAD_START },
        { "BACK", UI_ICON_PAD_BACK }, { "DPAD", UI_ICON_PAD_DPAD }, { "LR", UI_ICON_PAD_LR },
        { "UD", UI_ICON_PAD_UD }, { "RIGHT", UI_ICON_PAD_RIGHT }, { "LT", UI_ICON_PAD_LT },
        { "RT", UI_ICON_PAD_RT }, { "LS", UI_ICON_PAD_LS }, { "RS", UI_ICON_PAD_RS },
        { "DOWN", UI_ICON_PAD_DOWN }
    };
    int i;

    for (i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i)
        if ((int)strlen(k[i].n) == len && strncmp(k[i].n, name, len) == 0)
            return k[i].icon;
    return -1;
}

/* Recorre la especificacion; si draw es 0 solo mide */
static float hints_run(float x, float y, const char *spec, unsigned int color, int draw)
{
    const char *p = spec, *eq, *end, *k, *kend;
    char label[96];
    float x0 = x;
    int first = 1, icon, n;

    while (*p)
    {
        end = strchr(p, '|');
        if (!end)
            end = p + strlen(p);
        eq = p;
        while (eq < end && *eq != '=')
            ++eq;
        if (!first)
            x += HINT_GAP;
        first = 0;

        /* botones (A, LB+RB, ...) */
        for (k = p; k < eq; k = kend + 1)
        {
            kend = k;
            while (kend < eq && *kend != '+')
                ++kend;
            icon = button_icon(k, (int)(kend - k));
            if (icon >= 0)
            {
                if (draw && !gfx_icon(icon, x, y - 2.0f, HINT_ICON, 0xFFFFFFFF))
                    gfx_text(x, y, HINT_SCALE, color, "?");
                x += HINT_ICON + 2.0f;
            }
            if (kend >= eq)
                break;
        }
        /* texto */
        if (eq < end)
        {
            n = (int)(end - eq - 1);
            if (n > (int)sizeof(label) - 1)
                n = (int)sizeof(label) - 1;
            memcpy(label, eq + 1, n);
            label[n] = 0;
            x += 6.0f;
            if (draw)
                gfx_text(x, y, HINT_SCALE, color, label);
            x += gfx_text_width(HINT_SCALE, label);
        }
        p = *end ? end + 1 : end;
    }
    return x - x0;
}

float ui_hints_width(const char *spec)
{
    return hints_run(0.0f, 0.0f, spec, 0, 0);
}

float ui_hints(float x, float y, const char *spec, unsigned int textColor)
{
    return x + hints_run(x, y, spec, textColor, 1);
}

void ui_footer(float x, const char *spec)
{
    ui_hints(x, 652.0f, spec, COL_DIM);
}

/* ------------------------------------------------------------------ */
/* Formas                                                              */

void ui_round_rect(float x, float y, float w, float h, float r, unsigned int color)
{
    if (r * 2.0f > h)
        r = h * 0.5f;
    if (r * 2.0f > w)
        r = w * 0.5f;
    if (r < 1.0f)
    {
        gfx_rect(x, y, w, h, color);
        return;
    }
    gfx_rect(x + r, y, w - 2.0f * r, h, color);
    gfx_rect(x, y + r, r, h - 2.0f * r, color);
    gfx_rect(x + w - r, y + r, r, h - 2.0f * r, color);
    gfx_circle(x, y, 2.0f * r, color);
    gfx_circle(x + w - 2.0f * r, y, 2.0f * r, color);
    gfx_circle(x, y + h - 2.0f * r, 2.0f * r, color);
    gfx_circle(x + w - 2.0f * r, y + h - 2.0f * r, 2.0f * r, color);
}

void ui_red_gloss(float x, float y, float w, float h)
{
    float g = h > 120.0f ? 120.0f : h * 0.5f;   /* reflejo arriba */

    gfx_rect_vgrad(x, y, w, h, 0xFFF0392E, 0xFFB8150D);
    gfx_rect_vgrad(x, y, w, g, 0x38FFFFFF, 0x08FFFFFF);
}

void ui_line(float x0, float y0, float x1, float y1, float w, unsigned int color)
{
    float dx = x1 - x0, dy = y1 - y0, len, t, step;
    int i, n;

    len = (float)sqrt(dx * dx + dy * dy);
    if (len < 1.0f)
        return;
    step = w * 0.35f;
    n = (int)(len / step) + 1;
    for (i = 0; i <= n; ++i)
    {
        t = (float)i / n;
        gfx_rect(x0 + dx * t - w * 0.5f, y0 + dy * t - w * 0.5f, w, w, color);
    }
}

/* ------------------------------------------------------------------ */
/* Perfil: gamertag y foto de jugador                                  */

#define PIC_SIZE 64     /* la foto grande de la Xbox 360 es de 64x64 */

static int          g_picUser = -1;      /* indice del control con sesion, -1 ninguno */
static XUID         g_picXuid = 0;
static void        *g_picTex = NULL;
static char         g_gamertag[32] = "";
static DWORD        g_lastCheck = 0;
static int          g_checked = 0;
static int          g_picTries = 0;      /* reintentos si la foto no se pudo leer */
static unsigned int g_picPx[PIC_SIZE * PIC_SIZE];

static void load_profile(int user)
{
    int i;

    if (g_picTex)
    {
        gfx_texture_free(g_picTex);
        g_picTex = NULL;
    }
    g_gamertag[0] = 0;
    g_picUser = user;
    if (user < 0)
        return;
    if (XUserGetName(user, g_gamertag, sizeof(g_gamertag)) != ERROR_SUCCESS)
        g_gamertag[0] = 0;
    g_gamertag[sizeof(g_gamertag) - 1] = 0;
    /* sin OVERLAPPED: la lee de una vez (sale del perfil guardado) */
    if (XUserReadGamerPicture(user, FALSE, (PBYTE)g_picPx, PIC_SIZE * 4, PIC_SIZE, NULL) == ERROR_SUCCESS)
    {
        for (i = 0; i < PIC_SIZE * PIC_SIZE; ++i)
            g_picPx[i] |= 0xFF000000;   /* por si viene sin alfa */
        g_picTex = gfx_texture_from_argb(g_picPx, PIC_SIZE, PIC_SIZE);
    }
}

void ui_profile_poll(void)
{
    DWORD now = GetTickCount();
    int i, user = -1;
    XUID xuid = 0;

    if (g_checked && now - g_lastCheck < 2000)
        return;
    g_checked = 1;
    g_lastCheck = now;
    for (i = 0; i < 4 && user < 0; ++i)
        if (XUserGetSigninState(i) != eXUserSigninState_NotSignedIn)
            user = i;
    if (user >= 0 && XUserGetXUID(user, &xuid) != ERROR_SUCCESS)
        xuid = 0;
    if (user != g_picUser || xuid != g_picXuid)
    {
        g_picXuid = xuid;
        g_picTries = 0;
        load_profile(user);
    }
    else if (user >= 0 && !g_picTex && g_picTries < 5)
    {
        ++g_picTries;   /* recien iniciada la sesion la foto puede no estar lista */
        load_profile(user);
    }
}

void ui_profile_draw(float x, float y, float size)
{
    if (g_picTex)
    {
        gfx_image_circle(g_picTex, x, y, size, 0xFFFFFFFF);
        return;
    }
    gfx_circle(x, y, size, g_lightTheme ? 0xFFBDBDBD : 0xFF4A4A4A);
    gfx_icon(UI_ICON_M_USER, x + size * 0.18f, y + size * 0.16f, size * 0.64f, 0xFFFFFFFF);
}

void *ui_profile_texture(void)
{
    return g_picTex;
}

const char *ui_profile_name(void)
{
    return g_gamertag;
}

void ui_profile_signin(void)
{
    XShowSigninUI(1, 0);
    g_checked = 0;   /* revisar pronto */
}

/* ------------------------------------------------------------------ */
/* Menu lateral                                                        */

static const struct { int icon; int name; } kSections[UI_SEC_COUNT] = {
    { UI_ICON_M_USER,     S_PICK_PROFILE },
    { UI_ICON_M_SEARCH,   S_SEC_SEARCH },
    { UI_ICON_M_HOME,     S_SEC_HOME },
    { UI_ICON_M_SUBS,     S_SEC_SUBS },
    { UI_ICON_M_STAR,     S_SEC_COMMUNITY },
    { UI_ICON_M_GAMING,   S_SEC_GAMING },
    { UI_ICON_M_MUSIC,    S_SEC_MUSIC },
    { UI_ICON_M_NEWS,     S_SEC_NEWS },
    { UI_ICON_M_SPORTS,   S_SEC_SPORTS },
    { UI_ICON_M_HISTORY,  S_SEC_HISTORY },
    { UI_ICON_M_SETTINGS, S_SEC_SETTINGS }
};

const char *ui_section_name(int sec)
{
    if (sec == UI_SEC_PROFILE && g_gamertag[0])
        return g_gamertag;
    return (sec >= 0 && sec < UI_SEC_COUNT) ? T(kSections[sec].name) : "";
}

/* Centro vertical de cada renglon del menu */
static float sec_y(int sec)
{
    if (sec == UI_SEC_PROFILE)
        return 62.0f;
    if (sec == UI_SEC_SETTINGS)
        return 614.0f;      /* aparte, debajo de una raya (abajo va la cuenta de Google) */
    return 144.0f + (sec - UI_SEC_SEARCH) * 48.0f;   /* 9 renglones: Historial en 528 */
}

/* raya entre Historial y Configuracion */
#define SEP_Y 570.0f

/* Debajo de Ajustes: punto verde y "Usuarios activos: 00" (solo con
 * internet y la base de Firebase configurada). Plegado: punto y numero. */
/* "Usuarios activos" debajo del panel del avatar (menu desplegado), sin
 * recuadro y en negritas:
 *   [logo] Usuarios activos: 01
 *   (verde) Tu sesion esta activa      | sin conexion: (rojo) ... no esta activa
 * centrado en cx a partir de y. Devuelve la altura usada (0 = nada, cuando
 * no hay base de Firebase en version.h). */
float ui_draw_active_users(float cx, float y)
{
    char buf[80];
    int n = presence_count();
    int on = n >= 0;
    const char *ses = T(on ? S_SESSION_ACTIVE : S_SESSION_OFF);
    float w1, w2, x, logo = 30.0f, s1 = 0.85f, s2 = 0.85f, dot = 12.0f;

    if (!TB_PRESENCE_URL[0])
        return 0.0f;
    if (n > 9999)
        n = 9999;   /* el numero completo ("%02d": 01..99, luego 100, 128...) */
    if (on)
        _snprintf(buf, sizeof(buf) - 1, T(S_ACTIVE_USERS), n);
    else
        _snprintf(buf, sizeof(buf) - 1, "%s", T(S_ACTIVE_USERS_NA));
    buf[sizeof(buf) - 1] = 0;

    gfx_font_bold(1);
    w1 = logo + 10.0f + gfx_text_width(s1, buf);
    w2 = dot + 10.0f + gfx_text_width(s2, ses);

    /* renglon 1: logo + numero (logo centrado con el texto) */
    x = cx - w1 * 0.5f;
    gfx_icon(UI_ICON_XBOX, x, y + (GFX_CHAR_H * s1 - logo) * 0.5f, logo, 0xFFFFFFFF);
    gfx_text(x + logo + 10.0f, y, s1, 0xFFFFFFFF, buf);

    /* renglon 2: puntito + sesion (el punto a media altura del texto) */
    y += 38.0f;
    x = cx - w2 * 0.5f;
    gfx_circle(x, y + (GFX_CHAR_H * s2 - dot) * 0.5f, dot, on ? 0xFF3DDC5A : 0xFFE03030);
    gfx_text(x + dot + 10.0f, y, s2, 0xFFFFFFFF, ses);

    gfx_font_bold(0);
    return 72.0f;
}

void ui_draw_rail(int current, int focus)
{
    int i;
    float y, h;
    unsigned int col;
    char name[64];

    if (focus < 0)
    {
        /* plegado: solo iconos; la seccion que se ve, en rojo */
        gfx_rect(0.0f, 0.0f, UI_RAIL_W, 720.0f, g_th->rail);
        ui_profile_draw(UI_RAIL_CX - 24.0f, sec_y(UI_SEC_PROFILE) - 24.0f, 48.0f);
        for (i = UI_SEC_SEARCH; i < UI_SEC_COUNT; ++i)
        {
            y = sec_y(i);
            col = i == current ? COL_ACCENT : (g_lightTheme ? 0xFF505050 : 0xFFE0E0E0);
            if (i == current)
                gfx_rect(UI_RAIL_CX - 38.0f, y - 18.0f, 4.0f, 36.0f, COL_ACCENT);
            gfx_icon(kSections[i].icon, UI_RAIL_CX - 18.0f, y - 18.0f, 36.0f, col);
        }
        gfx_rect(UI_RAIL_CX - 26.0f, SEP_Y, 52.0f, 2.0f, g_lightTheme ? 0x30000000 : 0x40FFFFFF);
        return;
    }

    /* desplegado: el contenido se oscurece y el menu sale en el rojo con
     * brillo del menu de calidades; la elegida en un rectangulo blanco */
    gfx_rect(0.0f, 0.0f, 1280.0f, 720.0f, 0x99000000);
    ui_red_gloss(0.0f, 0.0f, UI_GUIDE_W, 720.0f);
    gfx_rect_vgrad(UI_GUIDE_W, 0.0f, 10.0f, 720.0f, 0x60000000, 0x60000000);

    for (i = 0; i < UI_SEC_COUNT; ++i)
    {
        y = sec_y(i);
        h = i == UI_SEC_PROFILE ? 68.0f : 46.0f;
        col = 0xFFFFFFFF;
        if (i == focus)
        {
            gfx_rect(0.0f, y - h * 0.5f, UI_GUIDE_W, h, 0xFFFFFFFF);
            col = COL_GUIDE;
        }
        else if (i == current)
            gfx_rect(UI_RAIL_CX - 38.0f, y - 14.0f, 4.0f, 28.0f, 0xFFFFFFFF);

        if (i == UI_SEC_PROFILE)
        {
            ui_profile_draw(UI_RAIL_CX - 24.0f, y - 24.0f, 48.0f);
            gfx_fit_text(ui_section_name(i), UI_GUIDE_W - 150.0f, 1.0f, name, sizeof(name));
            gfx_text(UI_RAIL_CX + 46.0f, y - 12.0f, 1.0f, col, name);
            continue;
        }
        gfx_icon(kSections[i].icon, UI_RAIL_CX - 18.0f, y - 18.0f, 36.0f, col);
        gfx_text(UI_RAIL_CX + 46.0f, y - 12.0f, 1.0f, col, T(kSections[i].name));
    }
    gfx_rect(UI_RAIL_CX - 30.0f, SEP_Y, UI_GUIDE_W - UI_RAIL_CX - 10.0f, 2.0f, 0x50FFFFFF);

    /* debajo de Configuracion: foto y @usuario de Google (gris sin sesion) */
    {
        const char *who;
        char fit[128];
        float pic = 34.0f, ry = sec_y(UI_SEC_SETTINGS) + 50.0f;
        int in = cuenta_signed_in();

        if (in)
            who = cuenta_handle()[0] ? cuenta_handle() : cuenta_name();
        else
            who = T(S_G_NOT_SIGNED);
        gfx_font_bold(1);
        gfx_fit_text(who, UI_GUIDE_W - UI_RAIL_CX - 46.0f - 16.0f, 0.8f, fit, sizeof(fit));
        if (in)
            thumb_draw_avatar(cuenta_avatar(), UI_RAIL_CX - pic * 0.5f, ry - pic * 0.5f, pic, cuenta_name());
        else
        {
            gfx_circle(UI_RAIL_CX - pic * 0.5f, ry - pic * 0.5f, pic, 0x55000000);
            gfx_icon(UI_ICON_M_USER, UI_RAIL_CX - pic * 0.5f + 7.0f, ry - pic * 0.5f + 7.0f, pic - 14.0f, 0xFFBDBDBD);
        }
        gfx_text(UI_RAIL_CX + 46.0f, ry - GFX_CHAR_H * 0.8f * 0.5f, 0.8f, in ? 0xFFFFFFFF : 0xFFE0C0C0, fit);
        gfx_font_bold(0);
    }
}

/* Ruedita de carga estilo Xbox 360: dos arcos (uno por fuera y otro un
 * poco mas adentro, del lado contrario) que giran juntos; cada uno tiene la
 * punta brillante y la cola que se desvanece, y se ven como una espiral.
 * cx, cy = centro; rad = radio del arco de afuera. */
void ui_draw_spinner(float cx, float cy, float rad)
{
    DWORD t = GetTickCount();
    float base = (float)(t % 1100) * (6.2831853f / 1100.0f);            /* una vuelta en 1.1 s */
    float len = 4.0f + 0.4f * sinf((float)(t % 2400) * (6.2831853f / 2400.0f)); /* ~205-250 grados: se enciman (espiral) */
    float thick = rad * 0.13f, r2 = rad * 0.80f, a1, a2;
    unsigned int head = g_lightTheme ? 0xFF505050 : 0xFFFFFFFF;
    unsigned int tail = head & 0x00FFFFFF;

    if (thick < 2.5f)
        thick = 2.5f;
    /* arco de afuera: la cola en base, la punta en base + len */
    a1 = base + len;
    gfx_arc(cx, cy, rad, thick, base, a1, tail, head);
    gfx_circle(cx + cosf(a1) * rad - thick * 0.5f, cy + sinf(a1) * rad - thick * 0.5f, thick, head);
    /* arco de adentro: media vuelta despues */
    a2 = base + 3.14159265f + len;
    gfx_arc(cx, cy, r2, thick, base + 3.14159265f, a2, tail, head);
    gfx_circle(cx + cosf(a2) * r2 - thick * 0.5f, cy + sinf(a2) * r2 - thick * 0.5f, thick, head);
}

void ui_draw_sub_pill(float x, float y, int subscribed, int focused, int enabled)
{
    /* rectangulo recto (sin puntas redondas): en la tele las esquinas
     * redondas se veian con circulos encimados */
    float w = UI_SUB_W, h = UI_SUB_H, s = 0.85f, tw;
    unsigned int a = enabled ? 0xFF000000 : 0x80000000;
    const char *label = T(subscribed ? S_SUBSCRIBED : S_SUBSCRIBE);
    char fit[64];

    if (focused)
    {
        /* burbuja roja alrededor, con la misma forma */
        gfx_rect(x - 6.0f, y - 6.0f, w + 12.0f, h + 12.0f, 0xFFE62117);
        gfx_rect_vgrad(x - 6.0f, y - 6.0f, w + 12.0f, (h + 12.0f) * 0.45f, 0x50FFFFFF, 0x00FFFFFF);
    }
    if (subscribed)
    {
        gfx_rect(x, y, w, h, a | 0x00F2F2F2);
        gfx_rect_vgrad(x, y, w, h * 0.48f, enabled ? 0x80FFFFFF : 0x40FFFFFF, 0x00FFFFFF);
    }
    else
    {
        gfx_rect(x, y, w, h, a | 0x00C8140C);
        gfx_rect_vgrad(x, y, w, h * 0.50f, enabled ? 0x70FFFFFF : 0x30FFFFFF, 0x08FFFFFF);
        gfx_rect_vgrad(x, y + h * 0.60f, w, h * 0.40f, 0x00000000, enabled ? 0x30000000 : 0x18000000);
    }
    gfx_font_bold(1);
    gfx_fit_text(label, w - 24.0f, s, fit, sizeof(fit));
    tw = gfx_text_width(s, fit);
    gfx_text(x + (w - tw) * 0.5f, y + (h - GFX_CHAR_H * s) * 0.5f - 1.0f, s,
             subscribed ? (enabled ? 0xFF202020 : 0x80202020) : (enabled ? 0xFFFFFFFF : 0x90FFFFFF), fit);
    gfx_font_bold(0);
}
