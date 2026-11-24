/* avatar.c - El avatar del jugador junto al menu lateral (ver avatar.h). C89.
 *
 * Estado: hacia donde mira la cabeza (yaw/pitch, sigue a la palanca derecha
 * con suavidad y vuelve al centro al soltarla) y el giro del cuerpo (los
 * gatillos lo giran; si se sueltan, a los 1.5 s vuelve de frente).
 *
 * Dibujo 2.5D mientras entra el avatar 3D (XAvatar): un busto con la foto
 * de jugador como cara. El giro angosta el cuerpo y la cabeza con el coseno
 * del angulo y, de espaldas, se ve la nuca. */
#include <xtl.h>
#include <stdio.h>
#include <math.h>
#include "gfx.h"
#include "ui.h"
#include "ui_assets.h"
#include "lang.h"
#include "avatar.h"
#include "avatar3d.h"

#define PI_F        3.14159265f
#define TURN_SPEED  3.2f      /* radianes por segundo con el gatillo a fondo */
#define BACK_DELAY  1500      /* ms sin gatillos para volver de frente */

static float g_yaw = 0.0f, g_pitch = 0.0f;      /* -1..1 */
static float g_turn = 0.0f;                     /* radianes */
static unsigned long g_idle = 0;                /* ms sin tocar los gatillos */
static float g_dt = 0.016f;                     /* segundos de la ultima vuelta (para animar el 3D) */
static DWORD g_noItemUntil = 0;                 /* aviso "no hay elementos interactivos" hasta */

void avatar_gesture(void)
{
    if (av3d_gesture() < 0)
        g_noItemUntil = GetTickCount() + 4000;   /* se queda de pie y sale el aviso */
    else
        g_noItemUntil = 0;
}

void avatar_reset(void)
{
    av3d_wave();     /* al abrir el menu: saluda */
    g_yaw = g_pitch = 0.0f;
    g_turn = 0.0f;
    g_idle = 0;
}

static float approach(float v, float target, float k)
{
    return v + (target - v) * (k > 1.0f ? 1.0f : k);
}

void avatar_input(float rx, float ry, float lt, float rt, unsigned long ms)
{
    float dt = ms / 1000.0f, d;

    if (dt > 0.1f)
        dt = 0.1f;
    g_dt = dt;
    g_yaw = approach(g_yaw, rx, dt * 9.0f);
    g_pitch = approach(g_pitch, ry, dt * 9.0f);

    d = rt - lt;
    if (d > 0.05f || d < -0.05f)
    {
        g_turn += d * TURN_SPEED * dt;
        g_idle = 0;
    }
    else
    {
        g_idle += ms;
        if (g_idle > BACK_DELAY)
        {
            /* de vuelta de frente por el lado mas corto */
            while (g_turn > PI_F)
                g_turn -= 2.0f * PI_F;
            while (g_turn < -PI_F)
                g_turn += 2.0f * PI_F;
            g_turn = approach(g_turn, 0.0f, dt * 4.0f);
        }
    }
    while (g_turn > 2.0f * PI_F)
        g_turn -= 2.0f * PI_F;
    while (g_turn < -2.0f * PI_F)
        g_turn += 2.0f * PI_F;
}

static void draw_help(float cx, float y)
{
    char spec[160];
    float tw, hw, sw = 0.0f, w, h = 82.0f, x;
    const char *title = !ui_profile_name()[0] ? T(S_AV_SIGNIN) : av3d_loading() ? T(S_AV_LOADING) : T(S_AV_HELP);
    /* mientras no hay 3D, el estado de XAvatar en chico (para probar sin depurador) */
    const char *st = (!av3d_ready() && ui_profile_name()[0]) ? av3d_status() : "";

    _snprintf(spec, sizeof(spec) - 1, "RS=%s|LT+RT=%s|X=%s", T(H_AV_HEAD), T(H_AV_TURN), T(H_AV_GESTURE));
    spec[sizeof(spec) - 1] = 0;
    gfx_font_bold(1);
    tw = gfx_text_width(0.9f, title);
    gfx_font_bold(0);
    hw = ui_hints_width(spec);
    if (st[0])
    {
        sw = gfx_text_width(0.7f, st);
        h += 24.0f;
    }
    w = (tw > hw ? tw : hw);
    if (sw > w)
        w = sw;
    w += 44.0f;
    x = cx - w * 0.5f;
    ui_round_rect(x, y, w, h, 12.0f, 0xFF1C1C1C);
    gfx_rect(x + 12.0f, y, w - 24.0f, 2.0f, COL_GUIDE);
    gfx_font_bold(1);
    gfx_text(cx - tw * 0.5f, y + 12.0f, 0.9f, COL_WHITE, title);
    gfx_font_bold(0);
    ui_hints(cx - hw * 0.5f, y + 44.0f, spec, 0xFFCFCFCF);
    if (st[0])
        gfx_text(cx - sw * 0.5f, y + 82.0f, 0.7f, 0xFF9A9A9A, st);

    /* debajo del panel: usuarios activos (y el aviso del gesto, si hay) */
    {
        float my = y + h + 10.0f;
        my += ui_draw_active_users(cx, my);
        if (g_noItemUntil && (long)(g_noItemUntil - GetTickCount()) > 0)
        {
            const char *m = T(S_AV_NO_ITEM);
            float mw = gfx_text_width(0.85f, m) + 32.0f;
            my = y - 44.0f;   /* encima del panel (abajo ya estan los usuarios) */
            ui_round_rect(cx - mw * 0.5f, my, mw, 34.0f, 10.0f, 0xFF3A1010);
            gfx_text(cx - mw * 0.5f + 16.0f, my + 6.0f, 0.85f, COL_WHITE, m);
        }
    }
}

void avatar_draw(float cx, float floorY, float h)
{
    void *pic = ui_profile_texture();
    float c = cosf(g_turn), sn = sinf(g_turn), ac = c < 0.0f ? -c : c;
    float bodyW = h * 0.58f * (0.42f + 0.58f * ac);
    float headTurn = g_turn + g_yaw * 0.55f;
    float hc = cosf(headTurn), hac = hc < 0.0f ? -hc : hc;
    float hs = h * 0.46f, hw = hs * (0.62f + 0.38f * hac);
    float hx = cx + sn * h * 0.05f + g_yaw * h * 0.07f;
    float hy = floorY - h * 0.70f - g_pitch * h * 0.05f;
    unsigned int shirt = c >= 0.0f ? 0xFF41587A : 0xFF2F4058;
    int i;

    /* fondo oscuro redondo (separa al avatar de la lista de atras) y luz de
     * escenario */
    gfx_ellipse(cx - h * 0.66f, floorY - h * 0.52f - h * 0.66f, h * 1.32f, h * 1.32f, 0x70000000);
    for (i = 0; i < 5; ++i)
    {
        float r = h * (0.60f - i * 0.085f);
        gfx_ellipse(cx - r, floorY - h * 0.52f - r, 2.0f * r, 2.0f * r, 0x0BFFFFFF);
    }

    /* el avatar 3D si ya cargo; si no (cargando, sin avatar o sin XAvatar),
     * el busto 2.5D con la foto de jugador */
    if (av3d_ready() &&
        av3d_draw(cx - h * 0.50f, floorY - h * 1.32f, h, h * 1.36f, g_yaw, g_pitch, g_turn, g_dt))
    {
        draw_help(cx, floorY + 18.0f);
        return;
    }

    /* busto: hombros redondos y el pecho que se desvanece hacia abajo */
    gfx_ellipse(cx - bodyW * 0.5f, floorY - h * 0.40f, bodyW, h * 0.26f, shirt);
    gfx_rect_vgrad(cx - bodyW * 0.5f, floorY - h * 0.27f, bodyW, h * 0.27f, shirt, shirt & 0x00FFFFFF);
    /* cuello */
    gfx_rect(hx - hs * 0.12f, hy + hs * 0.36f, hs * 0.24f, hs * 0.26f, 0xFFC99C7A);

    /* cabeza: la foto de jugador de frente, la nuca de espaldas */
    gfx_ellipse(hx - hw * 0.5f - 4.0f, hy - hs * 0.5f - 4.0f, hw + 8.0f, hs + 8.0f, 0xFFFFFFFF);
    if (hc >= 0.0f && pic)
        gfx_image_ellipse(pic, hx - hw * 0.5f, hy - hs * 0.5f, hw, hs, 0xFFFFFFFF);
    else if (hc >= 0.0f)
    {
        gfx_ellipse(hx - hw * 0.5f, hy - hs * 0.5f, hw, hs, 0xFF5A5A5A);
        gfx_icon(UI_ICON_M_USER, hx - hs * 0.3f, hy - hs * 0.3f, hs * 0.6f, 0xFFE0E0E0);
    }
    else
        gfx_ellipse(hx - hw * 0.5f, hy - hs * 0.5f, hw, hs, 0xFF3A2A20);   /* nuca */

    draw_help(cx, floorY + 18.0f);
}
