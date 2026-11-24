# gen_assets.py - Genera ui_assets.c para TubeBox360:
#   - Letra Roboto (SIL Open Font License 1.1) en 3 tamanos normales y uno
#     Light (titulo del reproductor), como atlas PNG
#     (blanco con alfa) + tabla de medidas por letra.
#   - Iconos estilo "Frutiger Aero" (blancos brillantes, orbe rojo con
#     reflejo para el elegido), dibujados aqui a 4x y reducidos.
#   - El logo del usuario recortado al tamano del encabezado.
# Uso: python3 -I gen_assets.py <salida.c>
import io, math, os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

ROBOTO = '/usr/share/fonts/truetype/google-fonts/ofl/roboto/Roboto[wdth,wght].ttf'
LOGO = '/mnt/user-data/uploads/71a71536-image.png'   # logo corregido (2026-11-21)
SHADOW = '/mnt/user-data/uploads/2e86a08d-image.png'   # sombra degradada del usuario (2026-11-22)
DP_LOGO = '/mnt/user-data/uploads/2f5ecdfe-image.png'  # logo de DeadPIayers (2026-11-22), ya trae alfa

# ---------------------------------------------------------------- letra
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2122]
CODEPOINTS = list(range(32, 127)) + list(range(160, 256)) + EXTRA

def roboto(size, weight=400):
    f = ImageFont.truetype(ROBOTO, size)
    f.set_variation_by_axes([weight, 100])   # [peso, ancho]
    return f

def make_font_atlas(size, weight, atlas_w):
    f = roboto(size, weight)
    asc, desc = f.getmetrics()
    glyphs = []
    for cp in CODEPOINTS:
        ch = chr(cp)
        adv = f.getlength(ch)
        if cp in (32, 160):
            glyphs.append((cp, None, 0, 0, adv))
            continue
        mask, (ox, oy) = f.getmask2(ch, mode='L', anchor='ls')
        im = Image.frombytes('L', mask.size, bytes(mask)) if mask.size[0] > 0 else None
        glyphs.append((cp, im, ox, oy, adv))
    # empaquetar por filas (las mas altas primero da filas parejas)
    pad = 2
    order = sorted(range(len(glyphs)), key=lambda i: -(glyphs[i][1].size[1] if glyphs[i][1] else 0))
    x, y, rowh = 8, pad, 0          # (0..7, 0..7) queda para el bloque blanco
    pos = {}
    for i in order:
        im = glyphs[i][1]
        if im is None:
            continue
        w, h = im.size
        if x + w + pad > atlas_w:
            x = pad
            y += rowh + pad
            rowh = 0
        pos[i] = (x, y)
        x += w + pad
        rowh = max(rowh, h)
    atlas_h = y + rowh + pad
    atlas_h = (atlas_h + 3) // 4 * 4
    alpha = Image.new('L', (atlas_w, atlas_h), 0)
    for i, (px, py) in pos.items():
        alpha.paste(glyphs[i][1], (px, py))
    alpha.paste(255, (1, 1, 7, 7))   # bloque blanco para rectangulos
    rgba = Image.merge('RGBA', [Image.new('L', alpha.size, 255)] * 3 + [alpha])
    table = []
    for i, (cp, im, ox, oy, adv) in enumerate(glyphs):
        if im is None:
            table.append((cp, 0, 0, 0, 0, 0, 0, adv))
        else:
            px, py = pos[i]
            table.append((cp, px, py, im.size[0], im.size[1], ox, oy, adv))
    return rgba, table, asc, desc

# ---------------------------------------------------------------- iconos
S = 4  # se dibuja a 4x y se reduce

def vgrad(size, top, bottom):
    w, h = size
    g = Image.new('RGBA', size)
    d = ImageDraw.Draw(g)
    for yy in range(h):
        t = yy / max(1, h - 1)
        c = tuple(int(top[k] + (bottom[k] - top[k]) * t) for k in range(4))
        d.line([(0, yy), (w, yy)], fill=c)
    return g

def glossy_white(mask, accent=None):
    """Cuerpo blanco con degradado, brillo arriba y sombra suave."""
    size = mask.size
    out = Image.new('RGBA', size, (0, 0, 0, 0))
    # sombra
    sh = mask.filter(ImageFilter.GaussianBlur(2.2 * S))
    sh = ImageChops.offset(sh, 0, int(1.5 * S))
    shadow = Image.new('RGBA', size, (0, 0, 0, 0))
    shadow.putalpha(sh.point(lambda v: int(v * 0.65)))
    out.alpha_composite(shadow)
    # cuerpo
    body = vgrad(size, (255, 255, 255, 255), (214, 216, 220, 255))
    body.putalpha(mask)
    out.alpha_composite(body)
    # brillo en la mitad de arriba
    hl = Image.new('L', size, 0)
    ImageDraw.Draw(hl).ellipse([-size[0] * 0.3, -size[1] * 0.55, size[0] * 1.3, size[1] * 0.48], fill=90)
    hl = ImageChops.multiply(hl, mask)
    shine = Image.new('RGBA', size, (255, 255, 255, 0))
    shine.putalpha(hl)
    out.alpha_composite(shine)
    if accent is not None:
        out.alpha_composite(accent)
    return out

def new_mask(cell):
    return Image.new('L', (cell * S, cell * S), 0)

def thick_arc(d, box, start, end, width):
    d.arc(box, start, end, fill=255, width=width)

def text_mask(m, text, size, center, weight=900):
    f = roboto(size, weight)
    d = ImageDraw.Draw(m)
    d.text(center, text, font=f, fill=255, anchor='mm')

def icon_play(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.polygon([(c * 0.34, c * 0.22), (c * 0.34, c * 0.78), (c * 0.80, c * 0.50)], fill=255)
    return glossy_white(m)

def icon_pause(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    r = int(c * 0.04)
    d.rounded_rectangle([c * 0.28, c * 0.22, c * 0.43, c * 0.78], r, fill=255)
    d.rounded_rectangle([c * 0.57, c * 0.22, c * 0.72, c * 0.78], r, fill=255)
    return glossy_white(m)

def icon_seek(cell, forward):
    """Flecha circular con "10": a favor del reloj (adelantar) o en contra
    (retroceder, la misma flecha en espejo)."""
    from PIL import ImageOps
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    w = int(c * 0.075)
    r = c * 0.30
    cx = cy = c * 0.5
    box = [cx - r, cy - r, cx + r, cy + r]
    d.arc(box, -50, 262, fill=255, width=w)        # hueco arriba a la derecha
    th = math.radians(262)                          # punta: llega por arriba yendo a la derecha
    p = (cx + (r - w / 2) * math.cos(th), cy + (r - w / 2) * math.sin(th))
    t = (-math.sin(th), math.cos(th))               # direccion del giro
    n = (math.cos(th), math.sin(th))                # hacia afuera
    L, W = c * 0.17, c * 0.13
    tip = (p[0] + t[0] * L * 0.75, p[1] + t[1] * L * 0.75)
    b1 = (p[0] - t[0] * L * 0.25 + n[0] * W, p[1] - t[1] * L * 0.25 + n[1] * W)
    b2 = (p[0] - t[0] * L * 0.25 - n[0] * W, p[1] - t[1] * L * 0.25 - n[1] * W)
    d.polygon([tip, b1, b2], fill=255)
    if not forward:
        m = ImageOps.mirror(m)
    text_mask(m, '10', int(c * 0.25), (c * 0.5, c * 0.53), 800)
    return glossy_white(m)

def icon_close(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    w = int(c * 0.065)
    d.rounded_rectangle([c * 0.20, c * 0.24, c * 0.80, c * 0.76], int(c * 0.06), outline=255, width=w)
    k = c * 0.12
    d.line([(c * 0.5 - k, c * 0.5 - k), (c * 0.5 + k, c * 0.5 + k)], fill=255, width=w)
    d.line([(c * 0.5 - k, c * 0.5 + k), (c * 0.5 + k, c * 0.5 - k)], fill=255, width=w)
    return glossy_white(m)

def icon_badge(cell, text):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.rounded_rectangle([c * 0.16, c * 0.26, c * 0.84, c * 0.74], int(c * 0.09), fill=255)
    out = glossy_white(m)
    # letras rojas con un poco de degradado
    lm = new_mask(cell)
    text_mask(lm, text, int(c * 0.30), (c * 0.5, c * 0.505), 900)
    letters = vgrad(lm.size, (236, 40, 32, 255), (176, 16, 12, 255))
    letters.putalpha(lm)
    out.alpha_composite(letters)
    return out

def icon_globe(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    box = [c * 0.17, c * 0.17, c * 0.83, c * 0.83]
    d.ellipse(box, fill=255)
    out = glossy_white(m)
    # meridianos y paralelos rojos
    lines = Image.new('L', m.size, 0)
    dl = ImageDraw.Draw(lines)
    lw = max(2, int(c * 0.035))
    dl.ellipse(box, outline=255, width=lw)
    dl.ellipse([c * 0.36, c * 0.17, c * 0.64, c * 0.83], outline=255, width=lw)
    dl.line([(c * 0.5, c * 0.17), (c * 0.5, c * 0.83)], fill=255, width=lw)
    dl.line([(c * 0.17, c * 0.5), (c * 0.83, c * 0.5)], fill=255, width=lw)
    dl.arc([c * 0.22, c * 0.06, c * 0.78, c * 0.40], 30, 150, fill=255, width=lw)
    dl.arc([c * 0.22, c * 0.60, c * 0.78, c * 0.94], 210, 330, fill=255, width=lw)
    lines = ImageChops.multiply(lines, m)
    red = vgrad(m.size, (232, 38, 30, 255), (170, 14, 10, 255))
    red.putalpha(lines)
    out.alpha_composite(red)
    # reflejo
    hl = Image.new('L', m.size, 0)
    ImageDraw.Draw(hl).ellipse([c * 0.27, c * 0.21, c * 0.55, c * 0.40], fill=120)
    hl = hl.filter(ImageFilter.GaussianBlur(1.5 * S))
    shine = Image.new('RGBA', m.size, (255, 255, 255, 0))
    shine.putalpha(hl)
    out.alpha_composite(shine)
    return out

def icon_info(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    w = int(c * 0.07)
    d.ellipse([c * 0.18, c * 0.18, c * 0.82, c * 0.82], outline=255, width=w)
    d.ellipse([c * 0.455, c * 0.30, c * 0.545, c * 0.39], fill=255)
    d.rounded_rectangle([c * 0.458, c * 0.44, c * 0.542, c * 0.70], int(c * 0.02), fill=255)
    return glossy_white(m)

def icon_sun(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.ellipse([c * 0.33, c * 0.33, c * 0.67, c * 0.67], fill=255)
    w = int(c * 0.07)
    for k in range(8):
        a = math.radians(k * 45)
        r0, r1 = c * 0.25, c * 0.37
        d.line([(c * 0.5 + r0 * math.cos(a), c * 0.5 + r0 * math.sin(a)),
                (c * 0.5 + r1 * math.cos(a), c * 0.5 + r1 * math.sin(a))], fill=255, width=w)
    return glossy_white(m)

def icon_moon(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.ellipse([c * 0.22, c * 0.20, c * 0.78, c * 0.76], fill=255)
    d.ellipse([c * 0.38, c * 0.10, c * 0.92, c * 0.62], fill=0)
    return glossy_white(m)

def icon_check(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    w = int(c * 0.10)
    d.line([(c * 0.22, c * 0.52), (c * 0.42, c * 0.72), (c * 0.80, c * 0.30)], fill=255, width=w, joint='curve')
    return glossy_white(m)

def icon_search(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    w = int(c * 0.08)
    d.ellipse([c * 0.20, c * 0.20, c * 0.60, c * 0.60], outline=255, width=w)
    d.line([(c * 0.55, c * 0.55), (c * 0.80, c * 0.80)], fill=255, width=int(c * 0.11))
    return glossy_white(m)

def icon_hourglass(cell):
    """Reloj de arena: marco blanco brillante, vidrio claro y arena roja."""
    c = cell * S
    frame = new_mask(cell); d = ImageDraw.Draw(frame)
    r = int(c * 0.03)
    d.rounded_rectangle([c * 0.20, c * 0.08, c * 0.80, c * 0.19], r, fill=255)
    d.rounded_rectangle([c * 0.20, c * 0.81, c * 0.80, c * 0.92], r, fill=255)
    glass = [(c * 0.27, c * 0.19), (c * 0.73, c * 0.19), (c * 0.535, c * 0.50),
             (c * 0.73, c * 0.81), (c * 0.27, c * 0.81), (c * 0.465, c * 0.50)]
    d.line(glass + [glass[0]], fill=255, width=int(c * 0.06), joint='curve')
    out = glossy_white(frame)
    # vidrio: blanco muy tenue por dentro
    inner = new_mask(cell)
    ImageDraw.Draw(inner).polygon(glass, fill=70)
    tint = Image.new('RGBA', inner.size, (255, 255, 255, 0))
    tint.putalpha(inner)
    out.alpha_composite(tint)
    # arena roja: un poco arriba y el monticulo abajo
    sand = new_mask(cell); ds = ImageDraw.Draw(sand)
    ds.polygon([(c * 0.36, c * 0.31), (c * 0.64, c * 0.31), (c * 0.5, c * 0.47)], fill=255)
    ds.polygon([(c * 0.31, c * 0.77), (c * 0.69, c * 0.77), (c * 0.5, c * 0.60)], fill=255)
    ds.line([(c * 0.5, c * 0.47), (c * 0.5, c * 0.66)], fill=255, width=int(c * 0.025))
    red = vgrad(sand.size, (240, 52, 40, 255), (176, 16, 12, 255))
    red.putalpha(sand)
    out.alpha_composite(red)
    return out

# ------------------------------------------------- iconos planos (menu)
# Blancos y planos: se pueden tenir (rojo el elegido sobre la pastilla).
def flat(mask):
    # 2026-11-22: el usuario los quiere estilo Frutiger Aero (blancos con
    # brillo), como los del reproductor; siguen pudiendose tenir de rojo
    return glossy_white(mask)

def m_search(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.ellipse([c * 0.16, c * 0.16, c * 0.62, c * 0.62], outline=255, width=int(c * 0.10))
    d.line([(c * 0.56, c * 0.56), (c * 0.84, c * 0.84)], fill=255, width=int(c * 0.12))
    return flat(m)

def m_home(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.polygon([(c * 0.50, c * 0.12), (c * 0.90, c * 0.48), (c * 0.10, c * 0.48)], fill=255)
    d.rectangle([c * 0.22, c * 0.44, c * 0.78, c * 0.86], fill=255)
    d.rectangle([c * 0.42, c * 0.60, c * 0.58, c * 0.86], fill=0)
    return flat(m)

def m_gaming(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.rounded_rectangle([c * 0.10, c * 0.30, c * 0.90, c * 0.66], int(c * 0.16), fill=255)
    d.ellipse([c * 0.06, c * 0.40, c * 0.40, c * 0.86], fill=255)
    d.ellipse([c * 0.60, c * 0.40, c * 0.94, c * 0.86], fill=255)
    w = c * 0.05
    d.rectangle([c * 0.27 - w, c * 0.38, c * 0.27 + w, c * 0.62], fill=0)       # cruceta
    d.rectangle([c * 0.15, c * 0.50 - w, c * 0.39, c * 0.50 + w], fill=0)
    r = c * 0.055
    for bx, by in ((0.70, 0.42), (0.78, 0.52), (0.62, 0.52), (0.70, 0.62)):    # botones
        d.ellipse([c * bx - r, c * by - r, c * bx + r, c * by + r], fill=0)
    return flat(m)

def m_music(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.polygon([(c * 0.36, c * 0.18), (c * 0.84, c * 0.10), (c * 0.84, c * 0.24), (c * 0.36, c * 0.32)], fill=255)
    d.rectangle([c * 0.36, c * 0.20, c * 0.44, c * 0.74], fill=255)
    d.rectangle([c * 0.76, c * 0.12, c * 0.84, c * 0.66], fill=255)
    d.ellipse([c * 0.14, c * 0.62, c * 0.44, c * 0.86], fill=255)
    d.ellipse([c * 0.54, c * 0.54, c * 0.84, c * 0.78], fill=255)
    return flat(m)

def m_sound(cell):
    """Bocina con dos ondas (sonidos de la interfaz)."""
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.polygon([(c * 0.10, c * 0.38), (c * 0.26, c * 0.38), (c * 0.48, c * 0.18), (c * 0.48, c * 0.82),
               (c * 0.26, c * 0.62), (c * 0.10, c * 0.62)], fill=255)
    w = int(c * 0.07)
    thick_arc(d, [c * 0.40, c * 0.30, c * 0.70, c * 0.70], -50, 50, w)
    thick_arc(d, [c * 0.36, c * 0.14, c * 0.90, c * 0.86], -50, 50, w)
    return flat(m)

def m_shorts(cell):
    """Shorts: rectangulo vertical redondeado (como un telefono) con un
    triangulo de play hueco adentro. Dibujo generico propio."""
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.rounded_rectangle([c * 0.24, c * 0.08, c * 0.76, c * 0.92], int(c * 0.14), fill=255)
    d.polygon([(c * 0.42, c * 0.34), (c * 0.64, c * 0.50), (c * 0.42, c * 0.66)], fill=0)
    return flat(m)

def icon_update(cell):
    """Actualizacion: circulo con flecha hacia abajo y raya (descargar)."""
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.ellipse([c * 0.06, c * 0.06, c * 0.94, c * 0.94], fill=255)
    d.rectangle([c * 0.43, c * 0.20, c * 0.57, c * 0.52], fill=0)
    d.polygon([(c * 0.28, c * 0.46), (c * 0.72, c * 0.46), (c * 0.50, c * 0.70)], fill=0)
    d.rectangle([c * 0.28, c * 0.74, c * 0.72, c * 0.81], fill=0)
    return flat(m)

XBOX_LOGO = '/home/claude/assets/xbox360_logo.png'   # lo envio el usuario (2026-11-23)

def icon_xbox(cell):
    """Logo de la consola (imagen del usuario, a color; no se tine)."""
    im = Image.open(XBOX_LOGO).convert('RGBA')
    bb = im.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox()
    if bb:
        im = im.crop(bb)
    side = max(im.size)
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.paste(im, ((side - im.size[0]) // 2, (side - im.size[1]) // 2))
    return sq.resize((cell * S, cell * S), Image.LANCZOS)

def star_poly(c, cx, cy, ro, ri):
    import math
    pts = []
    for k in range(10):
        r = ro if k % 2 == 0 else ri
        a = -math.pi / 2 + k * math.pi / 5
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

def m_star(cell):
    """Menu: Recomendados (estrella)."""
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.polygon(star_poly(c, c * 0.5, c * 0.54, c * 0.46, c * 0.19), fill=255)
    return flat(m)

def icon_star(cell, filled):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.polygon(star_poly(c, c * 0.5, c * 0.53, c * 0.36, c * 0.15), fill=255)
    if not filled:
        inner = m.filter(ImageFilter.MinFilter(int(cell * S * 0.085) | 1))
        m = ImageChops.subtract(m, inner)
    return glossy_white(m)

def m_subs(cell):
    """Suscripciones: una pantalla con play y dos rayas encima (videos
    apilados). Dibujo generico propio."""
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.rounded_rectangle([c * 0.26, c * 0.10, c * 0.74, c * 0.17], int(c * 0.03), fill=255)
    d.rounded_rectangle([c * 0.18, c * 0.23, c * 0.82, c * 0.30], int(c * 0.03), fill=255)
    d.rounded_rectangle([c * 0.10, c * 0.36, c * 0.90, c * 0.90], int(c * 0.08), fill=255)
    d.polygon([(c * 0.42, c * 0.48), (c * 0.62, c * 0.63), (c * 0.42, c * 0.78)], fill=0)
    return flat(m)

def thumb_mask(cell, down):
    """Pulgar arriba (o abajo) relleno, en una mascara."""
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    r = int(c * 0.06)
    d.rounded_rectangle([c * 0.14, c * 0.44, c * 0.30, c * 0.86], r, fill=255)          # puno
    d.rounded_rectangle([c * 0.34, c * 0.42, c * 0.84, c * 0.86], int(c * 0.10), fill=255)  # mano
    d.polygon([(c * 0.34, c * 0.48), (c * 0.47, c * 0.20), (c * 0.60, c * 0.24), (c * 0.56, c * 0.44)], fill=255)
    d.ellipse([c * 0.45, c * 0.12, c * 0.61, c * 0.28], fill=255)                     # punta del pulgar
    # mismo margen que los demas iconos del reproductor
    k = 0.76
    small = m.resize((int(c * k), int(c * k)), Image.LANCZOS)
    m = new_mask(cell)
    m.paste(small, (int(c * (1 - k) / 2), int(c * (1 - k) / 2)))
    if down:
        m = m.transpose(Image.FLIP_TOP_BOTTOM)
    return m

def icon_thumb(cell, down, filled):
    m = thumb_mask(cell, down)
    if not filled:
        # solo el contorno: se le quita el interior (la mascara encogida)
        inner = m.filter(ImageFilter.MinFilter(int(cell * S * 0.085) | 1))
        m = ImageChops.subtract(m, inner)
    return glossy_white(m)

def m_news(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.rounded_rectangle([c * 0.12, c * 0.16, c * 0.88, c * 0.84], int(c * 0.08), fill=255)
    d.rectangle([c * 0.22, c * 0.27, c * 0.48, c * 0.50], fill=0)                # foto
    for yy in (0.29, 0.40, 0.51):
        d.rectangle([c * 0.55, c * yy, c * 0.78, c * (yy + 0.05)], fill=0)
    for yy in (0.62, 0.73):
        d.rectangle([c * 0.22, c * yy, c * 0.78, c * (yy + 0.05)], fill=0)
    return flat(m)

def m_sports(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.pieslice([c * 0.26, c * -0.08, c * 0.74, c * 0.62], 0, 180, fill=255)      # copa
    d.rectangle([c * 0.26, c * 0.12, c * 0.74, c * 0.27], fill=255)
    w = int(c * 0.07)
    d.arc([c * 0.08, c * 0.14, c * 0.36, c * 0.44], 90, 270, fill=255, width=w)  # asas
    d.arc([c * 0.64, c * 0.14, c * 0.92, c * 0.44], 270, 90, fill=255, width=w)
    d.rectangle([c * 0.45, c * 0.56, c * 0.55, c * 0.72], fill=255)              # tallo
    d.rounded_rectangle([c * 0.28, c * 0.72, c * 0.72, c * 0.86], int(c * 0.03), fill=255)
    return flat(m)

def m_history(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    w = int(c * 0.09)
    d.arc([c * 0.14, c * 0.14, c * 0.86, c * 0.86], 200, 520, fill=255, width=w)
    d.polygon([(c * 0.06, c * 0.40), (c * 0.30, c * 0.40), (c * 0.15, c * 0.60)], fill=255)  # punta
    d.line([(c * 0.50, c * 0.30), (c * 0.50, c * 0.52), (c * 0.65, c * 0.62)], fill=255, width=int(c * 0.08), joint='curve')
    return flat(m)

def m_settings(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    cx = cy = c * 0.5
    for k in range(8):
        a = k * math.pi / 4
        dx, dy = math.cos(a), math.sin(a)
        px, py = -dy, dx
        r0, r1, hw = c * 0.26, c * 0.44, c * 0.08
        d.polygon([(cx + dx * r0 + px * hw, cy + dy * r0 + py * hw), (cx + dx * r1 + px * hw * 0.8, cy + dy * r1 + py * hw * 0.8),
                   (cx + dx * r1 - px * hw * 0.8, cy + dy * r1 - py * hw * 0.8), (cx + dx * r0 - px * hw, cy + dy * r0 - py * hw)], fill=255)
    d.ellipse([c * 0.20, c * 0.20, c * 0.80, c * 0.80], fill=255)
    d.ellipse([c * 0.38, c * 0.38, c * 0.62, c * 0.62], fill=0)
    return flat(m)

def m_user(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    d.ellipse([c * 0.34, c * 0.14, c * 0.66, c * 0.46], fill=255)
    d.pieslice([c * 0.16, c * 0.52, c * 0.84, c * 1.20], 180, 360, fill=255)
    return flat(m)

def m_backspace(cell):
    m = new_mask(cell); d = ImageDraw.Draw(m); c = cell * S
    flat_ = lambda mk: (lambda o: (o.putalpha(mk), o)[1])(Image.new('RGBA', mk.size, (255, 255, 255, 0)))
    w = int(c * 0.07)
    pts = [(c * 0.08, c * 0.50), (c * 0.30, c * 0.22), (c * 0.90, c * 0.22), (c * 0.90, c * 0.78), (c * 0.30, c * 0.78)]
    d.line(pts + [pts[0]], fill=255, width=w, joint='curve')
    d.line([(c * 0.46, c * 0.38), (c * 0.70, c * 0.62)], fill=255, width=w)
    d.line([(c * 0.70, c * 0.38), (c * 0.46, c * 0.62)], fill=255, width=w)
    return flat_(m)

# ------------------------------------------- botones del control (ayudas)
def pad_button(cell, letter, color):
    """Boton redondo con brillo (A verde, B rojo, X azul, Y amarillo)."""
    c = cell * S
    out = Image.new('RGBA', (c, c), (0, 0, 0, 0))
    circ = Image.new('L', (c, c), 0)
    ImageDraw.Draw(circ).ellipse([c * 0.04, c * 0.04, c * 0.96, c * 0.96], fill=255)
    dark = tuple(int(v * 0.55) for v in color) + (255,)
    rim = Image.new('RGBA', (c, c), dark); rim.putalpha(circ); out.alpha_composite(rim)
    inner = Image.new('L', (c, c), 0)
    ImageDraw.Draw(inner).ellipse([c * 0.10, c * 0.10, c * 0.90, c * 0.90], fill=255)
    light = tuple(min(255, int(v * 1.25 + 30)) for v in color) + (255,)
    body = vgrad((c, c), light, tuple(int(v * 0.85) for v in color) + (255,))
    body.putalpha(inner); out.alpha_composite(body)
    lm = Image.new('L', (c, c), 0)
    text_mask(lm, letter, int(c * 0.62), (c * 0.5, c * 0.53), 900)
    sh = Image.new('RGBA', (c, c), (0, 0, 0, 0))
    sh.putalpha(ImageChops.offset(lm, 0, int(c * 0.04)).point(lambda v: int(v * 0.55)))
    out.alpha_composite(sh)
    let = Image.new('RGBA', (c, c), (255, 255, 255, 0)); let.putalpha(lm); out.alpha_composite(let)
    hl = Image.new('L', (c, c), 0)
    ImageDraw.Draw(hl).ellipse([c * 0.18, c * 0.08, c * 0.82, c * 0.46], fill=70)
    shine = Image.new('RGBA', (c, c), (255, 255, 255, 0)); shine.putalpha(hl); out.alpha_composite(shine)
    return out

def pad_bumper(cell, text):
    c = cell * S
    out = Image.new('RGBA', (c, c), (0, 0, 0, 0))
    m = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m).rounded_rectangle([c * 0.02, c * 0.22, c * 0.98, c * 0.80], int(c * 0.16), fill=255)
    rim = Image.new('RGBA', (c, c), (40, 40, 44, 255)); rim.putalpha(m); out.alpha_composite(rim)
    m2 = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m2).rounded_rectangle([c * 0.07, c * 0.27, c * 0.93, c * 0.75], int(c * 0.12), fill=255)
    body = vgrad((c, c), (150, 152, 158, 255), (86, 88, 94, 255)); body.putalpha(m2); out.alpha_composite(body)
    lm = Image.new('L', (c, c), 0)
    text_mask(lm, text, int(c * 0.40), (c * 0.5, c * 0.515), 900)
    let = Image.new('RGBA', (c, c), (255, 255, 255, 0)); let.putalpha(lm); out.alpha_composite(let)
    return out

def pad_small(cell, right):
    """Start (flecha a la derecha) y Back (a la izquierda): botones plateados."""
    c = cell * S
    out = Image.new('RGBA', (c, c), (0, 0, 0, 0))
    m = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m).ellipse([c * 0.08, c * 0.08, c * 0.92, c * 0.92], fill=255)
    rim = Image.new('RGBA', (c, c), (70, 72, 78, 255)); rim.putalpha(m); out.alpha_composite(rim)
    m2 = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m2).ellipse([c * 0.14, c * 0.14, c * 0.86, c * 0.86], fill=255)
    body = vgrad((c, c), (236, 238, 242, 255), (170, 174, 182, 255)); body.putalpha(m2); out.alpha_composite(body)
    a = Image.new('L', (c, c), 0)
    if right:
        ImageDraw.Draw(a).polygon([(c * 0.38, c * 0.30), (c * 0.70, c * 0.50), (c * 0.38, c * 0.70)], fill=255)
    else:
        ImageDraw.Draw(a).polygon([(c * 0.62, c * 0.30), (c * 0.30, c * 0.50), (c * 0.62, c * 0.70)], fill=255)
    arr = Image.new('RGBA', (c, c), (50, 52, 58, 255)); arr.putalpha(a); out.alpha_composite(arr)
    return out

def pad_dpad(cell, lit):
    """Cruceta; lit = brazos encendidos ('LR', 'UD' o '' = todos igual)."""
    c = cell * S
    out = Image.new('RGBA', (c, c), (0, 0, 0, 0))
    w0, w1 = c * 0.34, c * 0.66
    base = Image.new('L', (c, c), 0); d = ImageDraw.Draw(base)
    d.rounded_rectangle([w0, c * 0.04, w1, c * 0.96], int(c * 0.06), fill=255)
    d.rounded_rectangle([c * 0.04, w0, c * 0.96, w1], int(c * 0.06), fill=255)
    body = vgrad((c, c), (120, 122, 128, 255), (66, 68, 74, 255)); body.putalpha(base); out.alpha_composite(body)
    on = Image.new('L', (c, c), 0); d = ImageDraw.Draw(on)
    if lit in ('LR', ''):
        d.polygon([(c * 0.08, c * 0.5), (c * 0.24, c * 0.39), (c * 0.24, c * 0.61)], fill=255)
    if lit in ('LR', '', 'R'):
        d.polygon([(c * 0.92, c * 0.5), (c * 0.76, c * 0.39), (c * 0.76, c * 0.61)], fill=255)
    if lit in ('UD', ''):
        d.polygon([(c * 0.5, c * 0.08), (c * 0.39, c * 0.24), (c * 0.61, c * 0.24)], fill=255)
    if lit in ('UD', '', 'D'):
        d.polygon([(c * 0.5, c * 0.92), (c * 0.39, c * 0.76), (c * 0.61, c * 0.76)], fill=255)
    arr = Image.new('RGBA', (c, c), (255, 255, 255, 255)); arr.putalpha(on); out.alpha_composite(arr)
    return out

def pad_trigger(cell, text):
    """Gatillo (LT / RT): pestana redondeada arriba, gris oscuro."""
    c = cell * S
    out = Image.new('RGBA', (c, c), (0, 0, 0, 0))
    m = Image.new('L', (c, c), 0); d = ImageDraw.Draw(m)
    d.rounded_rectangle([c * 0.14, c * 0.06, c * 0.86, c * 0.94], int(c * 0.30), fill=255)
    d.rectangle([c * 0.14, c * 0.55, c * 0.86, c * 0.94], fill=255)
    rim = Image.new('RGBA', (c, c), (40, 40, 44, 255)); rim.putalpha(m); out.alpha_composite(rim)
    m2 = Image.new('L', (c, c), 0); d = ImageDraw.Draw(m2)
    d.rounded_rectangle([c * 0.20, c * 0.12, c * 0.80, c * 0.88], int(c * 0.25), fill=255)
    d.rectangle([c * 0.20, c * 0.55, c * 0.80, c * 0.88], fill=255)
    body = vgrad((c, c), (150, 152, 158, 255), (86, 88, 94, 255)); body.putalpha(m2); out.alpha_composite(body)
    lm = Image.new('L', (c, c), 0)
    text_mask(lm, text, int(c * 0.36), (c * 0.5, c * 0.56), 900)
    let = Image.new('RGBA', (c, c), (255, 255, 255, 0)); let.putalpha(lm); out.alpha_composite(let)
    return out

def pad_stick(cell, letter):
    """Palanca (L / R) vista desde arriba."""
    c = cell * S
    out = Image.new('RGBA', (c, c), (0, 0, 0, 0))
    m = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m).ellipse([c * 0.04, c * 0.04, c * 0.96, c * 0.96], fill=255)
    rim = Image.new('RGBA', (c, c), (34, 34, 38, 255)); rim.putalpha(m); out.alpha_composite(rim)
    m2 = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m2).ellipse([c * 0.12, c * 0.12, c * 0.88, c * 0.88], fill=255)
    body = vgrad((c, c), (138, 140, 146, 255), (70, 72, 78, 255)); body.putalpha(m2); out.alpha_composite(body)
    m3 = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m3).ellipse([c * 0.26, c * 0.26, c * 0.74, c * 0.74], fill=255)
    dish = vgrad((c, c), (70, 72, 78, 255), (120, 122, 128, 255)); dish.putalpha(m3); out.alpha_composite(dish)
    lm = Image.new('L', (c, c), 0)
    text_mask(lm, letter, int(c * 0.36), (c * 0.5, c * 0.52), 900)
    let = Image.new('RGBA', (c, c), (255, 255, 255, 0)); let.putalpha(lm); out.alpha_composite(let)
    return out

def orb(cell, margin):
    """Orbe rojo brillante (fondo del boton elegido)."""
    c = cell * S
    size = (c, c)
    r0 = margin * S
    out = Image.new('RGBA', size, (0, 0, 0, 0))
    m = Image.new('L', size, 0)
    ImageDraw.Draw(m).ellipse([r0, r0, c - r0, c - r0], fill=255)
    # resplandor
    glow = m.filter(ImageFilter.GaussianBlur(2.5 * S))
    gl = Image.new('RGBA', size, (255, 40, 30, 0))
    gl.putalpha(glow.point(lambda v: int(v * 0.55)))
    out.alpha_composite(gl)
    # cuerpo con degradado radial (mas claro arriba)
    body = Image.new('RGBA', size)
    px = body.load()
    cx, cy, rad = c / 2, c * 0.38, (c - 2 * r0) * 0.72
    for yy in range(c):
        for xx in range(c):
            t = min(1.0, math.hypot(xx - cx, yy - cy) / rad)
            t = t ** 1.3
            px[xx, yy] = (int(255 - 95 * t), int(84 - 70 * t), int(70 - 60 * t), 255)
    body.putalpha(m)
    out.alpha_composite(body)
    # borde oscuro
    rim = Image.new('L', size, 0)
    ImageDraw.Draw(rim).ellipse([r0, r0, c - r0, c - r0], outline=255, width=int(1.2 * S))
    rimc = Image.new('RGBA', size, (110, 8, 4, 0))
    rimc.putalpha(rim.point(lambda v: int(v * 0.8)))
    out.alpha_composite(rimc)
    # reflejo de vidrio arriba
    hl = Image.new('L', size, 0)
    hd = ImageDraw.Draw(hl)
    top = Image.new('L', size, 0)
    ImageDraw.Draw(top).ellipse([c * 0.22, r0 + c * 0.04, c * 0.78, c * 0.50], fill=255)
    grad = Image.new('L', size, 0)
    gd = ImageDraw.Draw(grad)
    for yy in range(c):
        gd.line([(0, yy), (c, yy)], fill=max(0, int(170 - 300 * (yy / c))))
    hl = ImageChops.multiply(top, grad)
    shine = Image.new('RGBA', size, (255, 255, 255, 0))
    shine.putalpha(hl)
    out.alpha_composite(shine)
    # reflejo de abajo (luz que "atraviesa" el vidrio)
    low = Image.new('L', size, 0)
    ImageDraw.Draw(low).ellipse([c * 0.30, c * 0.62, c * 0.70, c * 0.86], fill=70)
    low = low.filter(ImageFilter.GaussianBlur(2 * S))
    lowc = Image.new('RGBA', size, (255, 150, 140, 0))
    lowc.putalpha(ImageChops.multiply(low, m))
    out.alpha_composite(lowc)
    return out

def knob(cell):
    c = cell * S
    m = Image.new('L', (c, c), 0)
    ImageDraw.Draw(m).ellipse([c * 0.2, c * 0.2, c * 0.8, c * 0.8], fill=255)
    return glossy_white(m)

def down(im, cell):
    return im.resize((cell, cell), Image.LANCZOS)

# (nombre, celda, funcion)
ICONS = [
    ('PLAY', 56, lambda: icon_play(56)),
    ('PAUSE', 56, lambda: icon_pause(56)),
    ('REW10', 56, lambda: icon_seek(56, False)),
    ('FWD10', 56, lambda: icon_seek(56, True)),
    ('CLOSE', 56, lambda: icon_close(56)),
    ('HQ', 56, lambda: icon_badge(56, 'HQ')),
    ('CC', 56, lambda: icon_badge(56, 'CC')),
    ('GLOBE', 56, lambda: icon_globe(56)),
    ('INFO', 56, lambda: icon_info(56)),
    ('SEARCH', 56, lambda: icon_search(56)),
    ('ORB', 56, lambda: orb(56, 2)),
    ('SUN', 32, lambda: icon_sun(32)),
    ('MOON', 32, lambda: icon_moon(32)),
    ('CHECK', 32, lambda: icon_check(32)),
    ('ORB_S', 32, lambda: orb(32, 1)),
    ('KNOB', 24, lambda: knob(24)),
    ('HOURGLASS', 32, lambda: icon_hourglass(32)),
    # menu lateral (planos, se tinen) y teclado
    ('M_SEARCH', 36, lambda: m_search(36)),
    ('M_HOME', 36, lambda: m_home(36)),
    ('M_GAMING', 36, lambda: m_gaming(36)),
    ('M_MUSIC', 36, lambda: m_music(36)),
    ('M_NEWS', 36, lambda: m_news(36)),
    ('M_SPORTS', 36, lambda: m_sports(36)),
    ('M_HISTORY', 36, lambda: m_history(36)),
    ('M_SETTINGS', 36, lambda: m_settings(36)),
    ('M_USER', 36, lambda: m_user(36)),
    ('M_SOUND', 36, lambda: m_sound(36)),
    ('M_SHORTS', 36, lambda: m_shorts(36)),
    ('K_BACKSPACE', 32, lambda: m_backspace(32)),
    # botones del control para las ayudas de abajo
    ('PAD_A', 28, lambda: pad_button(28, 'A', (88, 170, 40))),
    ('PAD_B', 28, lambda: pad_button(28, 'B', (214, 40, 36))),
    ('PAD_X', 28, lambda: pad_button(28, 'X', (30, 112, 214))),
    ('PAD_Y', 28, lambda: pad_button(28, 'Y', (232, 168, 0))),
    ('PAD_LB', 28, lambda: pad_bumper(28, 'LB')),
    ('PAD_RB', 28, lambda: pad_bumper(28, 'RB')),
    ('PAD_START', 28, lambda: pad_small(28, True)),
    ('PAD_BACK', 28, lambda: pad_small(28, False)),
    ('PAD_DPAD', 28, lambda: pad_dpad(28, '')),
    ('PAD_LR', 28, lambda: pad_dpad(28, 'LR')),
    ('PAD_UD', 28, lambda: pad_dpad(28, 'UD')),
    ('PAD_RIGHT', 28, lambda: pad_dpad(28, 'R')),
    ('PAD_LT', 28, lambda: pad_trigger(28, 'LT')),
    ('PAD_RT', 28, lambda: pad_trigger(28, 'RT')),
    ('PAD_LS', 28, lambda: pad_stick(28, 'L')),
    ('PAD_RS', 28, lambda: pad_stick(28, 'R')),
    ('PAD_DOWN', 28, lambda: pad_dpad(28, 'D')),
    # 2026-11-23: suscripciones y me gusta / no me gusta
    ('M_SUBS', 36, lambda: m_subs(36)),
    ('LIKE', 56, lambda: icon_thumb(56, False, False)),
    ('LIKE_ON', 56, lambda: icon_thumb(56, False, True)),
    ('DISLIKE', 56, lambda: icon_thumb(56, True, False)),
    ('DISLIKE_ON', 56, lambda: icon_thumb(56, True, True)),
    ('UPDATE', 36, lambda: icon_update(36)),
    ('XBOX', 40, lambda: icon_xbox(40)),
    ('M_STAR', 36, lambda: m_star(36)),
    ('STAR', 56, lambda: icon_star(56, False)),
    ('STAR_ON', 56, lambda: icon_star(56, True)),
]

def make_icon_atlas(width=512):
    ims = [(name, cell, down(fn(), cell)) for name, cell, fn in ICONS]
    x, y, rowh = 0, 0, 0
    rects = []
    for name, cell, im in ims:
        if x + cell > width:
            x = 0; y += rowh; rowh = 0
        rects.append((name, x, y, cell, cell))
        x += cell
        rowh = max(rowh, cell)
    h = (y + rowh + 3) // 4 * 4
    atlas = Image.new('RGBA', (width, h), (0, 0, 0, 0))
    for (name, cell, im), (_, rx, ry, _, _) in zip(ims, rects):
        atlas.paste(im, (rx, ry))
    return atlas, rects

# ------------------------------------------------- mando (manual)
# Dibujo del control que paso el usuario (2026-11-22), ya sin fondo, sin
# lineas rojas y sin numeros: lo deja asi clean_pad.py en pad_clean.png.
# Aqui se le pone un borde claro tenue (el contorno es casi negro y sobre
# el fondo oscuro se perdia) y se reduce al tamano en que se dibuja, para
# que se vea nitido. PAD_POINTS = donde llega la linea de cada boton, en
# pixeles de pad_clean.png (se escalan solos).
PAD_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pad_clean.png')
PAD_SCALE = 0.92
PAD_RIM = 3
PAD_POINTS_SRC = [
    ('LT', 118, 16), ('RT', 387, 16), ('LB', 88, 44), ('RB', 412, 44),
    ('LS', 102, 156), ('RS', 325, 228), ('DPAD', 180, 216),
    ('BACK', 197.5, 143.3), ('START', 312, 143.3), ('GUIDE', 254, 142),
    ('Y', 407.5, 106.8), ('X', 363, 141.4), ('B', 441.5, 138.6), ('A', 397, 173),
]
PAD_POINTS = [(n, int(round((x + PAD_RIM) * PAD_SCALE)), int(round((y + PAD_RIM) * PAD_SCALE)))
              for n, x, y in PAD_POINTS_SRC]

def make_pad():
    src = Image.open(PAD_SRC).convert('RGBA')
    w, h = src.size
    out = Image.new('RGBA', (w + 2 * PAD_RIM, h + 2 * PAD_RIM), (0, 0, 0, 0))
    a = Image.new('L', out.size, 0)
    a.paste(src.getchannel('A'), (PAD_RIM, PAD_RIM))
    rim = a.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(0.8)).point(lambda v: int(v * 0.45))
    glow = Image.new('RGBA', out.size, (150, 152, 158, 0))
    glow.putalpha(rim)
    out.alpha_composite(glow)
    out.alpha_composite(src, (PAD_RIM, PAD_RIM))
    size = (int(round(out.size[0] * PAD_SCALE)), int(round(out.size[1] * PAD_SCALE)))
    return out.resize(size, Image.LANCZOS)

# ---------------------------------------------------------------- logo
def make_logo(height):
    """Logo a 'height' px de alto con un halo blanco suave detras: el
    "Tube" es negro y sin halo no se ve sobre el fondo oscuro (sobre el
    claro el halo no se nota)."""
    im = Image.open(LOGO).convert('RGBA')
    bbox = im.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox()
    im = im.crop(bbox)
    pad = max(2, int(round(height * 0.06)))
    w = int(round(im.size[0] * height / im.size[1]))
    im = im.resize((w, height), Image.LANCZOS)
    canvas = Image.new('RGBA', (w + 2 * pad, height + 2 * pad), (255, 255, 255, 0))
    a = Image.new('L', canvas.size, 0)
    a.paste(im.getchannel('A'), (pad, pad))
    # 2026-11-21: el usuario lo vio demasiado brillante; ahora es un borde
    # gris tenue y pegado a las letras, apenas para separar el negro del fondo
    halo = a.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(max(1.0, height * 0.012)))
    halo = halo.point(lambda v: int(v * 0.30))
    canvas = Image.new('RGBA', canvas.size, (190, 190, 190, 0))
    canvas.putalpha(halo)
    canvas.alpha_composite(im, (pad, pad))
    return canvas

# ---------------------------------------------------------------- sonidos
# Sonidos que dio el usuario (2026-11-22), en assets/sonidos/. Se recorta
# el silencio del principio (el de DESPLAZAMIENTO duraba 150 ms y se sentia
# tarde) y del final (con una salida suave), se igualan de volumen y se
# guardan como PCM de 16 bits. Los que traen los dos canales iguales se
# guardan en mono. En C son "short": el compilador de la 360 los deja en el
# orden de bytes de la consola, que es como los quiere XAudio2.
SOUNDS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sonidos')
SOUNDS = [   # (nombre en C, archivo, volumen RMS buscado)
    ('fila', 'CAMBIAR_DE_FILA.wav', 2600),
    ('mover', 'DESPLAZAMIENTO_INTERFAZ.wav', 2600),
    ('menu', 'MENU_LATERAL.wav', 3000),
    ('resultados', 'RESULTADO_DE_BUSQUEDA.wav', 4200),
    ('salir', 'SALIR.wav', 4200),
]

def load_sound(path, target):
    import wave
    import numpy as np
    w = wave.open(path)
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    assert sw == 2, path
    x = np.frombuffer(w.readframes(n), '<i2').astype(np.float64).reshape(-1, ch)
    if ch == 2 and np.abs(x[:, 0] - x[:, 1]).max() < np.abs(x).max() * 0.02:
        x = x[:, :1]                                   # los dos canales iguales: mono
    level = np.abs(x).max(axis=1)
    thr = max(80.0, level.max() * 0.01)
    on = np.nonzero(level > thr)[0]
    a = max(0, on[0] - int(rate * 0.004))
    b = min(len(x), on[-1] + int(rate * 0.030))
    x = x[a:b]
    fade = min(len(x), int(rate * 0.015))
    x[-fade:] *= np.linspace(1.0, 0.0, fade)[:, None]
    rms = np.sqrt((x ** 2).mean())
    act = x[np.abs(x).max(axis=1) > thr]
    if len(act):
        rms = np.sqrt((act ** 2).mean())
    g = target / rms if rms > 0 else 1.0
    g = min(g, 30000.0 / max(1.0, np.abs(x).max()))     # sin saturar
    y = np.clip(np.round(x * g), -32768, 32767).astype(np.int16)
    return y, rate, y.shape[1]

def c_shorts(name, arr):
    vals = arr.reshape(-1)
    lines = []
    for i in range(0, len(vals), 20):
        lines.append(','.join(str(int(v)) for v in vals[i:i + 20]))
    return 'const short %s[%d] = {\n%s\n};\n' % (name, len(vals), ',\n'.join(lines))

def sounds_c():
    out = []
    for name, fn, target in SOUNDS:
        y, rate, ch = load_sound(os.path.join(SOUNDS_DIR, fn), target)
        out.append(c_shorts('g_snd_%s_pcm' % name, y))
        out.append('const int g_snd_%s_frames = %d;\nconst int g_snd_%s_channels = %d;\nconst int g_snd_%s_rate = %d;\n\n'
                   % (name, y.shape[0], name, ch, name, rate))
        print('sonido %s: %s, %d Hz, %d canal(es), %.3f s' % (name, fn, rate, ch, y.shape[0] / float(rate)))
        import wave
        wv = wave.open('preview_snd_%s.wav' % name, 'wb')
        wv.setnchannels(ch); wv.setsampwidth(2); wv.setframerate(rate); wv.writeframes(y.astype('<i2').tobytes()); wv.close()
    return ''.join(out)

# ---------------------------------------------------------------- salida
def png_bytes(im):
    b = io.BytesIO()
    im.save(b, 'PNG', optimize=True)
    return b.getvalue()

def c_bytes(name, data):
    lines = []
    row = []
    for i, v in enumerate(data):
        row.append(str(v))
        if len(row) == 24:
            lines.append(','.join(row) + ',')
            row = []
    if row:
        lines.append(','.join(row))
    return 'const unsigned char %s[%d] = {\n%s\n};\n' % (name, len(data), '\n'.join(lines))

def main(out_path):
    parts = ['/* ui_assets.c - GENERADO por gen_assets.py, no editar a mano.\n'
             ' *\n'
             ' * Letra: Roboto (c) 2011 The Roboto Project Authors, SIL Open Font\n'
             ' * License 1.1 (https://openfontlicense.org). Se incluyen solo mapas de\n'
             ' * bits de sus letras.\n'
             ' * Iconos: dibujados por gen_assets.py (propios del proyecto).\n'
             ' * Logo: el del usuario (TubeBox360). */\n\n#include "ui_assets.h"\n\n']
    fonts = [('font20', 20, 400, 512), ('font24', 24, 400, 512), ('font40', 40, 400, 1024),
             ('font32b', 32, 700, 1024)]   # la ultima: Roboto Bold (titulos)
    previews = []
    for name, size, weight, aw in fonts:
        atlas, table, asc, desc = make_font_atlas(size, weight, aw)
        data = png_bytes(atlas)
        previews.append((name, atlas))
        parts.append(c_bytes('g_%s_png' % name, data))
        parts.append('static const UiGlyph g_%s_glyphs[%d] = {\n' % (name, len(table)))
        for cp, x, y, w, h, ox, oy, adv in table:
            parts.append('    { %d, %d, %d, %d, %d, %d, %d, %.3ff },\n' % (cp, x, y, w, h, ox, oy, adv))
        parts.append('};\n')
        parts.append('const UiFontDesc g_%s = { %d, %d, %d, %d, %d, g_%s_png, %d, g_%s_glyphs, %d };\n\n'
                     % (name, size, asc, desc, atlas.size[0], atlas.size[1], name, len(data), name, len(table)))
        print('%s: atlas %dx%d, png %d bytes, ascent %d descent %d' % (name, atlas.size[0], atlas.size[1], len(data), asc, desc))
    icons, rects = make_icon_atlas()
    data = png_bytes(icons)
    parts.append(c_bytes('g_icons_png', data))
    parts.append('const int g_icons_png_len = %d;\nconst int g_icons_w = %d;\nconst int g_icons_h = %d;\n' % (len(data), icons.size[0], icons.size[1]))
    parts.append('const UiIconRect g_icon_rects[UI_ICON_COUNT] = {\n')
    for name, x, y, w, h in rects:
        parts.append('    { %d, %d, %d, %d },   /* %s */\n' % (x, y, w, h, name))
    parts.append('};\n\n')
    print('icons: atlas %dx%d, png %d bytes' % (icons.size[0], icons.size[1], len(data)))
    for cname, h in (('logo', 50), ('logo_big', 150)):   # encabezado y pantalla de carga
        logo = make_logo(h)
        data = png_bytes(logo)
        parts.append(c_bytes('g_%s_png' % cname, data))
        parts.append('const int g_%s_png_len = %d;\nconst int g_%s_w = %d;\nconst int g_%s_h = %d;\n'
                     % (cname, len(data), cname, logo.size[0], cname, logo.size[1]))
        print('%s: %dx%d, png %d bytes' % (cname, logo.size[0], logo.size[1], len(data)))
        logo.save('preview_%s.png' % cname)
    dp = Image.open(DP_LOGO).convert('RGBA')
    dp = dp.crop(dp.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox())
    dph = 150
    dp = dp.resize((int(round(dp.size[0] * dph / dp.size[1])), dph), Image.LANCZOS)
    data = png_bytes(dp)
    parts.append(c_bytes('g_dp_png', data))
    parts.append('const int g_dp_png_len = %d;\nconst int g_dp_w = %d;\nconst int g_dp_h = %d;\n' % (len(data), dp.size[0], dp.size[1]))
    dp.save('preview_dp.png')
    print('logo DeadPIayers: %dx%d, png %d bytes' % (dp.size[0], dp.size[1], len(data)))
    # sombra de abajo: negra con alfa; se recorta la parte transparente de
    # arriba y se reduce (es un degradado suave, no pierde nada)
    sh = Image.open(SHADOW).convert('RGBA')
    bb = sh.getchannel('A').point(lambda v: 255 if v > 2 else 0).getbbox()
    sh = sh.crop((0, bb[1], sh.size[0], sh.size[1])).resize((256, 128), Image.LANCZOS)
    data = png_bytes(sh)
    parts.append(c_bytes('g_shadow_png', data))
    parts.append('const int g_shadow_png_len = %d;\nconst int g_shadow_w = %d;\nconst int g_shadow_h = %d;\n' % (len(data), sh.size[0], sh.size[1]))
    print('sombra: %dx%d, png %d bytes (recorte desde y=%d)' % (sh.size[0], sh.size[1], len(data), bb[1]))
    pad = make_pad()
    data = png_bytes(pad)
    parts.append(c_bytes('g_pad_png', data))
    parts.append('const int g_pad_png_len = %d;\nconst int g_pad_w = %d;\nconst int g_pad_h = %d;\n' % (len(data), pad.size[0], pad.size[1]))
    parts.append('const UiPoint g_pad_points[UI_PADPT_COUNT] = {\n')
    for name, x, y in PAD_POINTS:
        parts.append('    { %d, %d },   /* %s */\n' % (x, y, name))
    parts.append('};\n')
    pad.save('preview_pad.png')
    print('pad: %dx%d, png %d bytes' % (pad.size[0], pad.size[1], len(data)))
    parts.append(sounds_c())
    with open(out_path, 'w', newline='\n') as f:
        f.write(''.join(parts))
    # vistas previas para revisar
    icons.save('preview_icons.png')
    for name, atlas in previews:
        atlas.save('preview_%s.png' % name)
    print('icon order:', ', '.join(n for n, _, _, _, _ in rects))

if __name__ == '__main__':
    main(sys.argv[1])
