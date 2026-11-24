// gfx.cpp - Capa de graficos con Direct3D 9 de Xbox 360.
// Archivo C++: la API de D3D del XDK se usa con metodos (g_dev->Clear(...)),
// asi que aqui se queda y se expone en C mediante gfx.h.
//
// Dos formas de mostrar cada cuadro:
//   - Present (normal): la usan las pantallas de lista y formatos.
//   - Swap (modo video): copiamos la imagen a uno de 3 buffers propios con
//     Resolve, esperamos el refresco vertical y le decimos a la pantalla
//     que lo muestre con Swap. Es lo que se usa mientras el decodificador
//     H.264 de la consola esta abierto, porque Present se cruza con el
//     trabajo que el decodificador le encarga a la GPU. (Dato tomado del
//     GPU-MEDIA-PLAYER-360 de HG-ERIK.)
//
// Todo se dibuja con un solo shader: cada vertice lleva posicion, UV y
// color. Se agrupa todo lo que usa la misma textura en un solo dibujo
// (bind() solo manda lo pendiente cuando cambia la textura).
//
// Letras:
//   - gfx_text: Roboto (ui_assets.c) en 3 tamanos; se usa el atlas mas
//     cercano al tamano pedido. Si no cargo, la monoespaciada.
//   - gfx_text_mono: la monoespaciada de font_data.h (tablas y paneles).
// Los rectangulos usan un bloque blanco que tiene cada atlas de letras, asi
// texto y rectangulos van en el mismo dibujo.

#include <xtl.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "gfx.h"
#include "font_data.h"
#include "ui_assets.h"
#include "arabic.h"

// Centro de los pixeles: D3D9 (y Xbox 360 por omision) los pone en
// coordenadas enteras; con -0.5 cada texel cae justo en un pixel y las
// letras a tamano 1:1 se ven nitidas. Si en la consola se vieran borrosas
// al cambiar esto, poner 0.0f.
#define GFX_HALF_PIXEL (-0.5f)

#define ATLAS_COLS 16      // glifos por fila en la textura
#define CELL_PAD   1       // pixeles vacios alrededor de cada glifo
#define STRIDE_X   (FONT_CELL_W + 2 * CELL_PAD)   // 14
#define STRIDE_Y   (FONT_CELL_H + 2 * CELL_PAD)   // 26 -> 12 filas = 312 px
#define ATLAS_W    256
#define ATLAS_H    512
#define WHITE_U    (250.0f / ATLAS_W)   // centro del bloque blanco
#define WHITE_V    (506.0f / ATLAS_H)
#define MAX_VERTS  1200    // 200 cuadros por lote (~38 KB por DrawPrimitiveUP)

struct Vertex
{
    float x, y;        // posicion en pixeles
    float u, v;        // coordenada de textura
    float r, g, b, a;  // color
};

static IDirect3DDevice9            *g_dev  = NULL;
static IDirect3DVertexShader9      *g_vs   = NULL;
static IDirect3DPixelShader9       *g_ps   = NULL;
static IDirect3DPixelShader9       *g_psCircle = NULL;   // avatares redondos
static IDirect3DTexture9           *g_white = NULL;      // 4x4 blanco (circulos sin imagen)
static IDirect3DVertexDeclaration9 *g_decl = NULL;
static IDirect3DTexture9           *g_tex  = NULL;     // letra monoespaciada
static IDirect3DTexture9           *g_bound = NULL;    // la que esta puesta ahora
static D3DPRESENT_PARAMETERS        g_pp;

// Letra de la interfaz (Roboto) en 3 tamanos, mas la Bold de los titulos
struct UiFont
{
    const UiFontDesc  *d;
    IDirect3DTexture9 *tex;
    short              map[256];   // codepoint -> posicion en d->glyphs (-1 = no hay)
    float              lineH;      // ascent + descent
    float              whiteU, whiteV;
};
static UiFont             g_ui[3];
static int                g_uiCount = 0;
static UiFont             g_uiBold;         // Roboto Bold 32 (titulos)
static UiFont             g_extra;          // japones y arabe (se carga la primera vez que hace falta)
static int                g_extraState = 0; // 0 sin cargar, 1 lista, -1 no se pudo
static int                g_boldOk = 0;
static int                g_bold = 0;       // gfx_font_bold(1): se usa la Bold
static IDirect3DTexture9 *g_icons = NULL;
static IDirect3DTexture9 *g_logo = NULL;
static IDirect3DTexture9 *g_logoBig = NULL;   // pantalla de carga
static char               g_assetErr[128] = "";

static void load_ui_assets(void);

// Modo video (Swap)
static int                g_swap       = 0;
static IDirect3DTexture9 *g_fronts[3];
static int                g_frontCount = -1;   // -1 = aun no se crean
static int                g_frontNext  = 0;
static HANDLE             g_vblankEv   = NULL;
static volatile LONG      g_vblanks    = 0;

static Vertex g_verts[MAX_VERTS];
static int    g_count = 0;

// c0.x = 2/ancho, c0.y = -2/alto: convierte pixeles a coordenadas -1..1.
// c0.zw = medio pixel (ver GFX_HALF_PIXEL).
static const char g_vsSrc[] =
    "float4 screen : register(c0);\n"
    "struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "VSOut main(VSIn i)\n"
    "{\n"
    "    VSOut o;\n"
    "    o.pos = float4((i.pos.x + screen.z) * screen.x - 1.0, (i.pos.y + screen.w) * screen.y + 1.0, 0.0, 1.0);\n"
    "    o.uv  = i.uv;\n"
    "    o.col = i.col;\n"
    "    return o;\n"
    "}\n";

static const char g_psSrc[] =
    "sampler2D tex : register(s0);\n"
    "struct PSIn { float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "float4 main(PSIn i) : COLOR\n"
    "{\n"
    "    return tex2D(tex, i.uv) * i.col;\n"
    "}\n";

// Igual que el de arriba, pero recorta un circulo: el alfa baja a 0 fuera
// del radio, con un borde suave de c0.x (en unidades de la textura) para
// que no se vea dentado. Para los avatares de los canales.
static const char g_psCircleSrc[] =
    "sampler2D tex : register(s0);\n"
    "float4 edge : register(c0);\n"
    "struct PSIn { float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "float4 main(PSIn i) : COLOR\n"
    "{\n"
    "    float4 c = tex2D(tex, i.uv) * i.col;\n"
    "    float d = length(i.uv - float2(0.5, 0.5));\n"
    "    c.a = c.a * saturate((0.5 - d) / edge.x);\n"
    "    return c;\n"
    "}\n";

static const D3DVERTEXELEMENT9 g_elements[] =
{
    { 0,  0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0,  8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    D3DDECL_END()
};

static void log_msg(const char *s)
{
    OutputDebugStringA(s);
}

static ID3DXBuffer *compile_shader(const char *src, const char *profile)
{
    ID3DXBuffer *code = NULL;
    ID3DXBuffer *err  = NULL;
    HRESULT hr = D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL,
                                   "main", profile, 0, &code, &err, NULL);
    if (FAILED(hr))
    {
        log_msg("[gfx] Error compilando shader ");
        log_msg(profile);
        log_msg(":\n");
        if (err)
            log_msg((const char *)err->GetBufferPointer());
    }
    if (err)
        err->Release();
    return SUCCEEDED(hr) ? code : NULL;
}

// Copia los glifos de font_data.h a una textura lineal ARGB.
// RGB siempre blanco; el alfa es la cobertura del glifo.
static int build_atlas(void)
{
    D3DLOCKED_RECT lr;

    if (FAILED(g_dev->CreateTexture(ATLAS_W, ATLAS_H, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                    D3DPOOL_MANAGED, &g_tex, NULL)))
    {
        log_msg("[gfx] CreateTexture fallo\n");
        return 0;
    }
    if (FAILED(g_tex->LockRect(0, &lr, NULL, 0)))
    {
        log_msg("[gfx] LockRect fallo\n");
        return 0;
    }

    for (int y = 0; y < ATLAS_H; ++y)
    {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < ATLAS_W; ++x)
            row[x] = 0x00FFFFFF;
    }

    // El ultimo glifo de font_data.h es un bloque blanco; no se copia
    // porque abajo se pone uno aislado para que no se mezcle con otros.
    for (int s = 0; s < FONT_GLYPHS - 1; ++s)
    {
        // Cada glifo lleva un borde vacio para que el filtrado de la GPU
        // no tome pixeles del glifo vecino (las "rayitas" sobre las letras).
        int ox = (s % ATLAS_COLS) * STRIDE_X + CELL_PAD;
        int oy = (s / ATLAS_COLS) * STRIDE_Y + CELL_PAD;
        const unsigned char *src = g_fontData[s];

        for (int y = 0; y < FONT_CELL_H; ++y)
        {
            DWORD *row = (DWORD *)((BYTE *)lr.pBits + (oy + y) * lr.Pitch) + ox;
            for (int x = 0; x < FONT_CELL_W; ++x)
                row[x] = ((DWORD)src[y * FONT_CELL_W + x] << 24) | 0x00FFFFFF;
        }
    }

    // Bloque blanco de 8x8 en una esquina libre, para los rectangulos
    for (int y = 502; y < 510; ++y)
    {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 246; x < 254; ++x)
            row[x] = 0xFFFFFFFF;
    }

    g_tex->UnlockRect(0);
    return 1;
}

static void flush(void)
{
    if (g_count == 0)
        return;
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, g_count / 3, g_verts, sizeof(Vertex));
    g_count = 0;
}

static void set_vertex(Vertex *v, float x, float y, float u, float t,
                       float r, float g, float b, float a)
{
    v->x = x; v->y = y; v->u = u; v->v = t;
    v->r = r; v->g = g; v->b = b; v->a = a;
}

static void push_quad(float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, unsigned int c)
{
    float a = ((c >> 24) & 0xFF) / 255.0f;
    float r = ((c >> 16) & 0xFF) / 255.0f;
    float g = ((c >>  8) & 0xFF) / 255.0f;
    float b = ( c        & 0xFF) / 255.0f;
    Vertex *v;

    if (g_count + 6 > MAX_VERTS)
        flush();

    v = &g_verts[g_count];
    set_vertex(&v[0], x0, y0, u0, v0, r, g, b, a);
    set_vertex(&v[1], x1, y0, u1, v0, r, g, b, a);
    set_vertex(&v[2], x0, y1, u0, v1, r, g, b, a);
    v[3] = v[2];
    v[4] = v[1];
    set_vertex(&v[5], x1, y1, u1, v1, r, g, b, a);
    g_count += 6;
}

// Cambia la textura solo si es otra (manda antes lo pendiente)
static void bind(IDirect3DTexture9 *t)
{
    if (t != g_bound)
    {
        flush();
        g_dev->SetTexture(0, t);
        g_bound = t;
    }
}

// UV del bloque blanco de la textura puesta (o pone la monoespaciada)
static void white_uv(float *u, float *v)
{
    int i;

    for (i = 0; i < g_uiCount; ++i)
        if (g_bound == g_ui[i].tex)
        {
            *u = g_ui[i].whiteU;
            *v = g_ui[i].whiteV;
            return;
        }
    if (g_boldOk && g_bound == g_uiBold.tex)
    {
        *u = g_uiBold.whiteU;
        *v = g_uiBold.whiteV;
        return;
    }
    bind(g_tex);
    *u = WHITE_U;
    *v = WHITE_V;
}

// Lee un caracter UTF-8 y avanza. Si la secuencia es invalida devuelve '?'.
static const char *next_codepoint(const char *s, unsigned int *cp)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p[0] < 0x80)
    {
        *cp = p[0];
        return s + 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
    {
        *cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return s + 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        *cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return s + 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
    {
        *cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
              ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return s + 4;
    }
    *cp = '?';
    return s + 1;
}

#define SLOT_BLANK (-1)

// Indice del glifo en la textura. La fuente solo tiene ASCII y Latin-1:
//   - la puntuacion tipografica que es comun en los titulos (guiones
//     largos, comillas curvas, puntos suspensivos) sale como su parecida
//     en ASCII;
//   - emojis y simbolos salen en blanco (un '?' por cada uno se ve feo);
//   - lo demas (cirilico, japones...) sale como '?'.
static int glyph_slot(unsigned int cp)
{
    if (cp >= 32 && cp <= 126)
        return (int)cp - 32;
    if (cp >= 160 && cp <= 255)
        return 95 + (int)(cp - 160);

    switch (cp)
    {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return '-' - 32;
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
        return '\'' - 32;
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
        return '"' - 32;
    case 0x2022: case 0x2027: case 0x30FB:
        return 95 + (0xB7 - 160);   // punto medio
    case 0x2026:
        return '.' - 32;
    case 0x2039:
        return '<' - 32;
    case 0x203A:
        return '>' - 32;
    }

    if ((cp >= 0x1F000 && cp <= 0x1FAFF) ||   // emojis
        (cp >= 0x2600 && cp <= 0x27BF) ||     // simbolos varios
        (cp >= 0x2B00 && cp <= 0x2BFF))
        return SLOT_BLANK;
    return '?' - 32;
}

// Caracteres invisibles que unen o modifican emojis: no ocupan lugar
static int zero_width(unsigned int cp)
{
    return cp == 0x200B || cp == 0x200C || cp == 0x200D ||
           cp == 0xFE0E || cp == 0xFE0F ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF);  // tonos de piel
}

// Lo llama D3D en cada refresco vertical (60 veces por segundo)
static void WINAPI on_vblank(D3DVBLANKDATA *data)
{
    (void)data;
    InterlockedIncrement(&g_vblanks);
    if (g_vblankEv)
        SetEvent(g_vblankEv);
}

extern "C" int gfx_init(void)
{
    IDirect3D9 *d3d;
    D3DPRESENT_PARAMETERS &pp = g_pp;
    ID3DXBuffer *code;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
    {
        log_msg("[gfx] Direct3DCreate9 fallo\n");
        return 0;
    }

    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth        = SCREEN_W;
    pp.BackBufferHeight       = SCREEN_H;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    // CREATE_THREAD_ON_1 y BUFFER_2_FRAMES: asi lo pide el decodificador
    // H.264 de la consola (el hilo de D3D en el nucleo 1, 2 cuadros en cola).
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                 D3DCREATE_CREATE_THREAD_ON_1 |
                                 D3DCREATE_BUFFER_2_FRAMES, &pp, &g_dev)))
    {
        log_msg("[gfx] CreateDevice fallo\n");
        return 0;
    }

    code = compile_shader(g_vsSrc, "vs_2_0");
    if (!code)
        return 0;
    g_dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &g_vs);
    code->Release();

    code = compile_shader(g_psSrc, "ps_2_0");
    if (!code)
        return 0;
    g_dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &g_ps);
    code->Release();

    // si este falla, los avatares salen cuadrados (no es grave)
    code = compile_shader(g_psCircleSrc, "ps_2_0");
    if (code)
    {
        g_dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &g_psCircle);
        code->Release();
    }

    if (FAILED(g_dev->CreateVertexDeclaration(g_elements, &g_decl)))
    {
        log_msg("[gfx] CreateVertexDeclaration fallo\n");
        return 0;
    }

    if (!build_atlas())
        return 0;
    load_ui_assets();   // Roboto, iconos y logo (si fallan, se sigue sin ellos)

    if (SUCCEEDED(g_dev->CreateTexture(4, 4, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &g_white, NULL)))
    {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_white->LockRect(0, &lr, NULL, 0)))
        {
            for (int y = 0; y < 4; ++y)
            {
                DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < 4; ++x)
                    row[x] = 0xFFFFFFFF;
            }
            g_white->UnlockRect(0);
        }
    }

    log_msg("[gfx] Listo\n");
    return 1;
}

extern "C" void gfx_begin(unsigned int clearColor)
{
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 1.0f, 0);
    g_dev->BeginScene();
    gfx_reset_state();
    g_count = 0;
}

extern "C" void gfx_flush(void)
{
    flush();
}

extern "C" void gfx_reset_state(void)
{
    float screen[4] = { 2.0f / SCREEN_W, -2.0f / SCREEN_H, GFX_HALF_PIXEL, GFX_HALF_PIXEL };

    g_dev->SetVertexDeclaration(g_decl);
    g_dev->SetVertexShader(g_vs);
    g_dev->SetPixelShader(g_ps);
    g_dev->SetVertexShaderConstantF(0, screen, 1);

    g_dev->SetTexture(0, g_tex);
    g_bound = g_tex;
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

    g_dev->SetRenderState(D3DRS_ZENABLE,          FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
}

extern "C" void gfx_rect(float x, float y, float w, float h, unsigned int color)
{
    float u, v;

    white_uv(&u, &v);
    push_quad(x, y, x + w, y + h, u, v, u, v, color);
}

extern "C" void gfx_rect_vgrad(float x, float y, float w, float h, unsigned int top, unsigned int bottom)
{
    float u, v;
    float ta = ((top >> 24) & 0xFF) / 255.0f, tr = ((top >> 16) & 0xFF) / 255.0f;
    float tg = ((top >> 8) & 0xFF) / 255.0f, tb = (top & 0xFF) / 255.0f;
    float ba = ((bottom >> 24) & 0xFF) / 255.0f, br = ((bottom >> 16) & 0xFF) / 255.0f;
    float bg = ((bottom >> 8) & 0xFF) / 255.0f, bb = (bottom & 0xFF) / 255.0f;
    Vertex *vx;

    white_uv(&u, &v);
    if (g_count + 6 > MAX_VERTS)
        flush();
    vx = &g_verts[g_count];
    set_vertex(&vx[0], x,     y,     u, v, tr, tg, tb, ta);
    set_vertex(&vx[1], x + w, y,     u, v, tr, tg, tb, ta);
    set_vertex(&vx[2], x,     y + h, u, v, br, bg, bb, ba);
    vx[3] = vx[2];
    vx[4] = vx[1];
    set_vertex(&vx[5], x + w, y + h, u, v, br, bg, bb, ba);
    g_count += 6;
}

// --- Letra monoespaciada ---------------------------------------------

extern "C" float gfx_text_mono(float x, float y, float scale, unsigned int color, const char *utf8)
{
    const float cw = FONT_CELL_W * scale;
    const float ch = FONT_CELL_H * scale;
    const float du = (float)FONT_CELL_W / ATLAS_W;
    const float dv = (float)FONT_CELL_H / ATLAS_H;
    const char *s = utf8;
    unsigned int cp;

    bind(g_tex);
    while (*s)
    {
        s = next_codepoint(s, &cp);
        if (zero_width(cp))
            continue;
        int slot = glyph_slot(cp);
        if (cp != ' ' && slot != SLOT_BLANK)
        {
            float u0 = (float)((slot % ATLAS_COLS) * STRIDE_X + CELL_PAD) / ATLAS_W;
            float v0 = (float)((slot / ATLAS_COLS) * STRIDE_Y + CELL_PAD) / ATLAS_H;
            push_quad(x, y, x + cw, y + ch, u0, v0, u0 + du, v0 + dv, color);
        }
        x += cw;
    }
    return x;
}

extern "C" float gfx_text_mono_width(float scale, const char *utf8)
{
    const char *s = utf8;
    unsigned int cp;
    int n = 0;

    while (*s)
    {
        s = next_codepoint(s, &cp);
        if (!zero_width(cp))
            ++n;
    }
    return n * FONT_CELL_W * scale;
}

// --- Letra de la interfaz (Roboto) -----------------------------------

// El atlas mas chico que no haya que agrandar mucho (24 px de linea = escala 1)
static UiFont *pick_font(float scale, float *k)
{
    float want = GFX_CHAR_H * scale;
    int i;

    if (g_bold && g_boldOk)
    {
        *k = want / g_uiBold.lineH;
        return &g_uiBold;
    }
    for (i = 0; i < g_uiCount - 1; ++i)
        if (g_ui[i].lineH >= want * 0.92f)
            break;
    *k = want / g_ui[i].lineH;
    return &g_ui[i];
}

static IDirect3DTexture9 *load_png(const unsigned char *png, int len, int w, int h, const char *what);

// Letra de japones o arabe (atlas extra), o NULL si no la tiene
static const UiGlyph *extra_glyph(unsigned int cp)
{
    int lo, hi, mid;

    if (cp < 0x0600 || cp > 0xFFEF)
        return NULL;
    if (g_extraState == 0)
    {
        // primera vez: 2048x2048, se carga solo si alguien escribe en japones o arabe
        g_extra.d = &g_fontExtra;
        g_extra.tex = load_png(g_fontExtra.png, g_fontExtra.pngLen, g_fontExtra.texW, g_fontExtra.texH,
                               "La letra de japones y arabe");
        g_extra.lineH = (float)(g_fontExtra.ascent + g_fontExtra.descent);
        g_extraState = g_extra.tex ? 1 : -1;
    }
    if (g_extraState != 1)
        return NULL;
    lo = 0;
    hi = g_fontExtra.count - 1;
    while (lo <= hi)
    {
        mid = (lo + hi) / 2;
        if (g_fontExtra.glyphs[mid].cp == cp)
            return &g_fontExtra.glyphs[mid];
        if (g_fontExtra.glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

// La letra de un codepoint, o NULL si va en blanco (emojis). Los que no
// tiene se cambian por su parecida (glyph_slot) o por '?'. *isExtra = 1 si
// es del atlas de japones/arabe (otra textura y otra escala).
static const UiGlyph *ui_glyph2(const UiFont *f, unsigned int cp, int *isExtra)
{
    const UiGlyph *e;
    int i, slot;

    *isExtra = 0;
    if (cp < 256 && f->map[cp] >= 0)
        return &f->d->glyphs[f->map[cp]];
    for (i = 191; i < f->d->count; ++i)   // los signos tipograficos van al final
        if (f->d->glyphs[i].cp == cp)
            return &f->d->glyphs[i];
    e = extra_glyph(cp);
    if (e)
    {
        *isExtra = 1;
        return e;
    }
    slot = glyph_slot(cp);
    if (slot == SLOT_BLANK)
        return &f->d->glyphs[0];          // el espacio
    cp = slot < 95 ? (unsigned int)(slot + 32) : (unsigned int)(slot - 95 + 160);
    if (cp < 256 && f->map[cp] >= 0)
        return &f->d->glyphs[f->map[cp]];
    return &f->d->glyphs[f->map['?']];
}

static const UiGlyph *ui_glyph(const UiFont *f, unsigned int cp)
{
    int ex;
    return ui_glyph2(f, cp, &ex);
}

extern "C" float gfx_text(float x, float y, float scale, unsigned int color, const char *utf8)
{
    static char vis[1600];
    const char *s;
    const UiGlyph *g;
    unsigned int cp;
    UiFont *f;
    float k, kx, kk, base, x0, y0, iw, ih, ixw = 0.0f, ixh = 0.0f;
    int crisp, ex;

    if (g_uiCount == 0)
        return gfx_text_mono(x, y, scale, color, utf8);

    s = arabic_visual(utf8, vis, sizeof(vis));   // arabe: unido y de derecha a izquierda
    f = pick_font(scale, &k);
    kx = GFX_CHAR_H * scale / (g_extra.lineH > 0.0f ? g_extra.lineH : 1.0f);
    iw = 1.0f / f->d->texW;
    ih = 1.0f / f->d->texH;
    crisp = k > 0.96f && k < 1.04f;   // casi 1:1: se alinea a pixeles enteros
    base = y + f->d->ascent * k;
    if (crisp)
        base = (float)(int)(base + 0.5f);

    bind(f->tex);
    while (*s)
    {
        s = next_codepoint(s, &cp);
        if (zero_width(cp))
            continue;
        g = ui_glyph2(f, cp, &ex);
        kk = k;
        if (ex)
        {
            // japones/arabe: su textura y su escala (misma linea base)
            kx = GFX_CHAR_H * scale / g_extra.lineH;
            kk = kx;
            ixw = 1.0f / g_extra.d->texW;
            ixh = 1.0f / g_extra.d->texH;
            bind(g_extra.tex);
        }
        else
            bind(f->tex);
        if (g->w > 0)
        {
            x0 = x + g->ox * kk;
            y0 = base + g->oy * kk;
            if (crisp && !ex)
            {
                x0 = (float)(int)(x0 + 0.5f);
                y0 = (float)(int)(y0 + 0.5f);
            }
            if (ex)
                push_quad(x0, y0, x0 + g->w * kk, y0 + g->h * kk,
                          g->x * ixw, g->y * ixh, (g->x + g->w) * ixw, (g->y + g->h) * ixh, color);
            else
                push_quad(x0, y0, x0 + g->w * kk, y0 + g->h * kk,
                          g->x * iw, g->y * ih, (g->x + g->w) * iw, (g->y + g->h) * ih, color);
        }
        x += g->adv * kk;
    }
    return x;
}

// Ancho de un caracter con la letra que toca a esa escala
static float cp_width(unsigned int cp, float scale)
{
    UiFont *f;
    float k;

    if (zero_width(cp))
        return 0.0f;
    if (g_uiCount == 0)
        return FONT_CELL_W * scale;
    {
        const UiGlyph *g;
        int ex;
        f = pick_font(scale, &k);
        g = ui_glyph2(f, cp, &ex);
        if (ex)
            return g->adv * (GFX_CHAR_H * scale / g_extra.lineH);
        return g->adv * k;
    }
}

extern "C" float gfx_text_width(float scale, const char *utf8)
{
    static char vis[1600];
    const char *s;
    unsigned int cp;
    float w = 0.0f;

    if (g_uiCount == 0)
        return gfx_text_mono_width(scale, utf8);
    s = arabic_visual(utf8, vis, sizeof(vis));   // (las formas unidas miden distinto)
    while (*s)
    {
        s = next_codepoint(s, &cp);
        w += cp_width(cp, scale);
    }
    return w;
}

// Copia src en dst sin pasarse de size bytes (sin cortar un caracter UTF-8)
static void copy_bytes(char *dst, int size, const char *src, int n)
{
    if (n > size - 1)
    {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

extern "C" void gfx_fit_text(const char *src, float maxW, float scale, char *dst, int dstSize)
{
    const char *dots = g_uiCount ? "\xE2\x80\xA6" : "...";   // ... en un solo signo con Roboto
    const char *s = src, *cut = src;
    unsigned int cp;
    float w = 0.0f, dotsW;
    int n;

    if (!src)
    {
        dst[0] = 0;
        return;
    }
    if (gfx_text_width(scale, src) <= maxW)
    {
        copy_bytes(dst, dstSize, src, (int)strlen(src));
        return;
    }
    dotsW = gfx_text_width(scale, dots);
    while (*s)
    {
        s = next_codepoint(s, &cp);
        w += cp_width(cp, scale);
        if (w + dotsW > maxW)
            break;
        cut = s;
    }
    /* sin espacios sobrantes antes de los puntos */
    while (cut > src && cut[-1] == ' ')
        --cut;
    n = (int)(cut - src);
    if (n > dstSize - 4)
        n = dstSize - 4;
    copy_bytes(dst, dstSize - 3, src, n);
    strcat(dst, dots);
}

extern "C" const char *gfx_wrap_text(const char *src, float maxW, float scale, char *line, int lineSize)
{
    const char *s = src, *lastSpace = NULL, *fitEnd = src;
    unsigned int cp;
    float w = 0.0f;

    if (!src)
    {
        line[0] = 0;
        return "";
    }
    while (*s)
    {
        const char *at = s;
        s = next_codepoint(s, &cp);
        w += cp_width(cp, scale);
        if (w > maxW)
        {
            if (lastSpace && lastSpace > src)
                fitEnd = lastSpace;      // se corta en el ultimo espacio (o despues de un kanji/kana)
            else if (fitEnd == src)
                fitEnd = s;              // una palabra mas larga que la linea
            copy_bytes(line, lineSize, src, (int)(fitEnd - src));
            while (*fitEnd == ' ')
                ++fitEnd;
            return fitEnd;
        }
        if (cp == ' ')
            lastSpace = at;
        else if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xFF00 && cp <= 0xFFEF))
            lastSpace = s;               // japones: se puede cortar despues de cualquier letra
        fitEnd = s;
    }
    copy_bytes(line, lineSize, src, (int)strlen(src));
    return s;
}

// --- Iconos y logo ----------------------------------------------------

extern "C" int gfx_icon(int id, float x, float y, float size, unsigned int color)
{
    const UiIconRect *r;

    if (!g_icons || id < 0 || id >= UI_ICON_COUNT)
        return 0;
    r = &g_icon_rects[id];
    bind(g_icons);
    push_quad(x, y, x + size, y + size,
              (float)r->x / g_icons_w, (float)r->y / g_icons_h,
              (float)(r->x + r->w) / g_icons_w, (float)(r->y + r->h) / g_icons_h, color);
    return 1;
}

extern "C" float gfx_logo(float x, float y, float h)
{
    float w;

    if (!g_logo)
        return 0.0f;
    w = h * g_logo_w / g_logo_h;
    bind(g_logo);
    push_quad(x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
    return w;
}

extern "C" const char *gfx_assets_error(void)
{
    return g_assetErr;
}

extern "C" void gfx_font_bold(int on)
{
    g_bold = on;
}

extern "C" float gfx_logo_width(int big, float h)
{
    if (big && g_logoBig)
        return h * g_logo_big_w / g_logo_big_h;
    return g_logo ? h * g_logo_w / g_logo_h : 0.0f;
}

extern "C" float gfx_logo_big(float x, float y, float h)
{
    float w;

    if (!g_logoBig)
        return gfx_logo(x, y, h);   // sin el grande, el chico estirado
    w = h * g_logo_big_w / g_logo_big_h;
    bind(g_logoBig);
    push_quad(x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
    return w;
}

// PNG incluido en el .xex -> textura del tamano exacto
static IDirect3DTexture9 *load_png(const unsigned char *png, int len, int w, int h, const char *what)
{
    IDirect3DTexture9 *tex = NULL;
    HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(g_dev, png, (UINT)len, (UINT)w, (UINT)h, 1, 0,
                                                     D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                     D3DX_FILTER_NONE, D3DX_FILTER_NONE,
                                                     0, NULL, NULL, &tex);
    if (FAILED(hr))
    {
        if (!g_assetErr[0])
            _snprintf(g_assetErr, sizeof(g_assetErr) - 1, "%s no cargo (hr 0x%08lX)", what, (unsigned long)hr);
        log_msg("[gfx] ");
        log_msg(g_assetErr);
        log_msg("\n");
        return NULL;
    }
    return tex;
}

// Un tamano de Roboto: textura y tabla codepoint -> letra
static int load_ui_font(UiFont *f, const UiFontDesc *d)
{
    int j;

    f->d = d;
    f->tex = load_png(d->png, d->pngLen, d->texW, d->texH, "La letra");
    if (!f->tex)
        return 0;
    for (j = 0; j < 256; ++j)
        f->map[j] = -1;
    for (j = 0; j < d->count; ++j)
        if (d->glyphs[j].cp < 256)
            f->map[d->glyphs[j].cp] = (short)j;
    f->lineH = (float)(d->ascent + d->descent);
    f->whiteU = 4.0f / d->texW;   // el bloque blanco va en (1,1)-(7,7)
    f->whiteV = 4.0f / d->texH;
    return 1;
}

static void load_ui_assets(void)
{
    static const UiFontDesc *descs[3] = { &g_font20, &g_font24, &g_font40 };
    int i;

    g_uiCount = 0;
    for (i = 0; i < 3; ++i)
    {
        if (!load_ui_font(&g_ui[g_uiCount], descs[i]))
            break;   // sin los 3 tamanos se queda la monoespaciada
        ++g_uiCount;
    }
    if (g_uiCount < 3)
    {
        for (i = 0; i < g_uiCount; ++i)
            g_ui[i].tex->Release();
        g_uiCount = 0;
    }
    // la Bold es opcional: sin ella los titulos salen con la normal
    g_boldOk = g_uiCount == 3 && load_ui_font(&g_uiBold, &g_font32b);
    g_icons = load_png(g_icons_png, g_icons_png_len, g_icons_w, g_icons_h, "Los iconos");
    g_logo = load_png(g_logo_png, g_logo_png_len, g_logo_w, g_logo_h, "El logo");
    g_logoBig = load_png(g_logo_big_png, g_logo_big_png_len, g_logo_big_w, g_logo_big_h, "El logo grande");
}

// --- Imagenes (paso 5a: miniaturas) ---------------------------------

// Crea una textura a partir de un archivo de imagen en memoria (JPEG, PNG,
// BMP...). D3DX la decodifica y la escala a w x h. Sin mipmaps: las
// miniaturas se dibujan casi a su tamano. Llamar solo desde el hilo
// principal (el mismo que dibuja).
extern "C" void *gfx_texture_load(const void *data, int len, int w, int h, long *hrOut)
{
    IDirect3DTexture9 *tex = NULL;
    HRESULT hr;

    hr = D3DXCreateTextureFromFileInMemoryEx(g_dev, data, (UINT)len,
                                             (UINT)w, (UINT)h, 1, 0,
                                             D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                             D3DX_FILTER_LINEAR, D3DX_FILTER_NONE,
                                             0, NULL, NULL, &tex);
    if (hrOut)
        *hrOut = (long)hr;
    if (FAILED(hr))
        return NULL;
    return tex;
}

// La GPU puede ir hasta 2 cuadros atrasada: quien llama se asegura de que
// la textura no se dibujo en los ultimos cuadros (thumb.c espera 4).
// Textura desde pixeles 0xAARRGGBB ya en memoria (la foto de jugador).
// Formato lineal, como el atlas de la letra: se copia fila por fila.
extern "C" void *gfx_texture_from_argb(const unsigned int *px, int w, int h)
{
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT lr;
    int y;

    if (!g_dev || !px || w <= 0 || h <= 0)
        return NULL;
    if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)))
        return NULL;
    if (FAILED(t->LockRect(0, &lr, NULL, 0)))
    {
        t->Release();
        return NULL;
    }
    for (y = 0; y < h; ++y)
        memcpy((unsigned char *)lr.pBits + y * lr.Pitch, px + y * w, w * 4);
    t->UnlockRect(0);
    return t;
}

extern "C" void gfx_texture_free(void *tex)
{
    if (tex)
        ((IDirect3DTexture9 *)tex)->Release();
}

extern "C" void gfx_image(void *tex, float x, float y, float w, float h, unsigned int color)
{
    if (!tex)
        return;
    bind((IDirect3DTexture9 *)tex);
    push_quad(x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, color);
}

extern "C" void gfx_image_uv(void *tex, float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1, unsigned int color)
{
    if (!tex)
        return;
    bind((IDirect3DTexture9 *)tex);
    push_quad(x, y, x + w, y + h, u0, v0, u1, v1, color);
}

extern "C" void gfx_image_circle(void *tex, float x, float y, float size, unsigned int color)
{
    float edge[4];

    if (!tex)
        return;
    if (!g_psCircle)
    {
        gfx_image(tex, x, y, size, size, color);
        return;
    }
    flush();
    edge[0] = size > 2.0f ? 1.5f / size : 0.5f;   // borde suave de ~1.5 pixeles
    edge[1] = edge[2] = edge[3] = 0.0f;
    g_dev->SetPixelShader(g_psCircle);
    g_dev->SetPixelShaderConstantF(0, edge, 1);
    bind((IDirect3DTexture9 *)tex);
    push_quad(x, y, x + size, y + size, 0.0f, 0.0f, 1.0f, 1.0f, color);
    flush();
    g_dev->SetPixelShader(g_ps);
}

extern "C" void gfx_clip(float x, float y, float w, float h)
{
    RECT r;

    flush();
    r.left = (LONG)x;
    r.top = (LONG)y;
    r.right = (LONG)(x + w);
    r.bottom = (LONG)(y + h);
    g_dev->SetScissorRect(&r);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

extern "C" void gfx_clip_off(void)
{
    flush();
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}

extern "C" void gfx_image_ellipse(void *tex, float x, float y, float w, float h, unsigned int color)
{
    float edge[4], m = w < h ? w : h;

    if (!tex || w <= 0.0f || h <= 0.0f)
        return;
    if (!g_psCircle)
    {
        gfx_image(tex, x, y, w, h, color);
        return;
    }
    flush();
    edge[0] = m > 2.0f ? 1.5f / m : 0.5f;
    edge[1] = edge[2] = edge[3] = 0.0f;
    g_dev->SetPixelShader(g_psCircle);
    g_dev->SetPixelShaderConstantF(0, edge, 1);
    bind((IDirect3DTexture9 *)tex);
    push_quad(x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, color);
    flush();
    g_dev->SetPixelShader(g_ps);
}

extern "C" void gfx_ellipse(float x, float y, float w, float h, unsigned int color)
{
    if (g_white && g_psCircle)
        gfx_image_ellipse(g_white, x, y, w, h, color);
    else
        gfx_rect(x, y, w, h, color);
}

extern "C" void gfx_circle(float x, float y, float size, unsigned int color)
{
    if (g_white && g_psCircle)
        gfx_image_circle(g_white, x, y, size, color);
    else
        gfx_rect(x, y, size, size, color);
}

// Cuadrilatero de esquinas libres (para los arcos): p0-p1 arriba, p2-p3
// abajo, cada esquina con su color
static void push_quad4(const float *xs, const float *ys, const unsigned int *cs, float u, float v)
{
    Vertex *vt;
    int i;
    static const int order[6] = { 0, 1, 2, 2, 1, 3 };

    if (g_count + 6 > MAX_VERTS)
        flush();
    vt = &g_verts[g_count];
    for (i = 0; i < 6; ++i)
    {
        int k = order[i];
        unsigned int c = cs[k];
        set_vertex(&vt[i], xs[k], ys[k], u, v,
                   ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                   (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
    }
    g_count += 6;
}

static unsigned int lerp_color(unsigned int a, unsigned int b, float t)
{
    unsigned int out = 0;
    int sh;

    for (sh = 0; sh < 32; sh += 8)
    {
        float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= ((unsigned int)(ca + (cb - ca) * t + 0.5f) & 0xFF) << sh;
    }
    return out;
}

extern "C" void gfx_arc(float cx, float cy, float radius, float thick, float a0, float a1,
                        unsigned int c0, unsigned int c1)
{
    // 4 anillos por angulo: orilla de adentro (transparente), trazo, trazo,
    // orilla de afuera (transparente). El GPU degrada el color entre ellos y
    // eso da la orilla suave de ~1 pixel.
    const float feather = 1.0f;
    float rr[4], u, v, span = a1 - a0;
    int n, i, j;

    rr[0] = radius - thick * 0.5f - feather;
    rr[1] = radius - thick * 0.5f;
    rr[2] = radius + thick * 0.5f;
    rr[3] = radius + thick * 0.5f + feather;
    if (rr[0] < 0.0f)
        rr[0] = 0.0f;
    n = (int)(fabsf(span) * radius / 3.0f) + 2;   // un tramo cada ~3 pixeles
    if (n > 180)
        n = 180;
    white_uv(&u, &v);
    for (i = 0; i < n; ++i)
    {
        float t0 = (float)i / n, t1 = (float)(i + 1) / n;
        float ang0 = a0 + span * t0, ang1 = a0 + span * t1;
        float cs0 = cosf(ang0), sn0 = sinf(ang0), cs1 = cosf(ang1), sn1 = sinf(ang1);
        unsigned int k0 = lerp_color(c0, c1, t0), k1 = lerp_color(c0, c1, t1);
        unsigned int col[4][2];

        col[0][0] = k0 & 0x00FFFFFF;  col[0][1] = k1 & 0x00FFFFFF;
        col[1][0] = k0;               col[1][1] = k1;
        col[2][0] = k0;               col[2][1] = k1;
        col[3][0] = k0 & 0x00FFFFFF;  col[3][1] = k1 & 0x00FFFFFF;
        for (j = 0; j < 3; ++j)
        {
            float xs[4], ys[4];
            unsigned int cc[4];
            xs[0] = cx + cs0 * rr[j];     ys[0] = cy + sn0 * rr[j];     cc[0] = col[j][0];
            xs[1] = cx + cs1 * rr[j];     ys[1] = cy + sn1 * rr[j];     cc[1] = col[j][1];
            xs[2] = cx + cs0 * rr[j + 1]; ys[2] = cy + sn0 * rr[j + 1]; cc[2] = col[j + 1][0];
            xs[3] = cx + cs1 * rr[j + 1]; ys[3] = cy + sn1 * rr[j + 1]; cc[3] = col[j + 1][1];
            push_quad4(xs, ys, cc, u, v);
        }
    }
}

extern "C" void *gfx_device(void)
{
    return g_dev;
}

// --- 3D en un rectangulo (avatar) -------------------------------------

static IDirect3DSurface9 *g_depth = NULL;
static int                g_depthTried = 0;

extern "C" int gfx_begin_3d(float x, float y, float w, float h)
{
    D3DVIEWPORT9 vp;

    flush();
    if (!g_depth && !g_depthTried)
    {
        D3DSURFACE_PARAMETERS sp;

        g_depthTried = 1;
        ZeroMemory(&sp, sizeof(sp));
        // En la EDRAM la pantalla (1280x720, 32 bits, sin MSAA) ocupa
        // 16 x 45 = 720 bloques de 80x16 pixeles; la profundidad va despues.
        sp.Base = ((g_pp.BackBufferWidth + 79) / 80) * ((g_pp.BackBufferHeight + 15) / 16);
        sp.HierarchicalZBase = 0;
        if (FAILED(g_dev->CreateDepthStencilSurface(g_pp.BackBufferWidth, g_pp.BackBufferHeight, D3DFMT_D24S8,
                                                    D3DMULTISAMPLE_NONE, 0, FALSE, &g_depth, &sp)))
        {
            log_msg("[gfx] No se pudo crear el buffer de profundidad (avatar 3D)\n");
            g_depth = NULL;
        }
    }
    if (!g_depth)
        return 0;
    if (x < 0.0f) { w += x; x = 0.0f; }
    if (y < 0.0f) { h += y; y = 0.0f; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w < 8.0f || h < 8.0f)
        return 0;
    g_dev->SetDepthStencilSurface(g_depth);
    vp.X = (DWORD)x;
    vp.Y = (DWORD)y;
    vp.Width = (DWORD)w;
    vp.Height = (DWORD)h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    g_dev->SetViewport(&vp);
    g_dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    return 1;
}

extern "C" void gfx_end_3d(void)
{
    D3DVIEWPORT9 vp;

    vp.X = 0;
    vp.Y = 0;
    vp.Width = SCREEN_W;
    vp.Height = SCREEN_H;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    g_dev->SetViewport(&vp);
    g_dev->SetDepthStencilSurface(NULL);
    // lo que el renderer del avatar pudo haber cambiado
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    gfx_reset_state();     // shaders, texturas, mezcla y la constante de pantalla de gfx
    g_count = 0;
}

// --- 3D con antialias (avatar): superficie aparte con MSAA -----------

static IDirect3DSurface9 *g_aaRT = NULL, *g_aaDepth = NULL, *g_bbRT = NULL;
static IDirect3DTexture9 *g_aaTex = NULL;
static int                g_aaW = 0, g_aaH = 0, g_aaTried = 0, g_aaSamples = 0;

#define EDRAM_TILES 2048    // 10 MB de EDRAM en bloques de 5120 bytes

static void restore_states_after_3d(void)
{
    D3DVIEWPORT9 vp;

    vp.X = 0;
    vp.Y = 0;
    vp.Width = SCREEN_W;
    vp.Height = SCREEN_H;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    g_dev->SetViewport(&vp);
    g_dev->SetDepthStencilSurface(NULL);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    gfx_reset_state();
    g_count = 0;
}

static int create_aa(int w, int h)
{
    static const D3DMULTISAMPLE_TYPE kMs[2] = { D3DMULTISAMPLE_4_SAMPLES, D3DMULTISAMPLE_2_SAMPLES };
    D3DSURFACE_PARAMETERS sp;
    UINT bb, ct, dt;
    int i;
    char msg[128];

    bb = XGSurfaceSize(g_pp.BackBufferWidth, g_pp.BackBufferHeight, g_pp.BackBufferFormat, D3DMULTISAMPLE_NONE);
    for (i = 0; i < 2; ++i)
    {
        ct = XGSurfaceSize(w, h, D3DFMT_A8R8G8B8, kMs[i]);
        dt = XGSurfaceSize(w, h, D3DFMT_D24S8, kMs[i]);
        if (bb + ct + dt > EDRAM_TILES)
            continue;
        ZeroMemory(&sp, sizeof(sp));
        sp.Base = bb;                       // despues de la pantalla
        if (FAILED(g_dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, kMs[i], 0, FALSE, &g_aaRT, &sp)))
        {
            g_aaRT = NULL;
            continue;
        }
        ZeroMemory(&sp, sizeof(sp));
        sp.Base = bb + ct;                  // y la profundidad despues del color
        if (FAILED(g_dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, kMs[i], 0, FALSE, &g_aaDepth, &sp)))
        {
            g_aaRT->Release();
            g_aaRT = NULL;
            g_aaDepth = NULL;
            continue;
        }
        if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, 0, &g_aaTex, NULL)))
        {
            g_aaRT->Release();
            g_aaDepth->Release();
            g_aaRT = NULL;
            g_aaDepth = NULL;
            g_aaTex = NULL;
            return 0;
        }
        g_aaSamples = i == 0 ? 4 : 2;
        g_aaW = w;
        g_aaH = h;
        _snprintf(msg, sizeof(msg) - 1, "[gfx] 3D con antialias %dx: %dx%d (EDRAM %u+%u+%u de %d)\n",
                  g_aaSamples, w, h, bb, ct, dt, EDRAM_TILES);
        msg[sizeof(msg) - 1] = 0;
        log_msg(msg);
        return 1;
    }
    log_msg("[gfx] No se pudo crear la superficie con antialias\n");
    return 0;
}

extern "C" int gfx_aa_samples(void)
{
    return g_aaSamples;
}

extern "C" int gfx_begin_3d_aa(int w, int h)
{
    D3DVIEWPORT9 vp;

    flush();
    if (!g_aaTex && !g_aaTried)
    {
        g_aaTried = 1;
        if (!create_aa(w, h))
            return 0;
    }
    if (!g_aaTex || w != g_aaW || h != g_aaH)
        return 0;
    if (!g_bbRT)
        g_dev->GetRenderTarget(0, &g_bbRT);
    if (!g_bbRT)
        return 0;
    g_dev->SetRenderTarget(0, g_aaRT);
    g_dev->SetDepthStencilSurface(g_aaDepth);
    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)w;
    vp.Height = (DWORD)h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    g_dev->SetViewport(&vp);
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x00000000, 1.0f, 0);
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    return 1;
}

extern "C" void *gfx_end_3d_aa(void)
{
    // la resolucion promedia las muestras del MSAA: orillas suaves y el alfa
    // queda como la parte del pixel que cubre el avatar
    g_dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, g_aaTex, NULL, 0, 0, NULL, 1.0f, 0, NULL);
    g_dev->SetRenderTarget(0, g_bbRT);
    restore_states_after_3d();
    return g_aaTex;
}

extern "C" void gfx_image_premul(void *tex, float x, float y, float w, float h)
{
    if (!tex)
        return;
    flush();
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    bind((IDirect3DTexture9 *)tex);
    push_quad(x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
    flush();
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
}

extern "C" void gfx_texture_count(void *tex, int w, int h, int *withAlpha, int *withColor)
{
    D3DLOCKED_RECT lr;
    int x, y;

    *withAlpha = *withColor = 0;
    if (!tex)
        return;
    g_dev->BlockUntilIdle();
    if (FAILED(((IDirect3DTexture9 *)tex)->LockRect(0, &lr, NULL, D3DLOCK_READONLY)))
        return;
    // la textura esta en mosaico (tiled): el orden no importa para contar
    for (y = 0; y < h; ++y)
    {
        const DWORD *row = (const DWORD *)((const BYTE *)lr.pBits + y * lr.Pitch);
        for (x = 0; x < w; ++x)
        {
            if ((row[x] >> 24) > 0x80)
                ++*withAlpha;
            if (row[x] & 0x00FFFFFF)
                ++*withColor;
        }
    }
    ((IDirect3DTexture9 *)tex)->UnlockRect(0);
}

extern "C" void *gfx_present_params(void)
{
    return &g_pp;
}

extern "C" int gfx_set_swap_mode(int on)
{
    int i;

    if (on && g_frontCount < 0)
    {
        // 3 buffers: uno en pantalla, uno esperando y uno donde copiamos
        g_frontCount = 3;
        for (i = 0; i < 3; ++i)
        {
            if (FAILED(g_dev->CreateTexture(g_pp.BackBufferWidth, g_pp.BackBufferHeight, 1, 0,
                                            D3DFMT_LE_X8R8G8B8, 0, &g_fronts[i], NULL)))
            {
                log_msg("[gfx] No se pudieron crear los buffers propios; se usa el de D3D\n");
                g_frontCount = 0;
                break;
            }
        }
    }
    if (on && !g_vblankEv)
    {
        g_vblankEv = CreateEvent(NULL, FALSE, FALSE, NULL);
        g_dev->SetVerticalBlankCallback(on_vblank);
    }
    g_swap = on ? 1 : 0;
    return g_frontCount;
}

extern "C" long gfx_vblank_count(void)
{
    return g_vblanks;
}

extern "C" void gfx_end(void)
{
    IDirect3DTexture9 *front = NULL;

    flush();
    g_dev->EndScene();

    if (!g_swap)
    {
        g_dev->Present(NULL, NULL, NULL, NULL);
        return;
    }

    if (g_frontCount > 0)
    {
        front = g_fronts[g_frontNext];
        front->AddRef();
        g_frontNext = (g_frontNext + 1) % g_frontCount;
    }
    else
        g_dev->GetFrontBuffer(&front);

    g_dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, front, NULL, 0, 0, NULL, 1.0f, 0, NULL);
    WaitForSingleObject(g_vblankEv, 50);   // como mucho 50 ms
    g_dev->Swap(front, NULL);
    front->Release();
}
