// Stage 9 (2026-11-08, the project owner's wish: "the bottom screen looks like Minecraft - it's a horror game"; their choice of
// the mockups: "B - the dark"): the game's bottom screen drawn from pictures made on the Mac (3ds/ui_assets.h by
// tools/ui_assets.py): the background by one copy, the text as coverage blended in its colour (every string
// in en fr de es it ja - Noto Serif JP, one face for all; ru until R0 2026-11-12), the numbers by glyphs, the heartbeats coloured by the life.
// By the panel's thread (settings_3ds.cpp drawGame -> re4_ui_game; the CPU, no GPU), into its picture of the screen.
//
// The language: RE4_3DS_LANG=en|fr|de|es|it|ja, else the game's own (pSys->language, runtime/cheats.cpp hudSample:
// 0 ja, 1/2/7 en, 3 de, 4 fr, 5 es, 6 it). RE4_3DS_UI=0 - the look of before (stage 8).
// Also the minimap's text (3ds/minimap_3ds.cpp): re4_ui_text, re4_ui_tpl, within re4_ui_clip.
// R2 (2026-11-12): the settings before the gameplay (3ds/setup_3ds.cpp) - re4_ui_setup.
#include <3ds.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_assets.h"
#include "hud_font.h"     // the panel's 8x8 font: the version
#include "re4_version.h"  // build3ds.py: RE4_3DS_VERSION

extern "C" {
// runtime/cheats.cpp hudSample (the game's thread, each frame)
extern volatile int re4_hud_life, re4_hud_life_max, re4_hud_ashley, re4_hud_ashley_max, re4_hud_ammo, re4_hud_reserve,
    re4_hud_peseta, re4_hud_lang;
int re4_3ds_minimap_draw(unsigned short* fb, int x, int y, int w, int h);
int re4_3ds_minimap_shot_due(void);  // (the stand: RE4_3DS_MAP_SHOT)
}

namespace {

constexpr u16 rgb(int r, int g, int b)
{
    return (u16) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

const u16 kBone = rgb(212, 204, 192), kBoneLit = rgb(240, 234, 224), kDim = rgb(122, 112, 104),
          kBtnText = rgb(160, 150, 140), kGold = rgb(196, 160, 90), kRed = rgb(200, 36, 28);

int gClip[4] = {0, 0, 320, 240};
u32 gCode[16];  // the codepoints of UI_DIGITS
int gCodes;

inline u16 blend(u16 d, u16 c, u32 a)
{
    if (a >= 250) {
        return c;
    }
    const u32 b = 256 - a;
    a += 1;
    const u32 r = ((d >> 11) * b + (c >> 11) * a) >> 8;
    const u32 g = (((d >> 5) & 63) * b + ((c >> 5) & 63) * a) >> 8;
    const u32 bl = ((d & 31) * b + (c & 31) * a) >> 8;
    return (u16) (r << 11 | g << 5 | bl);
}

// a piece's coverage in `c` with its baseline's left end at (x, base); the columns from `from` to `to` (of the piece) only
void blit(u16* fb, int piece, int x, int base, u16 c, int from = 0, int to = 1 << 16)
{
    const UiPiece& p = kUiPiece[piece];
    const int x0 = x + p.ox, y0 = base + p.oy;
    const u8* cov = kUiCov + p.off;
    for (int i = from < 0 ? 0 : from; i < p.w && i < to; i++) {
        const int sx = x0 + i;
        if (sx < gClip[0] || sx >= gClip[2]) {
            continue;
        }
        u16* col = fb + sx * 240 + 239;
        for (int j = 0; j < p.h; j++) {
            const u32 a = cov[j * p.w + i];
            const int sy = y0 + j;
            if (a && sy >= gClip[1] && sy < gClip[3]) {
                col[-sy] = blend(col[-sy], c, a);
            }
        }
    }
}

void fill(u16* fb, int x, int y, int w, int h, u16 c)
{
    for (int i = x; i < x + w; i++) {
        if (i < 0 || i >= 320) {
            continue;
        }
        for (int j = y; j < y + h; j++) {
            if (j >= 0 && j < 240) {
                fb[i * 240 + 239 - j] = c;
            }
        }
    }
}

int lang()
{
    static int env = -2;
    if (env == -2) {
        env = -1;
        static const char* const kCodes[UI_LANGS] = {"en", "fr", "de", "es", "it", "ja"};
        if (const char* v = getenv("RE4_3DS_LANG")) {
            for (int i = 0; i < UI_LANGS; i++) {
                if (!strncmp(v, kCodes[i], 2)) {
                    env = i;
                }
            }
        }
    }
    if (env >= 0) {
        return env;
    }
    switch (re4_hud_lang) {  // (pSys->language)
    case 0:
        return UI_LANG_JA;
    case 3:
        return UI_LANG_DE;
    case 4:
        return UI_LANG_FR;
    case 5:
        return UI_LANG_ES;
    case 6:
        return UI_LANG_IT;
    }
    return UI_LANG_EN;
}

int glyph(int style, u32 code)
{
    if (!gCodes) {
        const unsigned char* s = (const unsigned char*) UI_DIGITS;
        while (*s && gCodes < 16) {
            u32 v = *s++;
            if ((v & 0xE0) == 0xC0) {
                v = (v & 0x1F) << 6 | (*s++ & 0x3F);
            } else if ((v & 0xF0) == 0xE0) {
                v = (v & 0x0F) << 12 | (s[0] & 0x3F) << 6 | (s[1] & 0x3F);
                s += 2;
            }
            gCode[gCodes++] = v;
        }
    }
    for (int i = 0; i < gCodes; i++) {
        if (gCode[i] == code) {
            return kUiDigit[style][i];
        }
    }
    return -1;
}

int numWidth(int style, const char* s)
{
    int w = 0;
    for (; *s; s++) {
        const int g = glyph(style, (u8) *s);
        w += g >= 0 ? kUiPiece[g].adv : 0;
    }
    return w;
}

// a number (ASCII digits, '/', ' ', '-') in a style; align 0 left, 1 centre, 2 right; its width
int number(u16* fb, int style, const char* s, int x, int base, int align, u16 c, bool shadow)
{
    const int w = numWidth(style, s);
    x -= align == 2 ? w : align == 1 ? w / 2 : 0;
    for (; *s; s++) {
        const int g = glyph(style, (u8) *s);
        if (g < 0) {
            continue;
        }
        if (shadow) {
            blit(fb, g, x + 1, base + 1, 0);
        }
        blit(fb, g, x, base, c);
        x += kUiPiece[g].adv;
    }
    return w;
}

int pieceText(u16* fb, int piece, int x, int base, int align, u16 c, bool shadow)
{
    const int w = kUiPiece[piece].adv;
    x -= align == 2 ? w : align == 1 ? w / 2 : 0;
    if (shadow) {
        blit(fb, piece, x + 1, base + 1, 0);
    }
    blit(fb, piece, x, base, c);
    return w;
}

// a heartbeat (UI_PULSE_*) from x, its columns from `skip` on (narrowed by a long label), bright up to the life's share
void heartbeat(u16* fb, int piece, int x, int mid, int skip, int life, int max)
{
    const int w = kUiPiece[piece].w;
    const float share = max > 0 ? (life <= 0 ? 0.0f : life >= max ? 1.0f : (float) life / max) : 0.0f;
    const u16 c = share > 0.5f ? rgb(70, 200, 90) : share > 0.25f ? rgb(220, 190, 60) : rgb(220, 40, 30);
    const u16 dimmed = share > 0.5f ? rgb(22, 52, 28) : share > 0.25f ? rgb(56, 48, 18) : rgb(60, 14, 12);
    const int cut = skip + (int) ((w - skip) * share + 0.5f);
    blit(fb, piece, x - skip, mid, c, skip, cut);  // (the piece's box: its middle row on the baseline)
    blit(fb, piece, x - skip, mid, dimmed, cut, w);
}

void groupThousands(int v, char* out)
{
    char t[16];
    const int n = snprintf(t, sizeof(t), "%d", v < 0 ? 0 : v);
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) {
            out[o++] = ' ';
        }
        out[o++] = t[i];
    }
    out[o] = 0;
}

}  // namespace

// --- for the minimap (3ds/minimap_3ds.cpp; the panel's thread) ----------------------------------------------------------

extern "C" void re4_ui_clip(int x0, int y0, int x1, int y1)
{
    gClip[0] = x0, gClip[1] = y0, gClip[2] = x1, gClip[3] = y1;
}

// a string of the screen (UI_* of ui_assets.h) in the language; align 0 left, 1 centre, 2 right; its width
extern "C" int re4_ui_text(unsigned short* fb, int id, int x, int base, int align, unsigned short c, int shadow)
{
    if (id < 0 || id >= UI_STRINGS) {
        return 0;
    }
    return pieceText(fb, kUiStr[lang()][id], x, base, align, c, shadow != 0);
}

// a template (UI_T_*) with its two numbers (in UI_DS_SMALL)
extern "C" int re4_ui_tpl(unsigned short* fb, int tpl, int a0, int a1, int x, int base, int align, unsigned short c,
                          int shadow)
{
    if (tpl < 0 || tpl >= UI_TEMPLATES) {
        return 0;
    }
    const int16_t* t = kUiTpl[lang()][tpl];
    char num[2][12];
    snprintf(num[0], sizeof(num[0]), "%d", a0);
    snprintf(num[1], sizeof(num[1]), "%d", a1);
    int w = 0;
    for (int k = 1; k <= t[0]; k++) {
        w += t[k] >= 0 ? kUiPiece[t[k]].adv : numWidth(UI_DS_SMALL, num[-t[k] - 1]);
    }
    x -= align == 2 ? w : align == 1 ? w / 2 : 0;
    for (int k = 1; k <= t[0]; k++) {
        if (t[k] >= 0) {
            x += pieceText(fb, t[k], x, base, 0, c, shadow != 0);
        } else {
            x += number(fb, UI_DS_SMALL, num[-t[k] - 1], x, base, 0, c, shadow != 0);
        }
    }
    return w;
}

// --- the game's screen ---------------------------------------------------------------------------------------------------

// The whole bottom screen of the game into `fb` (320x240, the framebuffer's order); `pressed` the button under the finger
// (0 the case, 1 the map, -1 none). 0: the look of before asked for (RE4_3DS_UI=0) - nothing drawn
extern "C" int re4_ui_game(unsigned short* fb, int pressed)
{
    static int on = -1;
    if (on < 0) {
        const char* v = getenv("RE4_3DS_UI");
        on = v && *v == '0' ? 0 : 1;
    }
    if (!on) {
        return 0;
    }
    memcpy(fb, kUiBg, sizeof(kUiBg));
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const int life = re4_hud_life;
    if (life >= 0) {
        // the lives: the label, the heartbeat (after the longer label), the number
        const int ashley = re4_hud_ashley;
        int labelEnd = UI_LABEL_X + kUiPiece[kUiStr[lg][UI_LEON]].adv;
        if (ashley >= 0) {
            const int e = UI_LABEL_X + kUiPiece[kUiStr[lg][UI_ASHLEY]].adv;
            labelEnd = e > labelEnd ? e : labelEnd;
        }
        const int px = labelEnd + 8 > UI_PULSE_X ? labelEnd + 8 : UI_PULSE_X;
        const int skip = px - UI_PULSE_X;
        char t[24];
        pieceText(fb, kUiStr[lg][UI_LEON], UI_LABEL_X, UI_LEON_BASE, 0, kBone, true);
        heartbeat(fb, UI_PULSE_LEON, px, UI_PULSE_LEON_Y, skip, life, re4_hud_life_max);
        snprintf(t, sizeof(t), "%d", life);
        number(fb, UI_DS_SMALL, t, UI_NUM_R, UI_LEON_BASE, 2, kDim, false);
        if (ashley >= 0) {
            pieceText(fb, kUiStr[lg][UI_ASHLEY], UI_LABEL_X, UI_ASHLEY_BASE, 0, kBone, true);
            heartbeat(fb, UI_PULSE_ASHLEY, px, UI_PULSE_ASHLEY_Y, skip, ashley, re4_hud_ashley_max);
            snprintf(t, sizeof(t), "%d", ashley);
            number(fb, UI_DS_SMALL, t, UI_NUM_R, UI_ASHLEY_BASE, 2, kDim, false);
        }
        // the ammo: loaded large, the reserve after it; "—" without a counter
        pieceText(fb, kUiStr[lg][UI_AMMO], UI_LABEL_X, UI_LABEL2_BASE, 0, kDim, false);
        const int ammo = re4_hud_ammo, reserve = re4_hud_reserve;
        if (ammo >= 0) {
            snprintf(t, sizeof(t), "%d", ammo);
            const int w = number(fb, UI_DS_BIG, t, UI_LABEL_X, UI_NUM_BASE, 0, ammo == 0 ? kRed : kBone, true);
            if (reserve >= 0) {
                snprintf(t, sizeof(t), "/ %d", reserve);
                number(fb, UI_DS_MID, t, UI_LABEL_X + w + 4, UI_NUM_BASE, 0, kDim, true);
            }
        } else {
            const int g = glyph(UI_DS_BIG, 0x2014);
            if (g >= 0) {
                blit(fb, g, UI_LABEL_X, UI_NUM_BASE, kDim);
            }
        }
        // the pesetas (grouped by thousands)
        pieceText(fb, kUiStr[lg][UI_PTAS], UI_NUM_R, UI_LABEL2_BASE, 2, kDim, false);
        groupThousands(re4_hud_peseta, t);
        number(fb, UI_DS_PTAS, t, UI_NUM_R, UI_NUM_BASE, 2, kGold, true);
    }
    // the map, or why not
    if (!re4_3ds_minimap_draw(fb, UI_MAP_X, UI_MAP_Y, UI_MAP_W, UI_MAP_H)) {
        fill(fb, UI_MAP_X, UI_MAP_Y, UI_MAP_W, UI_MAP_H, 0x0000);
        re4_ui_clip(0, 0, 320, 240);
        pieceText(fb, kUiStr[lg][UI_NOMAP], UI_MAP_X + UI_MAP_W / 2, UI_MAP_Y + UI_MAP_H / 2 + 4, 1, kDim, false);
    }
    re4_ui_clip(0, 0, 320, 240);
    // the buttons: lit while the finger is on them (the inside lighter, the line beneath red)
    for (int i = 0; i < 2; i++) {
        const int x0 = i ? UI_BTN1_X : UI_BTN0_X;
        const bool lit = pressed == i;
        if (lit) {
            fill(fb, x0 + 1, UI_BTN_Y + 1, UI_BTN_W - 2, UI_BTN_H - 5, rgb(40, 30, 30));
            fill(fb, x0 + 1, UI_BTN_Y + UI_BTN_H - 3, UI_BTN_W - 2, 2, rgb(170, 22, 18));
        }
        pieceText(fb, kUiStr[lg][i ? UI_MAP : UI_CASE], x0 + UI_BTN_W / 2, UI_BTN_BASE, 1, lit ? kBoneLit : kBtnText,
                  true);
    }
    if (re4_3ds_minimap_shot_due()) {  // (the stand: the whole screen, the format of RE4_3DS_HUD_SHOT)
        if (FILE* f = fopen("sdmc:/3ds/pueblo/hud.fb", "wb")) {
            fwrite(fb, 2, 320 * 240, f);
            fclose(f);
        }
    }
    return 1;
}

// --- R2: the settings before the gameplay ---------------------------------------------------------------------------------

namespace {

// the game screen's gradient without its lines, slabs and frame: the formula and the dither of stage9_ui_assets.py
// background() (made here, not a second picture: 150 KB less in the image; drawn only when the settings change)
void gradient(u16* fb)
{
    static float vig[257];  // the vignette by the distance 0..1.6 in 256 steps
    static u8 bayer[8][8];
    static bool made;
    if (!made) {
        for (int i = 0; i <= 256; i++) {
            float d = i * (1.6f / 256.0f) - 0.45f;
            d = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
            vig[i] = 1.0f - 0.45f * powf(d, 1.3f);
        }
        static const u8 b2[2][2] = {{0, 2}, {3, 1}};
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                bayer[y][x] = (u8) (16 * b2[y & 1][x & 1] + 4 * b2[(y >> 1) & 1][(x >> 1) & 1] + b2[y >> 2][x >> 2]);
            }
        }
        made = true;
    }
    const float stepRB = 255.0f / 31.0f, stepG = 255.0f / 63.0f;
    for (int y = 0; y < 240; y++) {
        const float f = powf(y / 239.0f, 0.8f);
        const float r0 = 34.0f + (7.0f - 34.0f) * f, g0 = 17.0f + (6.0f - 17.0f) * f, b0 = 16.0f + (6.0f - 16.0f) * f;
        const float dy = (y - 96.0f) / 180.0f;
        for (int x = 0; x < 320; x++) {
            const float dx = (x - 160.0f) / 160.0f;
            int i = (int) (sqrtf(dx * dx + dy * dy) * (256.0f / 1.6f));
            const float v = vig[i > 256 ? 256 : i];
            const float t = (bayer[y & 7][x & 7] + 0.5f) / 64.0f - 0.5f;
            int r = (int) floorf((r0 * v + t * stepRB) / stepRB + 0.5f);
            int g = (int) floorf((g0 * v + t * stepG) / stepG + 0.5f);
            int b = (int) floorf((b0 * v + t * stepRB) / stepRB + 0.5f);
            r = r < 0 ? 0 : r > 31 ? 31 : r;
            g = g < 0 ? 0 : g > 63 ? 63 : g;
            b = b < 0 ? 0 : b > 31 ? 31 : b;
            fb[x * 240 + 239 - y] = (u16) (r << 11 | g << 5 | b);
        }
    }
}

void outline(u16* fb, int x, int y, int w, int h, u16 c)
{
    fill(fb, x, y, w, 1, c);
    fill(fb, x, y + h - 1, w, 1, c);
    fill(fb, x, y, 1, h, c);
    fill(fb, x + w - 1, y, 1, h, c);
}

}  // namespace

// (2026-11-19) The settings screen's tabs (more rows than a page holds): CHEATS, CONTROLS, SCREEN - each its rows, as
// the indices of setup_3ds.cpp's values (0..4 the cheats and the money, 5 the gyro, 6 the frame counter, 7 the circle
// pad's sensitivity, 8 the picture); -1 ends a tab
// (2026-11-21: 9 the button mapping's row on CONTROLS - a link to its page, re4_ui_buttons; 10 the Circle Pad's dead zone)
const int kTabRows[3][5] = {{0, 1, 2, 3, 4}, {5, 7, 10, 9, -1}, {8, 6, -1, -1, -1}};

// The top of a tab's `slot`th row - also for the touch (setup_3ds.cpp)
extern "C" int re4_ui_setup_row_y(int slot)
{
    return UI_S_ROW1_Y + slot * UI_S_ROW_H;
}

// What is under (x, y) with the tab `tab` open: a value's row (0..8), -2 - t the tab t, -1 nothing; *left: on an arrow
// row's left arrow (from halfway to the label's end up to halfway to the value)
extern "C" int re4_ui_setup_hit(int tab, int x, int y, int* left)
{
    *left = 0;
    if (x < 8 || x >= 312) {
        return -1;
    }
    if (y >= UI_S_TAB_Y && y < UI_S_TAB_Y + UI_S_TAB_H) {
        return -2 - (x - 8) * 3 / 304;
    }
    tab = tab < 0 || tab > 2 ? 0 : tab;
    for (int s = 0; s < 5 && kTabRows[tab][s] >= 0; s++) {
        const int y0 = re4_ui_setup_row_y(s);
        if (y >= y0 && y < y0 + UI_S_ROW_H) {
            const int i = kTabRows[tab][s];
            *left = (i == 5 || i == 7 || i == 8 || i == 10) && x >= UI_S_ARROW_L - 28 &&
                    x < (UI_S_ARROW_L + UI_S_GAIN_X) / 2;  // (2026-11-21: 10 the dead zone)
            return i;
        }
    }
    return -1;
}

// The build's version (RE4_3DS_VERSION, ASCII) in the panel's 8x8 font, ending at `right`, on the baseline `base`;
// its left end never left of x 168 (the titles' room): a longer text loses its first characters
void versionText(u16* fb, int right, int base, u16 c)
{
    const char* s = RE4_3DS_VERSION;
    int n = (int) strlen(s);
    const int room = (right - 168) / 8;
    if (n > room) {
        s += n - room;
        n = room;
    }
    const int x0 = right - n * 8, y0 = base - 7;
    for (int i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char) s[i];
        const unsigned char* glyph = &kHudFont[(ch >= 0x20 && ch < 0x7F ? ch : '?') * 8];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (0x80 >> col)) {
                    fill(fb, x0 + i * 8 + col, y0 + row, 1, 1, c);
                }
            }
        }
    }
}

// The whole settings screen into `fb` with the tab `tab` open: v[0..4] the cheats and the money (0 / 1), v[5] the gyro's
// gain x10 (0 off, 10..40), v[6] the frame counter, v[7] the circle pad's step, v[8] the picture; `lit` the value's row
// under the finger (-1 none)
extern "C" void re4_ui_setup(unsigned short* fb, const int* v, int lit, int tab)
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const u16 blood = rgb(90, 10, 10), bloodLit = rgb(170, 22, 18), slab = rgb(40, 30, 30);
    tab = tab < 0 || tab > 2 ? 0 : tab;
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    pieceText(fb, kUiStr[lg][UI_S_TITLE], 12, UI_S_TITLE_BASE, 0, kBone, true);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);  // (R7: the build's version; was UI_S_VERSION)
    // (2026-11-19) the tabs: the open one on a slab with a blood-red underline, the others dim over a hairline
    static const int kTab[3] = {UI_S_TAB_CHEATS, UI_S_TAB_CONTROLS, UI_S_TAB_SCREEN};
    for (int t = 0; t < 3; t++) {
        const int x0 = 8 + t * 304 / 3, x1 = 8 + (t + 1) * 304 / 3;
        if (t == tab) {
            fill(fb, x0 + 1, UI_S_TAB_Y, x1 - x0 - 2, UI_S_TAB_H, slab);
            fill(fb, x0 + 1, UI_S_TAB_Y + UI_S_TAB_H - 2, x1 - x0 - 2, 2, bloodLit);
        } else {
            fill(fb, x0 + 1, UI_S_TAB_Y + UI_S_TAB_H - 1, x1 - x0 - 2, 1, blood);
        }
        pieceText(fb, kUiStr[lg][kTab[t]], (x0 + x1) / 2, UI_S_TAB_BASE, 1, t == tab ? kBoneLit : kDim, t == tab);
    }
    static const int kLabel[9] = {UI_S_HEALTH, UI_S_AMMO, UI_S_ONEHIT, UI_S_QTE,  UI_S_MONEY,
                                  UI_S_GYRO,   UI_S_FPS,  UI_S_STICK,  UI_S_PICTURE};
    static const int kStick[4] = {UI_S_G10, UI_S_G12, UI_S_G15, UI_S_G20};
    static const int kPicture[3] = {UI_S_FIT, UI_S_STRETCH, UI_S_ZOOM};
    const int tb = UI_S_ROW_H / 2 + 4;  // (the text's baseline in a row: 16 of 24)
    for (int s = 0; s < 5 && kTabRows[tab][s] >= 0; s++) {
        const int i = kTabRows[tab][s];
        const int y0 = re4_ui_setup_row_y(s);
        if (lit == i) {
            fill(fb, 8, y0 + 1, 304, UI_S_ROW_H - 3, slab);
            fill(fb, 8, y0 + UI_S_ROW_H - 2, 304, 1, bloodLit);
        }
        if (i == 9) {  // (2026-11-21) the button mapping: a link (no value of its own), an arrow to its page
            pieceText(fb, kUiStr[lg][UI_S_BUTTONS], 16, y0 + tb, 0, kBone, true);
            blit(fb, UI_SHAPE_ARROW_R, UI_S_ARROW_R, y0 + UI_S_ROW_H / 2, kBone);
            continue;
        }
        if (i == 10) {  // (2026-11-21) the Circle Pad's dead zone: OFF (dim, as the gyro's) or the percent
            const int dz = v[10];
            pieceText(fb, kUiStr[lg][UI_S_DEADZONE], 16, y0 + tb, 0, lit == i ? kBoneLit : kBone, true);
            blit(fb, UI_SHAPE_ARROW_L, UI_S_ARROW_L, y0 + UI_S_ROW_H / 2, dz ? kBtnText : kDim);
            blit(fb, UI_SHAPE_ARROW_R, UI_S_ARROW_R, y0 + UI_S_ROW_H / 2, dz ? kBtnText : kDim);
            if (dz) {
                char t[8];
                snprintf(t, sizeof(t), "%d%%", dz);
                number(fb, UI_DS_MID, t, UI_S_GAIN_X, y0 + tb, 1, kBoneLit, true);
            } else {
                pieceText(fb, kUiStr[lg][UI_S_OFF], UI_S_GAIN_X, y0 + tb, 1, kDim, false);
            }
            continue;
        }
        const bool on = v[i] != 0;
        // (the owner: a default value is no "off") dim: an unticked box and the gyro off; the stick's 1x, the picture's
        // Fit as bright as any other value
        const bool dim = i == 5 ? v[5] == 0 : i == 7 || i == 8 ? false : !on;
        pieceText(fb, kUiStr[lg][kLabel[i]], 16, y0 + tb, 0, lit == i ? kBoneLit : dim ? kBtnText : kBone, true);
        if (i == 5 || i == 7 || i == 8) {  // the arrows and the value between them
            int id;
            u16 c, arrow = kBtnText;
            bool bright;
            if (i == 5) {  // the gyro: off dim
                const int g = v[5];
                id = g >= 40 ? UI_S_G40 : g >= 30 ? UI_S_G30 : g >= 25 ? UI_S_G25 : g >= 20 ? UI_S_G20 : g >= 15 ? UI_S_G15
                   : g > 0 ? UI_S_G10 : UI_S_OFF;
                c = g ? kBoneLit : kDim, bright = g != 0, arrow = g ? kBtnText : kDim;
            } else if (i == 7) {  // the circle pad's sensitivity: 1x (as it was) plain
                const int st = v[7] < 0 || v[7] > 3 ? 0 : v[7];
                id = kStick[st], c = st ? kBoneLit : kBone, bright = st != 0;
            } else {  // the picture: Fit (as it was) plain
                const int pm = v[8] < 0 || v[8] > 2 ? 0 : v[8];
                id = kPicture[pm], c = pm ? kBoneLit : kBone, bright = pm != 0;
            }
            blit(fb, UI_SHAPE_ARROW_L, UI_S_ARROW_L, y0 + UI_S_ROW_H / 2, arrow);
            blit(fb, UI_SHAPE_ARROW_R, UI_S_ARROW_R, y0 + UI_S_ROW_H / 2, arrow);
            pieceText(fb, kUiStr[lg][id], UI_S_GAIN_X, y0 + tb, 1, c, bright);
            continue;
        }
        const int by = y0 + (UI_S_ROW_H - UI_S_BOX) / 2;
        if (on) {
            fill(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, bloodLit);
            outline(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, rgb(200, 60, 50));
            blit(fb, UI_SHAPE_CHECK, UI_S_BOX_X, by, kBoneLit);
        } else {
            fill(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, rgb(18, 14, 14));
            outline(fb, UI_S_BOX_X, by, UI_S_BOX, UI_S_BOX, rgb(78, 68, 64));
        }
    }
    pieceText(fb, kUiStr[lg][UI_S_FOOT], 160, UI_S_FOOT_BASE, 1, kDim, false);
}

// --- (2026-11-21) the button mapping's page (3ds/setup_3ds.cpp, 3ds/controls_3ds.cpp) ---------------------------------

// its rows: the actions of controls_map.h (0..19), 20 "Reset to defaults"; three pages of seven rows of 22 px (-1: none)
const int kBtnPages = 3;
const int kBtnRows[kBtnPages][7] = {{0, 1, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12, 13}, {14, 15, 16, 17, 18, 19, 20}};
const int kBtnRowH = 22;
const int kBtnLabel[21] = {UI_A_UP,     UI_A_DOWN,  UI_A_LEFT,  UI_A_RIGHT,  UI_A_LUP,   UI_A_LDOWN, UI_A_LLEFT,
                           UI_A_LRIGHT, UI_A_ACTION, UI_A_RUN,  UI_A_TURN,   UI_A_AIM,   UI_A_FIRE,  UI_A_RELOAD,
                           UI_A_KNIFE,  UI_A_SLASH, UI_A_ASHLEY, UI_A_CASE,  UI_A_MAP,   UI_A_OPTIONS, UI_S_RESET};
const int kKeyName[21] = {UI_K_A,     UI_K_B,     UI_K_X,     UI_K_Y,      UI_K_L,     UI_K_R,     UI_K_ZL,
                          UI_K_ZR,    UI_K_UP,    UI_K_DOWN,  UI_K_LEFT,   UI_K_RIGHT, UI_K_START, UI_K_PUP,
                          UI_K_PDOWN, UI_K_PLEFT, UI_K_PRIGHT, UI_K_CUP,   UI_K_CDOWN, UI_K_CLEFT, UI_K_CRIGHT};

// What is under (x, y): a row (0..20), -10 back to the tabs (the band's left half), -11 the other page (its right half),
// -1 nothing
extern "C" int re4_ui_buttons_hit(int page, int x, int y)
{
    if (x < 8 || x >= 312) {
        return -1;
    }
    if (y >= UI_S_TAB_Y && y < UI_S_TAB_Y + UI_S_TAB_H) {
        return x < 160 ? -10 : -11;
    }
    page = page < 0 || page >= kBtnPages ? 0 : page;
    for (int s = 0; s < 7 && kBtnRows[page][s] >= 0; s++) {
        const int y0 = UI_S_ROW1_Y + s * kBtnRowH;
        if (y >= y0 && y < y0 + kBtnRowH) {
            return kBtnRows[page][s];
        }
    }
    return -1;
}

// The page into `fb`: `keys` each action's keys (controls_map.h's ctl::Key, 0xFF none), `catching` the action whose
// keys are awaited (-1 none), `lit` the row under the finger
extern "C" void re4_ui_buttons(unsigned short* fb, int page, int lit, int catching, const unsigned char (*keys)[2])
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = lang();
    const u16 blood = rgb(90, 10, 10), bloodLit = rgb(170, 22, 18), slab = rgb(40, 30, 30);
    page = page < 0 || page >= kBtnPages ? 0 : page;
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    pieceText(fb, kUiStr[lg][UI_S_BTN_TITLE], 12, UI_S_TITLE_BASE, 0, kBone, true);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);
    // the band of the tabs: back to CONTROLS (left), the page and its arrows (right)
    const int mid = UI_S_TAB_Y + UI_S_TAB_H / 2;
    fill(fb, 9, UI_S_TAB_Y + UI_S_TAB_H - 1, 302, 1, blood);
    blit(fb, UI_SHAPE_ARROW_L, 14, mid, kBone);
    pieceText(fb, kUiStr[lg][UI_S_TAB_CONTROLS], 28, UI_S_TAB_BASE, 0, kBone, false);
    blit(fb, UI_SHAPE_ARROW_L, 250, mid, kBtnText);
    char pages[8];
    snprintf(pages, sizeof(pages), "%d/%d", page + 1, kBtnPages);
    number(fb, UI_DS_SMALL, pages, 275, UI_S_TAB_BASE, 1, kBone, false);
    blit(fb, UI_SHAPE_ARROW_R, 296, mid, kBtnText);
    const int tb = kBtnRowH / 2 + 4;
    for (int s = 0; s < 7 && kBtnRows[page][s] >= 0; s++) {
        const int i = kBtnRows[page][s];
        const int y0 = UI_S_ROW1_Y + s * kBtnRowH;
        const bool wait = i == catching;
        if (lit == i || wait) {
            fill(fb, 8, y0 + 1, 304, kBtnRowH - 3, slab);
            fill(fb, 8, y0 + kBtnRowH - 2, 304, 1, bloodLit);
        }
        pieceText(fb, kUiStr[lg][kBtnLabel[i]], 16, y0 + tb, 0, lit == i || wait ? kBoneLit : kBone, true);
        if (i >= 20) {
            continue;  // (the reset: no value)
        }
        if (wait) {
            pieceText(fb, kUiStr[lg][UI_S_PRESS], 304, y0 + tb, 2, kGold, false);
            continue;
        }
        const unsigned char* k = keys[i];
        const int n = k[0] >= 21 ? 0 : k[1] >= 21 ? 1 : 2;
        if (!n) {
            pieceText(fb, kUiStr[lg][UI_K_NONE], 304, y0 + tb, 2, kDim, false);
            continue;
        }
        int w = kUiPiece[kUiStr[lg][kKeyName[k[0]]]].adv;
        if (n == 2) {
            w += kUiPiece[kUiStr[lg][UI_K_PLUS]].adv + kUiPiece[kUiStr[lg][kKeyName[k[1]]]].adv;
        }
        int x = 304 - w;
        x += pieceText(fb, kUiStr[lg][kKeyName[k[0]]], x, y0 + tb, 0, kBoneLit, true);
        if (n == 2) {
            x += pieceText(fb, kUiStr[lg][UI_K_PLUS], x, y0 + tb, 0, kBtnText, false);
            pieceText(fb, kUiStr[lg][kKeyName[k[1]]], x, y0 + tb, 0, kBoneLit, true);
        }
    }
    pieceText(fb, kUiStr[lg][UI_S_BTN_FOOT], 160, UI_S_FOOT_BASE, 1, kDim, false);
}

// --- R3: the checks at the start ------------------------------------------------------------------------------------------

// The whole screen into `fb` (3ds/startcheck_3ds.cpp; English only, R3): `title` and `foot` strings (UI_C_T_*, UI_C_F_*),
// `lines` the problems' strings: a heading with RE4_UI_HEAD (UI_C_H_*) and its lines under it
extern "C" void re4_ui_check(unsigned short* fb, int title, const int* lines, int n, int foot)
{
    gradient(fb);
    re4_ui_clip(0, 0, 320, 240);
    const int lg = UI_LANG_EN;
    const u16 blood = rgb(90, 10, 10);
    fill(fb, 8, UI_S_LINE1_Y, 305, 1, blood);
    fill(fb, 8, UI_S_LINE2_Y, 305, 1, blood);
    pieceText(fb, kUiStr[lg][title], 12, UI_S_TITLE_BASE, 0, title == UI_C_T_STOP ? kRed : kBone, true);
    versionText(fb, 308, UI_S_TITLE_BASE, kDim);  // (R7: the build's version; was UI_S_VERSION)
    int y = 50;
    for (int i = 0; i < n; i++) {
        const int id = lines[i] & 0xFFF;
        if (id < 0 || id >= UI_STRINGS) {
            continue;
        }
        if (lines[i] & 0x1000) {  // (RE4_UI_HEAD) a heading: a gap above it but the first
            y += i ? 10 : 0;
            pieceText(fb, kUiStr[lg][id], 14, y, 0, kBoneLit, true);
            y += 18;
        } else {
            pieceText(fb, kUiStr[lg][id], 20, y, 0, kBtnText, false);
            y += 15;
        }
    }
    pieceText(fb, kUiStr[lg][foot], 160, UI_S_FOOT_BASE, 1, kBone, false);
}
