#!/usr/bin/env python3
"""The bottom screen's look ("the dark"): its pictures and strings, made once on a computer into
overlay/port/whole/3ds/ui_assets.h (used by 3ds/ui_bottom_3ds.cpp; the file is committed - run this only to change it).

- the background 320x240: a smooth dark gradient with a soft vignette (dithered into RGB565), blood-red hairlines, the
  buttons' slabs, the map's frame - RGB565 in the framebuffer's order (column x from the bottom row up:
  x * 240 + 239 - y): drawn by one memcpy;
- the text: every string of the screen in each language (en fr de es it ja; Noto Serif JP, OFL - one face for Latin with
  accents and Japanese), anti-aliased, as 8-bit coverage cut to its box (origin: the left end of the baseline; the
  advance for the alignment); the numbers by glyphs of four styles; two strings with numbers as templates (pieces and the
  arguments' places: the order differs by language);
- the heartbeat's curves: coverage, coloured by the life at run time.
The layout's numbers go into the header too (UI_*): the background and the code agree.

Run from the repository's root (Python 3 with tools/requirements-assets.txt; the fonts: tools/fetch_fonts.sh):
  python3 tools/ui_assets.py [--preview PNG]
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # (the repository: tools/..)
FONTS = os.path.join(ROOT, 'build/fonts')  # (tools/fetch_fonts.sh)
OUT = os.path.join(ROOT, 'overlay/port/whole/3ds/ui_assets.h')
PREVIEW = sys.argv[sys.argv.index('--preview') + 1] if '--preview' in sys.argv else None  # (the background as shown)
W, H, SS = 320, 240, 4

SERIF = 'NotoSerifJP-SemiBold.otf'
SERIF_B = 'NotoSerifJP-Black.otf'
SERIF_R = 'NotoSerifJP-Regular.otf'

# --- the layout (the code reads these) ----------------------------------------------------------------------------------
L = dict(
    LABEL_X=12, NUM_R=308,             # the left labels, the right numbers' end
    LEON_BASE=19, ASHLEY_BASE=35,      # the life rows' baselines
    PULSE_X=64, PULSE_W=186,           # the heartbeats (from x, wide)
    PULSE_LEON_Y=15, PULSE_ASHLEY_Y=31,  # their middle rows
    LABEL2_BASE=46, NUM_BASE=63,       # AMMO / PESETAS, the numbers under them
    LINE1_Y=68, LINE2_Y=204,           # the hairlines
    MAP_X=8, MAP_Y=73, MAP_W=304, MAP_H=128,  # the minimap's box (its zoom buttons inside, right)
    BTN_Y=208, BTN_H=27, BTN0_X=8, BTN1_X=164, BTN_W=148, BTN_BASE=226,
    # the settings before the gameplay: the title, a hairline, three tabs (CHEATS,
    # CONTROLS, SCREEN: a player's wish brought more rows than one page holds), the open tab's rows, a hairline, the
    # footnote
    S_TITLE_BASE=21, S_LINE1_Y=28, S_TAB_Y=31, S_TAB_H=22, S_TAB_BASE=46, S_ROW1_Y=58, S_ROW_H=24,
    S_LINE2_Y=215, S_FOOT_BASE=229, S_BOX=11, S_BOX_X=289, S_ARROW_L=228, S_ARROW_R=300, S_GAIN_X=264,
)

# (the strings below keep a seventh, Russian text: left out of the screen - zip drops it)
LANGS = ['en', 'fr', 'de', 'es', 'it', 'ja']

# the strings: (name, font, size, letter spacing, {lang: text}); one text for all when a str
STRINGS = [
    ('LEON', SERIF, 10, 2, ['LEON'] * 5 + ['レオン', 'ЛЕОН']),
    ('ASHLEY', SERIF, 10, 2, ['ASHLEY'] * 5 + ['アシュリー', 'ЭШЛИ']),
    ('AMMO', SERIF_R, 8, 2, ['AMMO', 'MUNITIONS', 'MUNITION', 'MUNICIÓN', 'MUNIZIONI', '弾薬', 'ПАТРОНЫ']),
    ('PTAS', SERIF_R, 8, 2, ['PESETAS', 'PESETAS', 'PESETEN', 'PESETAS', 'PESETAS', 'ペセタ', 'ПЕСЕТЫ']),
    ('CASE', SERIF, 13, 3, ['CASE', 'MALLETTE', 'KOFFER', 'MALETÍN', 'VALIGETTA', 'アタッシュケース', 'КЕЙС']),
    ('MAP', SERIF, 13, 3, ['MAP', 'CARTE', 'KARTE', 'MAPA', 'MAPPA', 'マップ', 'КАРТА']),
    ('AREA', SERIF_R, 8, 2, ['AREA', 'ZONE', 'GEBIET', 'ZONA', 'ZONA', 'エリア', 'ОБЛАСТЬ']),
    ('ROOM', SERIF_R, 8, 2, ['ROOM', 'SALLE', 'RAUM', 'SALA', 'STANZA', '部屋', 'КОМНАТА']),
    ('X2', SERIF_R, 9, 1, '×2'),
    ('X4', SERIF_R, 9, 1, '×4'),
    ('OFFMAP', SERIF_R, 10, 0, ['outside the area map', 'hors de la carte', 'außerhalb der Karte', 'fuera del mapa',
                                'fuori dalla mappa', 'マップの範囲外', 'вне карты области']),
    ('NOMAP', SERIF_R, 11, 1, ['no map', 'pas de carte', 'keine Karte', 'sin mapa', 'nessuna mappa', 'マップなし',
                               'карта недоступна']),
    ('BADGE_S', SERIF_B, 9, 0, 'S'),       # (the minimap's typewriters)
    ('BADGE_D', SERIF_B, 8, 0, 'M'),       # (its merchant: M as on the game's map; 8 px: 9 fills the box)
    # the settings before the
    # gameplay (3ds/setup_3ds.cpp, drawn by re4_ui_setup)
    ('S_TITLE', SERIF, 13, 3, ['SETTINGS', 'RÉGLAGES', 'EINSTELLUNGEN', 'AJUSTES', 'IMPOSTAZIONI', '設定']),
    ('S_VERSION', SERIF_R, 8, 1, 'PuebloCTR β1'),
    ('S_CHEATS', SERIF_R, 8, 2, ['CHEATS', 'TRICHES', 'CHEATS', 'TRUCOS', 'TRUCCHI', 'チート']),
    ('S_GAME', SERIF_R, 8, 2, ['GAME', 'JEU', 'SPIEL', 'JUEGO', 'GIOCO', 'ゲーム']),
    ('S_HEALTH', SERIF_R, 11, 0, ['Infinite health', 'Santé infinie', 'Unendliche Gesundheit', 'Salud infinita',
                                  'Salute infinita', '体力無限']),
    ('S_AMMO', SERIF_R, 11, 0, ['Infinite ammo', 'Munitions infinies', 'Unendliche Munition', 'Munición infinita',
                                'Munizioni infinite', '弾薬無限']),
    ('S_ONEHIT', SERIF_R, 11, 0, ['One-hit kills', 'Ennemis en un coup', 'Gegner mit einem Treffer',
                                  'Enemigos de un golpe', 'Nemici con un colpo', '一撃で倒す']),
    ('S_QTE', SERIF_R, 11, 0, ['Automatic QTEs', 'QTE automatiques', 'QTEs automatisch', 'QTE automáticos',
                               'QTE automatici', 'QTE自動']),
    ('S_MONEY', SERIF_R, 11, 0, ['+1,000,000 pesetas', '+1 000 000 pesetas', '+1.000.000 Peseten',
                                 '+1.000.000 pesetas', '+1.000.000 pesetas', '+1,000,000ペセタ']),
    ('S_GYRO', SERIF_R, 11, 0, ['Gyro aim (hold R)', 'Visée gyroscopique (R)', 'Zielen mit Gyroskop (R)',
                                'Apuntar con giroscopio (R)', 'Mira con giroscopio (R)', 'ジャイロ照準（Rで構え）']),
    ('S_FPS', SERIF_R, 11, 0, ['Frame counter', 'Images par seconde', 'Bilder pro Sekunde', 'Contador de FPS',
                               'Contatore FPS', 'フレームレート表示']),
    ('S_OFF', SERIF_R, 9, 1, ['OFF', 'NON', 'AUS', 'NO', 'NO', 'オフ']),
    ('S_FOOT', SERIF_R, 9, 0, ['Applied when a game starts or loads', "Appliqué au début ou au chargement d'une partie",
                               'Gilt ab Spielbeginn oder Laden eines Spielstands',
                               'Se aplica al empezar o cargar una partida',
                               "Si applica all'avvio o al caricamento di una partita", 'ゲーム開始・ロード時に反映']),
    # the gyro's gains (the decimal mark of the language)
    ('S_G10', SERIF, 11, 0, '1×'),
    ('S_G15', SERIF, 11, 0, ['1.5×', '1,5×', '1,5×', '1,5×', '1,5×', '1.5×']),
    ('S_G20', SERIF, 11, 0, '2×'),
    ('S_G25', SERIF, 11, 0, ['2.5×', '2,5×', '2,5×', '2,5×', '2,5×', '2.5×']),
    ('S_G30', SERIF, 11, 0, '3×'),
    ('S_G40', SERIF, 11, 0, '4×'),
    # the circle pad's sensitivity (3ds/input_3ds.cpp re4_3ds_stick_set): its row and the gain between 1× and 1.5×
    ('S_STICK', SERIF_R, 11, 0, ['Circle Pad sensitivity', 'Sensibilité du pad circulaire', 'Schiebepad-Empfindlichkeit',
                                 'Sensibilidad del botón deslizante', 'Sensibilità del pad scorrevole', 'スライドパッド感度']),
    ('S_G12', SERIF, 11, 0, ['1.25×', '1,25×', '1,25×', '1,25×', '1,25×', '1.25×']),
    # the picture on the top screen (3ds/gx_3ds.cpp re4_3ds_gx_fill; a player's wish: no bars): the game's
    # 16:9 band fitted (as before), stretched to the screen's height, or zoomed to it (the sides cut)
    ('S_PICTURE', SERIF_R, 11, 0, ['Picture', 'Image', 'Bild', 'Imagen', 'Immagine', '画面']),
    ('S_FIT', SERIF, 10, 0, ['Fit', 'Ajustée', 'Passend', 'Ajustada', 'Adattata', '標準']),
    ('S_STRETCH', SERIF, 10, 0, ['Stretch', 'Étirée', 'Gestreckt', 'Estirada', 'Allungata', '引き伸ばし']),
    ('S_ZOOM', SERIF, 10, 0, ['Zoom', 'Zoom', 'Zoom', 'Zoom', 'Zoom', 'ズーム']),
    # the settings screen's tabs
    ('S_TAB_CHEATS', SERIF_R, 10, 2, ['CHEATS', 'TRICHES', 'CHEATS', 'TRUCOS', 'TRUCCHI', 'チート']),
    ('S_TAB_CONTROLS', SERIF_R, 10, 2, ['CONTROLS', 'COMMANDES', 'STEUERUNG', 'CONTROLES', 'COMANDI', '操作']),
    ('S_TAB_SCREEN', SERIF_R, 10, 2, ['SCREEN', 'ÉCRAN', 'BILDSCHIRM', 'PANTALLA', 'SCHERMO', '画面']),
    # (English only - the game's language is not known yet) the checks
    # at the start (3ds/startcheck_3ds.cpp, drawn by re4_ui_check). C_T_* the titles, C_F_* the footers, C_H_* the
    # headings of a problem, the rest its lines
    ('C_T_STOP', SERIF, 13, 3, 'CANNOT START'),
    ('C_T_WARN', SERIF, 13, 3, 'BEFORE YOU PLAY'),
    ('C_F_STOP', SERIF_R, 10, 0, 'START  exit'),
    ('C_F_WARN', SERIF_R, 10, 0, 'A  continue          START  exit'),
    ('C_H_OLD', SERIF, 12, 0, 'A New Nintendo 3DS is needed'),
    ('C_OLD1', SERIF_R, 10, 0, 'New Nintendo 3DS, New 3DS XL or New 2DS XL.'),
    ('C_OLD2', SERIF_R, 10, 0, 'The older 3DS and 2DS are too slow for this game.'),
    ('C_H_MEM', SERIF, 12, 0, 'Not enough memory'),
    ('C_MEM1', SERIF_R, 10, 0, 'The system gave PuebloCTR too little memory.'),
    ('C_MEM2', SERIF_R, 10, 0, 'Start the Homebrew Launcher through a game'),
    ('C_MEM3', SERIF_R, 10, 0, '(title takeover), not as an applet.'),
    ('C_H_NODATA', SERIF, 12, 0, 'Game data not found'),
    ('C_NODATA1', SERIF_R, 10, 0, 'The files of disc 1 are expected in'),
    ('C_NODATA2', SERIF_R, 10, 0, 'sdmc:/3ds/pueblo/data/disc0'),
    ('C_PREP', SERIF_R, 10, 0, 'Prepare them from your own discs with PuebloPrep.'),
    ('C_H_BAD', SERIF, 12, 0, 'Game data incomplete'),
    ('C_BAD1', SERIF_R, 10, 0, 'data/disc0 has no sys/boot.bin or sys/fst.bin.'),
    ('C_H_ED', SERIF, 12, 0, 'This disc is not supported'),
    ('C_ED_JP', SERIF_R, 10, 0, 'Found: the Japanese edition (G4BJ08).'),
    ('C_ED_DBG', SERIF_R, 10, 0, 'Found: a debug build of the game.'),
    ('C_ED_UNK', SERIF_R, 10, 0, 'Found: an unknown disc.'),
    ('C_ED_OK', SERIF_R, 10, 0, 'Supported: Europe (G4BP08) and USA (G4BE08).'),
    ('C_H_SWAP', SERIF, 12, 0, 'The discs are swapped'),
    ('C_SWAP1', SERIF_R, 10, 0, 'data/disc0 holds disc 2.'),
    ('C_DISCS', SERIF_R, 10, 0, 'Disc 1 goes in data/disc0, disc 2 in data/disc1.'),
    ('C_H_MIX', SERIF, 12, 0, 'The discs do not match'),
    ('C_MIX1', SERIF_R, 10, 0, 'data/disc1 is not disc 2 of the same edition.'),
    ('C_H_NO2', SERIF, 12, 0, 'Disc 2 not found (data/disc1)'),
    ('C_NO2_1', SERIF_R, 10, 0, 'You can play until the game asks for disc 2.'),
    ('C_H_NOMOV', SERIF, 12, 0, 'Movies not found (movie/)'),
    ('C_H_SOMEMOV', SERIF, 12, 0, 'Some movies are missing (movie/)'),
    ('C_MOV1', SERIF_R, 10, 0, 'The full-motion movies will not show properly.'),
    ('C_H_NOPAK', SERIF, 12, 0, 'Texture pack not found (textures.pak)'),
    ('C_H_OLDPAK', SERIF, 12, 0, 'The texture pack does not fit this version'),
    ('C_PAK1', SERIF_R, 10, 0, 'The game stutters when new textures appear.'),
]
# templates: {0} {1} the arguments (in the style DS_SMALL); the pieces in the same face
TEMPLATES = [
    ('FLOOR', ['floor {0} of {1}', 'étage {0} sur {1}', 'Etage {0} von {1}', 'planta {0} de {1}', 'piano {0} di {1}',
               '{0}階 / {1}', 'этаж {0} из {1}']),
    ('MEDALS', ['medallions {0}/{1}', 'médaillons {0}/{1}', 'Medaillons {0}/{1}', 'medallones {0}/{1}',
                'medaglioni {0}/{1}', '青いメダル {0}/{1}', 'медальоны {0}/{1}']),
]
TPL_FONT, TPL_SIZE = SERIF_R, 10
DIGITS = '0123456789/ -—×'
DSTYLES = [('BIG', SERIF_B, 16),  # (the ammo; 20 nearly touched the AMMO label)
           ('MID', SERIF_R, 12), ('PTAS', SERIF, 16), ('SMALL', SERIF_R, 10)]


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


# --- the pieces of coverage ---------------------------------------------------------------------------------------------
pieces = []  # (w, h, ox, oy, adv, bytes)


def add_piece(text, fname, size, spacing):
    f = font(fname, size)
    pad = size * 2
    widths = [f.getlength(c) for c in text]
    adv = sum(widths) + spacing * max(0, len(text) - 1)
    img = Image.new('L', (int(adv) + 2 * pad + 4, size * 4), 0)
    d = ImageDraw.Draw(img)
    x = pad
    base = size * 2
    if spacing:
        for c, wdt in zip(text, widths):
            d.text((x, base), c, font=f, fill=255, anchor='ls')
            x += wdt + spacing
    else:
        d.text((x, base), text, font=f, fill=255, anchor='ls')
    bb = img.getbbox()
    if not bb:  # (a space)
        pieces.append((0, 0, 0, 0, int(round(adv)), b''))
        return len(pieces) - 1
    crop = img.crop(bb)
    pieces.append((crop.width, crop.height, bb[0] - pad, bb[1] - base, int(round(adv)), crop.tobytes()))
    return len(pieces) - 1


def lang_size(lang, size):
    return max(size, 10) if lang == 'ja' else size  # (kana and kanji below 10 px do not read)


def lang_spacing(lang, spacing):
    return min(spacing, 1) if lang == 'ja' else spacing


str_idx = {}  # name -> [piece per language]
for name, fname, size, spacing, texts in STRINGS:
    if isinstance(texts, str):
        p = add_piece(texts, fname, size, spacing)
        str_idx[name] = [p] * len(LANGS)
    else:
        str_idx[name] = [add_piece(t, fname, lang_size(l, size), lang_spacing(l, spacing)) for t, l in zip(texts, LANGS)]
    if name.startswith('C_'):  # (R3) the start's lines from x 16 to 304 at most
        assert pieces[str_idx[name][0]][4] <= 288, (name, pieces[str_idx[name][0]][4])

tpl_tokens = {}  # name -> [tokens per language]; a token >= 0 a piece, -1 / -2 the argument 0 / 1
for name, texts in TEMPLATES:
    per = []
    for t, l in zip(texts, LANGS):
        toks = []
        rest = t
        while rest:
            i0, i1 = rest.find('{0}'), rest.find('{1}')
            cand = [i for i in (i0, i1) if i >= 0]
            i = min(cand) if cand else -1
            if i < 0:
                toks.append(add_piece(rest, TPL_FONT, lang_size(l, TPL_SIZE), 0))
                break
            if i > 0:
                toks.append(add_piece(rest[:i], TPL_FONT, lang_size(l, TPL_SIZE), 0))
            toks.append(-1 if rest[i + 1] == '0' else -2)
            rest = rest[i + 3:]
        assert len(toks) <= 8, t
        per.append(toks)
    tpl_tokens[name] = per

digit_idx = []
for dname, fname, size in DSTYLES:
    digit_idx.append([add_piece(c, fname, size, 0) for c in DIGITS])


# --- the heartbeats -----------------------------------------------------------------------------------------------------
def pulse(w, h, amp, period):
    img = Image.new('L', (w * SS, h * SS), 0)
    d = ImageDraw.Draw(img)
    mid = h / 2
    pts = []
    for xx in range(0, w * 4 + 1):
        x = xx / 4
        ph = (x + period * 0.3) % period
        y = mid
        # the P wave, the QRS, the T wave
        if 8 <= ph < 13:
            y = mid - amp * 0.18 * np.sin((ph - 8) / 5 * np.pi)
        elif 16 <= ph < 17.5:
            y = mid + amp * 0.25 * (ph - 16) / 1.5
        elif 17.5 <= ph < 19.5:
            y = mid + amp * 0.25 - amp * 1.25 * (ph - 17.5) / 2
        elif 19.5 <= ph < 22:
            y = mid - amp + amp * 1.6 * (ph - 19.5) / 2.5
        elif 22 <= ph < 23.5:
            y = mid + amp * 0.6 - amp * 0.6 * (ph - 22) / 1.5
        elif 27 <= ph < 35:
            y = mid - amp * 0.3 * np.sin((ph - 27) / 8 * np.pi)
        pts.append((x * SS, y * SS))
    d.line(pts, fill=255, width=int(1.25 * SS), joint='curve')
    img = img.resize((w, h), Image.LANCZOS)
    pieces.append((w, h, 0, -h // 2, w, img.tobytes()))
    return len(pieces) - 1


pulse_leon = pulse(L['PULSE_W'], 21, 9.5, 46)
pulse_ashley = pulse(L['PULSE_W'], 15, 6.0, 40)


# --- R2: the settings' shapes (coverage; the box's top left / the arrow's middle at (x, y) of the blit) ----------------
def shape(w, h, ox, oy, draw):
    img = Image.new('L', (w * SS, h * SS), 0)
    draw(ImageDraw.Draw(img), SS)
    img = img.resize((w, h), Image.LANCZOS)
    pieces.append((w, h, ox, oy, w, img.tobytes()))
    return len(pieces) - 1


# the mark inside the check box (S_BOX square, its top left at the piece's origin)
shape_check = shape(L['S_BOX'], L['S_BOX'], 0, 0, lambda d, k: d.line(
    (2.5 * k, 5.5 * k, 4.7 * k, 8 * k, 8.6 * k, 2.8 * k), fill=255, width=int(1.6 * k), joint='curve'))
# the gyro's arrows: 6 x 8 around the middle (x, y)
shape_arrow_l = shape(8, 10, -4, -5, lambda d, k: d.polygon([(7 * k, 1 * k), (7 * k, 9 * k), (1 * k, 5 * k)], fill=255))
shape_arrow_r = shape(8, 10, -4, -5, lambda d, k: d.polygon([(1 * k, 1 * k), (1 * k, 9 * k), (7 * k, 5 * k)], fill=255))


# --- the background -----------------------------------------------------------------------------------------------------
def background():
    """A smooth vertical gradient, dark blood-brown at the top to near black at
    the bottom, a soft vignette; no grain, no stains. Returns the picture and where its gradient shows (1 - the overlay's
    alpha): only there the RGB565 conversion dithers, the lines and slabs stay flat."""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    t = yy / (H - 1)
    top, bottom = np.array((34, 17, 16), np.float32), np.array((7, 6, 6), np.float32)
    a = top + (bottom - top) * (t ** 0.8)[..., None]
    dd = np.sqrt(((xx - W / 2) / (W / 2)) ** 2 + ((yy - H * 0.4) / (H * 0.75)) ** 2)
    a *= (1 - 0.45 * np.clip(dd - 0.45, 0, 1) ** 1.3)[..., None]
    img = Image.fromarray(np.round(a).clip(0, 255).astype(np.uint8))
    base = a
    layer = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    k = SS
    blood = (90, 10, 10, 255)
    d.line((8 * k, L['LINE1_Y'] * k, 312 * k, L['LINE1_Y'] * k), fill=blood, width=k)
    d.line((8 * k, L['LINE2_Y'] * k, 312 * k, L['LINE2_Y'] * k), fill=blood, width=k)
    # the map's frame
    mx, my, mw, mh = L['MAP_X'], L['MAP_Y'], L['MAP_W'], L['MAP_H']
    d.rectangle(((mx - 1) * k, (my - 1) * k, (mx + mw) * k, (my + mh) * k), outline=(40, 30, 28, 255), width=k)
    # the buttons' slabs (lit at run time: the inside and the line beneath)
    for x0 in (L['BTN0_X'], L['BTN1_X']):
        x1, y0, y1 = x0 + L['BTN_W'], L['BTN_Y'], L['BTN_Y'] + L['BTN_H']
        d.rectangle((x0 * k, y0 * k, x1 * k - 1, y1 * k - 1), fill=(22, 20, 20, 255), outline=(48, 44, 42, 255), width=k)
        d.rectangle(((x0 + 1) * k, (y1 - 3) * k, (x1 - 1) * k - 1, (y1 - 1) * k - 1), fill=blood)
    layer = layer.resize((W, H), Image.LANCZOS)
    alpha = np.asarray(layer)[..., 3].astype(np.float32) / 255
    over = np.asarray(layer)[..., :3].astype(np.float32)
    exact = base * (1 - alpha[..., None]) + over * alpha[..., None]  # (unrounded: the dither works from it)
    img.paste(layer, (0, 0), layer)
    return img, exact, 1 - alpha


bg, exact, open_ = background()
# RGB565 with an ordered 8x8 Bayer dither within one step where the gradient shows (32 steps of red and blue would
# band on a dark gradient); the lines and slabs as they are
b2 = np.array([[0, 2], [3, 1]])
bayer = b2
for _ in range(2):
    bayer = np.block([[4 * bayer + b2[0, 0], 4 * bayer + b2[0, 1]], [4 * bayer + b2[1, 0], 4 * bayer + b2[1, 1]]])
thr = ((bayer + 0.5) / 64.0)[np.arange(H)[:, None] % 8, np.arange(W)[None, :] % 8]
def quant(v, step, keep):
    d = np.floor((v + (thr - 0.5) * step * keep) / step + 0.5)
    return d.clip(0, 255 // step).astype(np.uint32)
flat = np.asarray(bg).astype(np.float32)
src = exact * (open_[..., None] > 0.99) + flat * (open_[..., None] <= 0.99)
keep = (open_ > 0.99).astype(np.float32)
r5, g6, b5 = quant(src[..., 0], 255 / 31, keep), quant(src[..., 1], 255 / 63, keep), quant(src[..., 2], 255 / 31, keep)
rgb565 = (r5 << 11) | (g6 << 5) | b5
if PREVIEW:
    Image.fromarray(np.stack([(r5 * 255 + 15) // 31, (g6 * 255 + 31) // 63, (b5 * 255 + 15) // 31], -1)
                    .astype(np.uint8)).save(PREVIEW)  # (as the screen shows it)
fbo = np.zeros(W * H, np.uint16)
for x in range(W):
    fbo[x * 240:(x + 1) * 240] = rgb565[::-1, x]  # (the column from the bottom row up)

# --- the header ---------------------------------------------------------------------------------------------------------
blob = bytearray()
table = []
for (w, h, ox, oy, adv, data) in pieces:
    table.append((len(blob), w, h, ox, oy, adv))
    blob += data


def hexrows(vals, fmt, per):
    rows = []
    for i in range(0, len(vals), per):
        rows.append('    ' + ','.join(fmt % v for v in vals[i:i + per]) + ',')
    return '\n'.join(rows)


o = ['// Generated by tools/ui_assets.py (stage 9: the bottom screen\'s look "B - the dark"): do not edit.',
     '// Noto Serif JP (SIL Open Font License 1.1: licenses/OFL-1.1.txt): its glyphs drawn into coverage here.',
     '#pragma once', '', '#include <stdint.h>', '']
for k_, v in L.items():
    o.append('#define UI_%s %d' % (k_, v))
o.append('')
o.append('enum { ' + ', '.join('UI_LANG_' + l.upper() for l in LANGS) + ', UI_LANGS };')
o.append('enum { ' + ', '.join('UI_' + s[0] for s in STRINGS) + ', UI_STRINGS };')
o.append('enum { ' + ', '.join('UI_T_' + t[0] for t in TEMPLATES) + ', UI_TEMPLATES };')
o.append('enum { ' + ', '.join('UI_DS_' + d[0] for d in DSTYLES) + ', UI_DSTYLES };')
o.append('#define UI_DIGITS "%s"  // (UTF-8: the glyphs of a style, in this order; a char not found - none)' % DIGITS)
o.append('#define UI_PULSE_LEON %d' % pulse_leon)
o.append('#define UI_PULSE_ASHLEY %d' % pulse_ashley)
o.append('#define UI_SHAPE_CHECK %d' % shape_check)
o.append('#define UI_SHAPE_ARROW_L %d' % shape_arrow_l)
o.append('#define UI_SHAPE_ARROW_R %d' % shape_arrow_r)
o.append('')
o.append('struct UiPiece {\n    uint32_t off;  // into kUiCov\n    uint16_t w, h;\n    int16_t ox, oy;  // the box from the baseline\'s left end\n    int16_t adv;\n};')
o.append('static const UiPiece kUiPiece[%d] = {' % len(table))
o.append('\n'.join('    {%d, %d, %d, %d, %d, %d},' % t for t in table))
o.append('};')
o.append('static const int16_t kUiStr[UI_LANGS][UI_STRINGS] = {')
for li in range(len(LANGS)):
    o.append('    {' + ', '.join(str(str_idx[s[0]][li]) for s in STRINGS) + '},')
o.append('};')
o.append('// the templates: up to 8 tokens (a piece, -1 the argument 0, -2 the argument 1), the count first')
o.append('static const int16_t kUiTpl[UI_LANGS][UI_TEMPLATES][9] = {')
for li in range(len(LANGS)):
    row = []
    for t in TEMPLATES:
        toks = tpl_tokens[t[0]][li]
        row.append('{' + ', '.join(str(v) for v in [len(toks)] + toks + [0] * (8 - len(toks))) + '}')
    o.append('    {' + ', '.join(row) + '},')
o.append('};')
o.append('static const int16_t kUiDigit[UI_DSTYLES][%d] = {' % len(DIGITS))
for d in digit_idx:
    o.append('    {' + ', '.join(str(v) for v in d) + '},')
o.append('};')
o.append('static const uint8_t kUiCov[%d] = {' % len(blob))
o.append(hexrows(list(blob), '0x%02x', 32))
o.append('};')
o.append('// the background: RGB565, the framebuffer\'s order (x * 240 + 239 - y)')
o.append('static const uint16_t kUiBg[320 * 240] = {')
o.append(hexrows([int(v) for v in fbo], '0x%04x', 24))
o.append('};')
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'w', encoding='utf-8') as fh:
    fh.write('\n'.join(o) + '\n')
print('ok: %s (%d pieces, %d KB of coverage, background %d KB)' % (OUT, len(table), len(blob) >> 10, W * H * 2 >> 10))
