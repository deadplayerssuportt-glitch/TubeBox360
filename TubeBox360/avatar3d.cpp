// avatar3d.cpp - El avatar 3D del jugador con la API XAvatar del XDK (ver
// avatar3d.h). C++ porque los renderers son interfaces con metodos y las
// matrices son de xboxmath; se expone en C.
//
// Pasos (todo sin trabar el ciclo principal):
//   1. XAvatarInitialize una vez, con el dispositivo de gfx.
//   2. Cada 2 s se mira que perfil tiene sesion. Si cambio, se piden sus
//      datos de avatar con XAvatarGetMetadataLocalUser (con XOVERLAPPED).
//   3. Con los datos, XAvatarCreateRenderer (carga modelos y texturas en
//      segundo plano). GetStatus() == S_OK cuando ya se puede dibujar.
//   4. Se le pone la animacion de estar parado en bucle (respira, se mueve
//      poquito) y cada cuadro: pose de la cabeza (palanca derecha),
//      Update, GetMatrices de cuerpo entero, giro del cuerpo (gatillos) y
//      Render dentro del rectangulo del escenario.

#include <xtl.h>
#include <xavatar.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "avatar3d.h"
#include "net.h"            // net_log: lo del avatar tambien queda en game:\tubebox_red.txt

// En este XDK la libreria es xavatar2: xavatar2d.lib (Debug), xavatar2.lib
// (Release); tambien trae xavatar2ltcg.lib para builds con LTCG.
#ifdef _DEBUG
#pragma comment(lib, "xavatar2d.lib")
#else
#pragma comment(lib, "xavatar2.lib")
#endif

#define AV_LOAD_THREAD   2      // hilo de hardware que carga los modelos (0 = audio, 1 = D3D, 4 = sugerencias)
#define AV_JOINT_BUFFERS 3      // el GPU va hasta 2 cuadros atras (D3DCREATE_BUFFER_2_FRAMES)
#define AV_HEAD_YAW      0.85f  // giro maximo de la cabeza (radianes, ~50 grados)
#define AV_HEAD_PITCH    0.45f  // arriba/abajo maximo (~25 grados)
#define AV_RETRY_MS      4000   // si fallo, se vuelve a intentar a los 4 s...
#define AV_RETRY_MAX     8      // ...hasta 8 veces por perfil
#define AV_SETTLE_MS     3000   // el perfil debe llevar 3 s sin cambiar de estado antes de pedir su avatar
#define AV_LOAD_MAX_MS   60000  // el renderer sigue "cargando" a los 60 s: se tira y se reintenta
#define AV_ZOOM          1.88f  // acerca la camara de cuerpo entero (1 = como la da el XDK)
#define AV_LIFT          0.35f  // sube la imagen (en unidades de pantalla: 2 = todo el alto del cuadro)
// 2026-11-22: con 1.88 / 0.35 la cabeza llega a la altura de "Buscar" y los
// pies a la de "Deportes" en el menu (calculado con la foto del usuario)

enum { ST_OFF, ST_IDLE, ST_META, ST_LOADING, ST_READY, ST_FAIL };

static int                g_st = ST_OFF;
static int                g_inited = 0;
static XAVATAR_METADATA   g_meta;
static XOVERLAPPED        g_ov;
static LPXAVATARRENDERER  g_r = NULL;
static LPXAVATARANIMATION g_stand = NULL;
static LPXAVATARANIMATION g_wave = NULL;
static int                g_wavePending = 0;   // saludar en cuanto se pueda
static int                g_carryLoop = 0;     // repitiendo el gesto del elemento interactivo
static int                g_standPlaying = 0;
static int                g_user = -2;      // -2 = todavia no se ha mirado
static XUID               g_xuid = 0;
static DWORD              g_lastCheck = 0;
static int                g_eyes = 0;
static int                g_baseOk = 0;     // ya se leyo la pose base de la cabeza
static char               g_headInfo[200] = "";
enum { AA_CHECK, AA_ON, AA_OFF };
static int                g_aaMode = AA_CHECK;       // 1 = los ojos estan mirando a un lado
static char               g_msg[96] = "XAvatar sin iniciar";
static DWORD              g_stateStart = 0;   // cuando empezo el paso actual
static DWORD              g_lastLog = 0;
static HRESULT            g_lastHr = 1;
static int                g_sign = -1;      // estado de sesion del perfil (local / Xbox Live)
static int                g_retries = 0;    // reintentos con este perfil
static DWORD              g_failAt = 0;     // cuando fallo el ultimo intento
static DWORD              g_askAt = 0;      // cuando se puede pedir el avatar (0 = ya se pidio)

// Animacion de estar parado (XAVATAR_ANIMATION_GENERIC_STAND_0 de xavatar.h).
// Se copia aqui porque DEFINE_GUID sin INITGUID solo la declara.
static const GUID kStand0 = { 0x00400000, 0x0003, 0x0003, { 0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0 } };
// Saludo (XAVATAR_ANIMATION_GENERIC_WAVE): al abrir el menu lateral
static const GUID kWave = { 0x00400000, 0x0007, 0x0003, { 0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0 } };

static void set_msg(const char *fmt, unsigned long v)
{
    _snprintf(g_msg, sizeof(g_msg) - 1, fmt, v);
    g_msg[sizeof(g_msg) - 1] = 0;
    OutputDebugStringA("[avatar] ");
    OutputDebugStringA(g_msg);
    OutputDebugStringA("\n");
    net_log("[avatar] %s", g_msg);
}

// Un intento fallo: queda en ST_FAIL y av3d_poll lo repite mas tarde
static void fail_now(void)
{
    g_st = ST_FAIL;
    g_failAt = GetTickCount();
}

static void release_all(void)
{
    if (g_st == ST_META)
        XCancelOverlapped(&g_ov);
    if (g_stand)
    {
        g_stand->Release();
        g_stand = NULL;
    }
    if (g_wave)
    {
        g_wave->Release();
        g_wave = NULL;
    }
    if (g_r)
    {
        g_r->Release();
        g_r = NULL;
    }
    g_standPlaying = 0;
    g_carryLoop = 0;
    g_eyes = 0;
    g_baseOk = 0;
}

extern "C" int av3d_init(void)
{
    HRESULT hr = XAvatarInitialize(XAVATAR_COORDINATE_SYSTEM_RIGHT_HANDED, XAVATAR_INITIALIZE_FLAGS_ENABLERENDERER,
                                   AV_LOAD_THREAD, AV_JOINT_BUFFERS, (LPDIRECT3DDEVICE9)gfx_device());
    if (FAILED(hr))
    {
        set_msg("XAvatarInitialize fallo (%08lX)", (unsigned long)hr);
        g_st = ST_FAIL;
        return 0;
    }
    set_msg("XAvatar listo", 0);
    g_st = ST_IDLE;
    g_inited = 1;
    return 1;
}

// Pide los datos del avatar del perfil con sesion
static void start_meta(int user)
{
    DWORD r;

    ZeroMemory(&g_ov, sizeof(g_ov));
    ZeroMemory(&g_meta, sizeof(g_meta));
    r = XAvatarGetMetadataLocalUser((DWORD)user, &g_meta, &g_ov);
    if (r != ERROR_IO_PENDING && r != ERROR_SUCCESS)
    {
        set_msg("XAvatarGetMetadataLocalUser fallo (%lu)", (unsigned long)r);
        fail_now();
        return;
    }
    set_msg("pidiendo el avatar del perfil %lu", (unsigned long)user);
    g_st = ST_META;
    g_stateStart = g_lastLog = GetTickCount();
}

static void create_renderer(void)
{
    HRESULT hr = XAvatarCreateRenderer(&g_meta, XAVATAR_COMPONENT_MASK_ALL, XAVATAR_SHADOW_SIZE_NONE,
                                       XAVATAR_MIPMAP_RECOMMENDED_BUFFER_SIZE, 0, &g_r);
    if (FAILED(hr) || !g_r)
    {
        set_msg("XAvatarCreateRenderer fallo (%08lX)", (unsigned long)hr);
        g_r = NULL;
        fail_now();
        return;
    }
    // la animacion de estar parado (si no carga, el renderer pone las suyas)
    if (FAILED(XAvatarLoadAnimation((const XAVATAR_ASSET_ID *)&kStand0, 0, &g_stand)))
        g_stand = NULL;
    if (FAILED(XAvatarLoadAnimation((const XAVATAR_ASSET_ID *)&kWave, 0, &g_wave)))
        g_wave = NULL;
    set_msg("cargando el avatar", 0);
    g_st = ST_LOADING;
    g_stateStart = GetTickCount();
    g_lastHr = 1;
}

extern "C" void av3d_poll(void)
{
    DWORD now = GetTickCount(), res = 0;
    int i, user = -1, sign = -1;
    XUID xuid = 0;
    HRESULT hr;

    if (!g_inited)
        return;

    // perfil con sesion (cada 2 s, como la foto de jugador)
    if (g_user == -2 || now - g_lastCheck >= 2000)
    {
        g_lastCheck = now;
        for (i = 0; i < 4 && user < 0; ++i)
            if (XUserGetSigninState(i) != eXUserSigninState_NotSignedIn)
                user = i;
        if (user >= 0 && XUserGetXUID(user, &xuid) != ERROR_SUCCESS)
            xuid = 0;
        if (user >= 0)
            sign = (int)XUserGetSigninState(user);
        if (user != g_user || xuid != g_xuid)
        {
            release_all();          // otro perfil (o ninguno): empezar de nuevo
            g_st = ST_IDLE;
            g_user = user;
            g_xuid = xuid;
            g_sign = sign;
            g_retries = 0;
            g_askAt = 0;
            if (user >= 0)
            {
                net_log("[avatar] perfil %d (sesion %s)", user,
                        sign == eXUserSigninState_SignedInToLive ? "Xbox Live" : "local");
                // se espera a que el perfil termine de entrar (local -> Xbox Live)
                g_askAt = now + AV_SETTLE_MS;
                if (!g_askAt)
                    g_askAt = 1;
                set_msg("esperando a que el perfil termine de entrar", 0);
            }
            else
                set_msg("sin sesion", 0);
        }
        else if (user >= 0 && sign != g_sign)
        {
            // 2026-11-22: un perfil con Xbox Live puede entrar primero "local"
            // y despues ya en Live; por eso se espera a que el estado no cambie.
            net_log("[avatar] el perfil %d paso a sesion %s", user,
                    sign == eXUserSigninState_SignedInToLive ? "Xbox Live" : "local");
            g_sign = sign;
            // Si todavia no se pidio, se espera otros 3 s. Si ya se pidio NO
            // se cancela (cancelar y volver a pedir dejaba todo atorado).
            if (g_st == ST_IDLE && g_askAt)
            {
                g_askAt = now + AV_SETTLE_MS;
                if (!g_askAt)
                    g_askAt = 1;
            }
        }
    }

    // pedir el avatar cuando el perfil ya lleva un rato sin cambiar
    if (g_st == ST_IDLE && g_user >= 0 && g_askAt && (int)(now - g_askAt) >= 0)
    {
        g_askAt = 0;
        start_meta(g_user);
    }

    // un intento fallo (la peticion TERMINO con error): se repite
    if (g_st == ST_FAIL && g_user >= 0 && g_retries < AV_RETRY_MAX && now - g_failAt >= AV_RETRY_MS)
    {
        ++g_retries;
        net_log("[avatar] reintento %d de %d", g_retries, AV_RETRY_MAX);
        release_all();
        start_meta(g_user);
    }

    switch (g_st)
    {
    case ST_META:
        now = GetTickCount();       // (start_meta pudo empezar en esta misma vuelta)
        if (!XHasOverlappedIoCompleted(&g_ov))
        {
            if (now - g_lastLog >= 10000)
            {
                // (no se cancela: se espera lo que tarde; queda en la bitacora)
                net_log("[avatar] sigue esperando los datos: %lu s, sesion %d", (unsigned long)((now - g_stateStart) / 1000),
                        g_user >= 0 ? (int)XUserGetSigninState(g_user) : -1);
                set_msg("esperando los datos del avatar (%lu s)", (unsigned long)((now - g_stateStart) / 1000));
                g_lastLog = now;
            }
            break;
        }
        if (XGetOverlappedResult(&g_ov, &res, FALSE) == ERROR_SUCCESS)
        {
            net_log("[avatar] llegaron los datos en %lu ms", (unsigned long)(now - g_stateStart));
            create_renderer();
        }
        else
        {
            // XAVATAR_E_USER_HAS_NO_AVATAR si el perfil no tiene avatar
            set_msg("no llegaron los datos del avatar (%08lX)", (unsigned long)XGetOverlappedExtendedError(&g_ov));
            fail_now();
        }
        break;
    case ST_LOADING:
        // Update tambien en la carga: el renderer puede avanzar su carga
        // ahi (no lo dice xavatar.h); sin dibujar no hace dano
        g_r->Update(0.016f);
        hr = g_r->GetStatus();
        if (hr != g_lastHr || now - g_lastLog >= 5000)
        {
            _snprintf(g_msg, sizeof(g_msg) - 1, "cargando el avatar: GetStatus %08lX (%lu s)",
                      (unsigned long)hr, (unsigned long)((now - g_stateStart) / 1000));
            g_msg[sizeof(g_msg) - 1] = 0;
            OutputDebugStringA("[avatar] ");
            OutputDebugStringA(g_msg);
            OutputDebugStringA("\n");
            g_lastHr = hr;
            g_lastLog = now;
        }
        if (hr == S_OK)
        {
            set_msg("avatar listo", 0);
            g_st = ST_READY;
        }
        else if (FAILED(hr) && hr != E_PENDING)
        {
            set_msg("el avatar no cargo (%08lX)", (unsigned long)hr);
            release_all();
            fail_now();
        }
        else if (now - g_stateStart > AV_LOAD_MAX_MS)
        {
            set_msg("el avatar sigue cargando a los 60 s (%08lX): se intenta de nuevo", (unsigned long)hr);
            release_all();
            fail_now();
        }
        break;
    case ST_READY:
        if (g_wavePending && g_wave && g_wave->GetStatus() == S_OK &&
            (!g_stand || g_stand->GetStatus() == S_OK))
        {
            // saluda y luego se queda parado (en bucle)
            LPXAVATARANIMATION seq[2];
            seq[0] = g_wave;
            seq[1] = g_stand;
            g_r->PlayAnimations(g_stand ? 2 : 1, seq,
                                XAVATAR_PLAYANIMATIONS_FLAGS_SNAP_FIRST_ANIM |
                                (g_stand ? XAVATAR_PLAYANIMATIONS_FLAGS_LOOP_LAST_ANIM : 0));
            g_wavePending = 0;
            g_standPlaying = 1;
        }
        else if (g_stand && !g_standPlaying && g_stand->GetStatus() == S_OK)
        {
            g_r->PlayAnimations(1, &g_stand, XAVATAR_PLAYANIMATIONS_FLAGS_LOOP_LAST_ANIM);
            g_standPlaying = 1;
        }
        break;
    }
}

extern "C" void av3d_wave(void)
{
    g_wavePending = 1;      // av3d_poll lo pone en cuanto el avatar y la animacion esten listos
    g_carryLoop = 0;
}

// Elemento interactivo (mascota, objeto...) = el "carryable" del avatar,
// con su propio esqueleto. PlayAnimations con NULL elige una animacion de
// espera al azar; con CARRYABLE_IDLES_ONLY solo las del elemento, y con
// LOOP_LAST_ANIM se repite sin fin.
extern "C" int av3d_gesture(void)
{
    const XAVATAR_SKELETON *csk = NULL;
    LPXAVATARANIMATION none = NULL;

    if (g_st != ST_READY || !g_r)
        return -1;
    if (g_carryLoop)
    {
        // otra vez abajo: de vuelta a estar parado
        g_carryLoop = 0;
        if (g_stand)
            g_r->PlayAnimations(1, &g_stand, XAVATAR_PLAYANIMATIONS_FLAGS_LOOP_LAST_ANIM);
        return 0;
    }
    if (FAILED(g_r->GetCarryableSkeleton(&csk)) || !csk || csk->Count == 0)
    {
        set_msg("el avatar no tiene elemento interactivo", 0);
        return -1;
    }
    if (FAILED(g_r->PlayAnimations(1, &none, XAVATAR_PLAYANIMATIONS_FLAGS_CARRYABLE_IDLES_ONLY |
                                             XAVATAR_PLAYANIMATIONS_FLAGS_LOOP_LAST_ANIM |
                                             XAVATAR_PLAYANIMATIONS_FLAGS_SNAP_FIRST_ANIM)))
    {
        set_msg("PlayAnimations del elemento interactivo fallo", 0);
        return -1;
    }
    g_wavePending = 0;
    g_carryLoop = 1;
    return 1;
}

extern "C" int av3d_ready(void)   { return g_st == ST_READY; }
extern "C" int av3d_loading(void) { return g_st == ST_META || g_st == ST_LOADING; }
extern "C" const char *av3d_status(void) { return g_msg; }

// Cabeza. Lo que se vio en la consola (2026-11-22):
//   1) Position = 0 -> la cabeza se fue a los pies.
//   2) base = la pose de reposo local del hueso -> el cuello se estiro.
// O sea que no hay que inventar la posicion ni la escala: se lee la pose
// tal como la da GetAvatarJointPose (antes de mandarle nada) y se le
// devuelve igual, solo con el giro de la palanca encima. Solo la cabeza
// (el cuello ya no se toca). Hasta que se mueve la palanca no se manda
// nada y la animacion la mueve sola.
static XAVATAR_SKELETON_POSE_JOINT g_base;

static void capture_base(void)
{
    char line[200];

    if (FAILED(g_r->GetAvatarJointPose(HEAD__Skeleton, &g_base)))
    {
        set_msg("GetAvatarJointPose fallo", 0);
        return;
    }
    _snprintf(line, sizeof(line) - 1, "cabeza: p %.2f %.2f %.2f  r %.2f %.2f %.2f %.2f  s %.2f %.2f %.2f",
              XMVectorGetX(g_base.Position), XMVectorGetY(g_base.Position), XMVectorGetZ(g_base.Position),
              XMVectorGetX(g_base.Rotation), XMVectorGetY(g_base.Rotation), XMVectorGetZ(g_base.Rotation),
              XMVectorGetW(g_base.Rotation),
              XMVectorGetX(g_base.Scale), XMVectorGetY(g_base.Scale), XMVectorGetZ(g_base.Scale));
    line[sizeof(line) - 1] = 0;
    OutputDebugStringA("[avatar] ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    strncpy(g_headInfo, line, sizeof(g_headInfo) - 1);
    g_baseOk = 1;
}

// El giro se pide en ejes del mundo (yaw sobre Y, pitch sobre X) y se pasa
// a los ejes del hueso con su rotacion de reposo B (B, luego el giro, luego
// B^-1); va encima del giro que ya trae la base.
static void turn_head(const XAVATAR_SKELETON *sk, float yaw, float pitch)
{
    XAVATAR_SKELETON_POSE_JOINT pose = g_base;
    XMVECTOR w, b, d;

    if (HEAD__Skeleton >= sk->Count)
        return;
    w = XMQuaternionRotationRollPitchYaw(-pitch, yaw, 0.0f);
    b = sk->pJoints[HEAD__Skeleton].BindPose.Rotation;
    d = XMQuaternionMultiply(XMQuaternionMultiply(b, w), XMQuaternionConjugate(b));
    pose.Rotation = XMQuaternionNormalize(XMQuaternionMultiply(d, g_base.Rotation));
    g_r->SetAvatarJointPose(HEAD__Skeleton, &pose);
}

static void set_eyes(float yaw, float pitch)
{
    DWORD l = XAVATAR_ANIMATED_TEXTURE_LAYER_EYE_NEUTRAL, r = l;

    if (yaw > 0.5f || yaw < -0.5f || pitch > 0.5f || pitch < -0.5f)
    {
        if (pitch > 0.6f)
            l = r = XAVATAR_ANIMATED_TEXTURE_LAYER_EYE_LOOK_UP;
        else if (pitch < -0.6f)
            l = r = XAVATAR_ANIMATED_TEXTURE_LAYER_EYE_LOOK_DOWN;
        else if (yaw > 0.0f)
        {
            l = XAVATAR_ANIMATED_TEXTURE_LAYER_EYE_LOOK_OUTER;
            r = XAVATAR_ANIMATED_TEXTURE_LAYER_EYE_LOOK_INNER;
        }
        else
        {
            l = XAVATAR_ANIMATED_TEXTURE_LAYER_EYE_LOOK_INNER;
            r = XAVATAR_ANIMATED_TEXTURE_LAYER_EYE_LOOK_OUTER;
        }
        g_eyes = 1;
    }
    else if (!g_eyes)
        return;             // no tocar: la animacion parpadea sola
    else
        g_eyes = 0;         // de vuelta al centro: una vez a neutral
    g_r->SetTextureLayer(XAVATAR_ANIMATED_TEXTURE_EYE_LEFT, l);
    g_r->SetTextureLayer(XAVATAR_ANIMATED_TEXTURE_EYE_RIGHT, r);
}

extern "C" int av3d_draw(float x, float y, float w, float h, float yaw, float pitch, float turn, float dt)
{
    const XAVATAR_SKELETON *sk = NULL;
    XMMATRIX model, view, proj;
    HRESULT hr;

    if (g_st != ST_READY || !g_r)
        return 0;

    // cabeza (ver turn_head)
    if (g_baseOk || yaw != 0.0f || pitch != 0.0f)
    {
        if (!g_baseOk)
            capture_base();
        if (g_baseOk && SUCCEEDED(g_r->GetAvatarSkeleton(&sk)) && sk)
            turn_head(sk, yaw * AV_HEAD_YAW, pitch * AV_HEAD_PITCH);
    }
    set_eyes(yaw, pitch);

    if (dt < 0.0f || dt > 0.1f)
        dt = 0.016f;
    g_r->Update(dt);
    if (FAILED(g_r->GetMatrices(XAVATAR_RENDER_VIEW_FULL_BODY, w / h, &model, &view, &proj)))
        return 0;
    model = XMMatrixMultiply(XMMatrixRotationY(turn), model);   // giro del cuerpo con los gatillos
    proj = XMMatrixMultiply(proj, XMMatrixScaling(AV_ZOOM, AV_ZOOM, 1.0f));   // mas cerca...
    proj = XMMatrixMultiply(proj, XMMatrixTranslation(0.0f, AV_LIFT, 0.0f));  // ...y mas arriba

    // con antialias (superficie aparte con MSAA): si el avatar escribe su
    // alfa, se pone encima con orillas suaves; si no (se revisa una vez), se
    // dibuja directo en la pantalla como antes
    if (g_aaMode != AA_OFF)
    {
        int aw = (int)(w + 0.5f), ah = (int)(h + 0.5f), withA, withC;
        void *tex;

        if (gfx_begin_3d_aa(aw, ah))
        {
            hr = g_r->Render((LPDIRECT3DDEVICE9)gfx_device(), model, view, proj);
            tex = gfx_end_3d_aa();
            if (g_aaMode == AA_CHECK && SUCCEEDED(hr))
            {
                gfx_texture_count(tex, aw, ah, &withA, &withC);
                if (withC > 200)
                {
                    g_aaMode = withA * 2 >= withC ? AA_ON : AA_OFF;
                    _snprintf(g_msg, sizeof(g_msg) - 1, "antialias %dx: %s (alfa %d / color %d)",
                              gfx_aa_samples(), g_aaMode == AA_ON ? "si" : "no", withA, withC);
                    g_msg[sizeof(g_msg) - 1] = 0;
                    OutputDebugStringA("[avatar] ");
                    OutputDebugStringA(g_msg);
                    OutputDebugStringA("\n");
                }
            }
            if (g_aaMode != AA_OFF)
            {
                gfx_image_premul(tex, x, y, w, h);
                return SUCCEEDED(hr);
            }
        }
        else
            g_aaMode = AA_OFF;
    }
    if (!gfx_begin_3d(x, y, w, h))
        return 0;
    hr = g_r->Render((LPDIRECT3DDEVICE9)gfx_device(), model, view, proj);
    gfx_end_3d();
    return SUCCEEDED(hr);
}
