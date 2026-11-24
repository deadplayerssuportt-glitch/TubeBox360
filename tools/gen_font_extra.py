# gen_font_extra.py - Genera font_extra.c para TubeBox360: letras de japones
# y arabe en un solo atlas PNG (blanco con alfa) + tabla de medidas.
#   - Japones: Noto Sans CJK JP (SIL OFL 1.1): kana, signos, ancho completo
#     y los 2965 kanji del nivel 1 de JIS X 0208 (los de uso comun).
#   - Arabe: DejaVu Sans (licencia libre de Bitstream Vera/DejaVu): letras
#     basicas y sus formas de presentacion (inicial, media, final, aislada)
#     que usa gfx.cpp para unirlas.
# Las medidas siguen las de Roboto al mismo tamano para que la letra extra
# salga del mismo tamano que la latina.
# Uso: python3 -I gen_font_extra.py <salida.c>
import io, sys
from PIL import Image, ImageFont

CJK = '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'
ARABIC = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
ROBOTO = '/usr/share/fonts/truetype/google-fonts/ofl/roboto/Roboto[wdth,wght].ttf'
SIZE = 26
ATLAS_W = 2048

def jis_level1():
    out = []
    for hi in range(0xB0, 0xD0):           # filas 16..47 de JIS X 0208
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode('euc_jp')
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and 0x4E00 <= ord(ch) <= 0x9FFF:
                out.append(ord(ch))
    return out

def charset():
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    from lang_ja_ar import JA_AR, NEW
    jp = set()
    jp.update(range(0x3000, 0x3040))       # signos CJK 「」、。〜
    jp.update(range(0x3041, 0x3097))       # hiragana
    jp.update(range(0x309B, 0x309F))
    jp.update(range(0x30A0, 0x3100))       # katakana
    jp.update(range(0xFF01, 0xFF5F))       # ancho completo
    jp.update(range(0xFF61, 0xFFA0))       # katakana de medio ancho
    jp.update([0x203B, 0x2605, 0x2606, 0x25CB, 0x25CF, 0x25CE, 0x25A0, 0x25A1, 0x25B2, 0x25BC,
               0x2190, 0x2191, 0x2192, 0x2193, 0x266A, 0x266B, 0x00D7, 0x00F7])
    jp.update(jis_level1())
    ar = set()
    ar.update(range(0x0621, 0x063B))       # letras
    ar.update(range(0x0641, 0x064B))
    ar.update(range(0x0660, 0x066A))       # numeros arabes
    ar.update([0x060C, 0x061B, 0x061F, 0x0640, 0x066A, 0x066B, 0x066C])
    ar.update(range(0xFE80, 0xFEFD))       # formas de presentacion B (sin tashkil)
    # todo lo que usan los textos de la app tiene que estar
    for ja, a in list(JA_AR.values()) + [(n[4], n[5]) for n in NEW]:
        for ch in ja:
            if ord(ch) >= 0x2E80:
                jp.add(ord(ch))
        for ch in a:
            if 0x0600 <= ord(ch) <= 0x06FF:
                ar.add(ord(ch))
    jp = sorted(c for c in jp if c >= 0x2000 or c in (0xD7, 0xF7))
    return jp, sorted(ar)

def main(out_path):
    cjk = ImageFont.truetype(CJK, SIZE, index=0)
    arf = ImageFont.truetype(ARABIC, SIZE)
    rob = ImageFont.truetype(ROBOTO, SIZE)
    rob.set_variation_by_axes([400, 100])
    asc, desc = rob.getmetrics()
    jp, ar = charset()
    jp = [c for c in jp if c not in (0xD7, 0xF7)]   # (ya estan en Roboto)
    glyphs = []
    missing = []
    for fnt, cps in ((cjk, jp), (arf, ar)):
        cmap_ok = fnt.font
        for cp in cps:
            ch = chr(cp)
            mask, (ox, oy) = fnt.getmask2(ch, mode='L', anchor='ls')
            adv = fnt.getlength(ch)
            if cp in (0x3000,):
                glyphs.append((cp, None, 0, 0, adv))
                continue
            if mask.size[0] == 0 or mask.size[1] == 0:
                if adv <= 0:
                    missing.append(cp)
                    continue
                glyphs.append((cp, None, 0, 0, adv))
                continue
            im = Image.frombytes('L', mask.size, bytes(mask))
            glyphs.append((cp, im, ox, oy, adv))
    glyphs.sort(key=lambda g: g[0])
    pad = 1
    order = sorted(range(len(glyphs)), key=lambda i: -(glyphs[i][1].size[1] if glyphs[i][1] else 0))
    x, y, rowh = 8, pad, 0
    pos = {}
    for i in order:
        im = glyphs[i][1]
        if im is None:
            continue
        w, h = im.size
        if x + w + pad > ATLAS_W:
            x = pad
            y += rowh + pad
            rowh = 0
        pos[i] = (x, y)
        x += w + pad
        rowh = max(rowh, h)
    atlas_h = y + rowh + pad
    th = 256
    while th < atlas_h:
        th *= 2
    alpha = Image.new('L', (ATLAS_W, th), 0)
    for i, (px, py) in pos.items():
        alpha.paste(glyphs[i][1], (px, py))
    alpha.paste(255, (1, 1, 7, 7))
    rgba = Image.merge('RGBA', [Image.new('L', alpha.size, 255)] * 3 + [alpha])
    buf = io.BytesIO()
    rgba.save(buf, 'PNG', optimize=True)
    png = buf.getvalue()

    lines = ['/* font_extra.c - GENERADO por gen_font_extra.py: letras de japones',
             ' * (Noto Sans CJK JP, SIL OFL 1.1) y arabe (DejaVu Sans) para gfx.cpp.',
             ' * %d letras, atlas %dx%d. */' % (len(glyphs), ATLAS_W, th),
             '#include "ui_assets.h"', '']
    lines.append('static const unsigned char g_extra_png[%d] = {' % len(png))
    for k in range(0, len(png), 40):
        lines.append(','.join(str(b) for b in png[k:k + 40]) + ',')
    lines.append('};')
    lines.append('')
    lines.append('/* ordenadas por codepoint (gfx.cpp busca por mitades) */')
    lines.append('static const UiGlyph g_extra_glyphs[%d] = {' % len(glyphs))
    for i, (cp, im, ox, oy, adv) in enumerate(glyphs):
        if im is None:
            lines.append('{%d,0,0,0,0,0,0,%.2ff},' % (cp, adv))
        else:
            px, py = pos[i]
            lines.append('{%d,%d,%d,%d,%d,%d,%d,%.2ff},' % (cp, px, py, im.size[0], im.size[1], ox, oy, adv))
    lines.append('};')
    lines.append('')
    lines.append('const UiFontDesc g_fontExtra = { %d, %d, %d, %d, %d, g_extra_png, %d, g_extra_glyphs, %d };'
                 % (SIZE, asc, desc, ATLAS_W, th, len(png), len(glyphs)))
    open(out_path, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('%d letras, atlas %dx%d, png %d KB, faltan %d' % (len(glyphs), ATLAS_W, th, len(png) // 1024, len(missing)))
    rgba.crop((0, 0, 1024, 512)).save('/home/claude/assets/preview_extra.png')

if __name__ == '__main__':
    main(sys.argv[1])
