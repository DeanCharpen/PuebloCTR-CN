// R2 (2026-11-12; the owner's decisions R0 / R2 and their choice of the mockup): the bottom screen before the gameplay - the settings. The cheats one by one
// (runtime/cheats.cpp bits 1 health, 2 ammo, 4 one-hit kill, 8 automatic QTEs), +1 000 000 pesetas once a launch
// (runtime/cheats.cpp re4_cheats_money_arm), the gyro aim's gain (input_3ds.cpp re4_3ds_gyro_set; on 2x by default),
// the frame counter in the game screen's corner (off by default).
//
// Shown until the gameplay starts and again when the game is back at its title (STA_TITLE: from the logos to the
// title's exit, again after "quit" or a reset; runtime/cheats.cpp hudSample re4_hud_title), drawn by the panel's
// thread (settings_3ds.cpp hudThread -> 3ds/ui_bottom_3ds.cpp re4_ui_setup), changed by touch (the game's thread,
// settings_3ds.cpp re4_3ds_hud_pad): a row - its box; the gyro - the left arrow less, elsewhere more (4x -> off).
// Taken when the title is left for the gameplay (a new game, a load, the extra games; the game's thread, its frame
// hook): only after the title was seen - the runs that start in a room (the benches, RE4_START_ROOM) take nothing, and
// pueblo.cfg's RE4_CHEATS / RE4_GYRO / RE4_3DS_HUD rule them as before. The cheats are not changed during the gameplay
// (R0: fewer new faults). RE4_3DS_SETUP=0: no settings screen, nothing taken (the game's screen at the title, as before);
// off by default under a replay or a pad script (RE4_PAD_REPLAY / RE4_PAD_SCRIPT: the benches, the start in r100).
//
// Kept in sdmc:/3ds/pueblo/settings.bin (16 bytes, a checksum; bad or missing - the defaults), written by the panel's
// thread half a second after the last change: only the settings screen changes it, so at the title (a write holds the
// game's reads of the SD card, 6.2e; rename over a file does not work there - written in place).
#include <3ds.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
extern volatile int re4_hud_title, re4_hud_life, re4_hud_lang;  // runtime/cheats.cpp hudSample
void re4_add_frame_hook(void (*hook)(unsigned));                // runtime/pad.cpp
void re4_cheats_set(int bits);                                  // runtime/cheats.cpp
int re4_cheats_wanted(void);
void re4_cheats_money_arm(void);                                // runtime/cheats.cpp (R2)
void re4_3ds_gyro_set(int gain10);                              // input_3ds.cpp (R2)
void re4_3ds_stick_set(int index);                              // input_3ds.cpp (2026-11-18)
void re4_3ds_gx_fill(int mode);                                 // gx_3ds.cpp (2026-11-19)
void re4_3ds_hud_fps(int on);                                   // settings_3ds.cpp (R2)
void re4_ui_setup(unsigned short* fb, const int* v, int lit, int tab);  // ui_bottom_3ds.cpp (R2; tabs 2026-11-19)
int re4_ui_setup_hit(int tab, int x, int y, int* left);
void re4_log(const char* fmt, ...);
}

namespace {

const char kPath[] = "sdmc:/3ds/pueblo/settings.bin";
const int kRows = 9, kGyroRow = 5, kStickRow = 7;  // (2026-11-18: row 7 the circle pad's sensitivity, 0..3)
const int kStickSteps = 4;
const int kPictureRow = 8, kPictureSteps = 3;  // (2026-11-19: row 8 the picture, 0 Fit 1 Stretch 2 Zoom)
const int kGains[] = {0, 10, 15, 20, 25, 30, 40};
const int kGainCount = (int) (sizeof(kGains) / sizeof(kGains[0]));
const int kDefaults[kRows] = {0, 0, 0, 0, 0, 20, 0, 0, 0};  // (R0: the cheats off, the gyro on 2x, the counter off; the stick 1x; Fit)

struct File {
    char magic[4];  // "PCS1"
    u8 version;     // 1
    u8 cheats;      // bits 1 2 4 8
    u8 money;       // +1 000 000 pesetas once a launch
    u8 gyro;        // the gain x10 (0 off)
    u8 fps;         // the frame counter
    u8 stick;       // (2026-11-18; padding before: 0) the circle pad's sensitivity 0..3: 1x 1.25x 1.5x 2x
    u8 picture;     // (2026-11-19; padding before: 0) the picture 0..2: Fit, Stretch, Zoom
    u8 pad[1];
    u32 sum;        // FNV-1a of the bytes before
};
static_assert(sizeof(File) == 16, "settings.bin is 16 bytes");

int gV[kRows] = {0, 0, 0, 0, 0, 20, 0, 0, 0};  // the screen's values: the boxes 0/1, the gyro's gain x10, the steps
volatile int gLit = -1;                  // the row under the finger
volatile int gTab;                       // (2026-11-19) the open tab: 0 CHEATS (at each launch), 1 CONTROLS, 2 SCREEN
bool gOn;                                // the screen (RE4_3DS_SETUP)
volatile bool gInGame;                   // the title left for the gameplay
bool gSawTitle;
volatile bool gChanged = true;           // a new picture due (a value, the finger, the screen switched, the language)
volatile u64 gWriteAt;                   // the file due at this tick (0: none)
bool gMoneyGiven;                        // (once a launch)
int gLastLang = -2;

u32 fnv(const u8* p, size_t n)
{
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

int gainIndex(int g)
{
    for (int i = 0; i < kGainCount; i++) {
        if (kGains[i] == g) {
            return i;
        }
    }
    return 3;  // (2x)
}

void load()
{
    memcpy(gV, kDefaults, sizeof(gV));
    File f;
    FILE* fp = fopen(kPath, "rb");
    if (!fp) {
        re4_log("3ds: settings: no %s - the defaults", kPath);
        return;
    }
    const size_t n = fread(&f, 1, sizeof(f), fp);
    fclose(fp);
    if (n != sizeof(f) || memcmp(f.magic, "PCS1", 4) || f.version != 1 ||
        f.sum != fnv((const u8*) &f, offsetof(File, sum))) {
        re4_log("3ds: settings: %s not ours or damaged (%u bytes) - the defaults", kPath, (unsigned) n);
        return;
    }
    for (int i = 0; i < 4; i++) {
        gV[i] = (f.cheats >> i) & 1;
    }
    gV[4] = f.money ? 1 : 0;
    gV[5] = kGains[gainIndex(f.gyro)];
    gV[6] = f.fps ? 1 : 0;
    gV[kStickRow] = f.stick < kStickSteps ? f.stick : 0;
    gV[kPictureRow] = f.picture < kPictureSteps ? f.picture : 0;
}

bool save()
{
    File f;
    memset(&f, 0, sizeof(f));
    memcpy(f.magic, "PCS1", 4);
    f.version = 1;
    f.cheats = (u8) ((gV[0] ? 1 : 0) | (gV[1] ? 2 : 0) | (gV[2] ? 4 : 0) | (gV[3] ? 8 : 0));
    f.money = (u8) (gV[4] ? 1 : 0);
    f.gyro = (u8) gV[5];
    f.fps = (u8) (gV[6] ? 1 : 0);
    f.stick = (u8) gV[kStickRow];
    f.picture = (u8) gV[kPictureRow];
    f.sum = fnv((const u8*) &f, offsetof(File, sum));
    FILE* fp = fopen(kPath, "r+b");  // (in place: the file keeps its cluster)
    if (!fp) {
        fp = fopen(kPath, "wb");
    }
    if (!fp) {
        return false;
    }
    const bool ok = fwrite(&f, 1, sizeof(f), fp) == sizeof(f);
    fclose(fp);
    return ok;
}

// the game's thread: the title left for the gameplay
void take()
{
    const int bits = (gV[0] ? 1 : 0) | (gV[1] ? 2 : 0) | (gV[2] ? 4 : 0) | (gV[3] ? 8 : 0);
    re4_cheats_set((re4_cheats_wanted() & 16) | bits);  // (16: the extra games kept unlocked - not ours)
    const bool money = gV[4] && !gMoneyGiven;
    if (money) {
        gMoneyGiven = true;
        re4_cheats_money_arm();
    }
    re4_3ds_gyro_set(gV[5]);
    re4_3ds_hud_fps(gV[6]);
    re4_3ds_stick_set(gV[kStickRow]);
    re4_3ds_gx_fill(gV[kPictureRow]);
    re4_log("3ds: settings taken by the game: cheats %d%s, gyro %d.%dx, frame counter %s, stick step %d, picture %d", bits,
            money ? ", +1 000 000 pesetas when Leon is free" : gV[4] ? " (the pesetas given this launch)" : "",
            gV[5] / 10, gV[5] % 10, gV[6] ? "on" : "off", gV[kStickRow], gV[kPictureRow]);
}

// the game's thread, each frame (after hudSample: its hook came first)
void frame(unsigned)
{
    const bool title = re4_hud_title != 0;
    gSawTitle = gSawTitle || title;
    if (gInGame && title) {
        gInGame = false;
        gChanged = true;
    } else if (!gInGame && !title && re4_hud_life >= 0) {
        gInGame = true;
        gChanged = true;
        if (gSawTitle) {
            take();
        }
    }
    if (re4_hud_lang != gLastLang) {
        gLastLang = re4_hud_lang;
        gChanged = true;
    }
}

}  // namespace

// settings_3ds.cpp re4_3ds_hud_start (before the game, the main thread): the file, the frame hook
extern "C" void re4_3ds_setup_start(void)
{
    // on by default, but not under a test's buttons (a replay, a pad script: the benches, the start in r100)
    const char* v = getenv("RE4_3DS_SETUP");
    const char* replay = getenv("RE4_PAD_REPLAY");
    const char* script = getenv("RE4_PAD_SCRIPT");
    const bool test = (replay && *replay) || (script && *script);
    gOn = v && *v ? *v != '0' : !test;
    if (!gOn) {
        re4_log("3ds: settings screen off (%s)", v && *v ? "RE4_3DS_SETUP=0" : "a replay or a pad script");
        return;
    }
    load();
    if (const char* t = getenv("RE4_3DS_SETUPTAB")) {  // (2026-11-19, the stand: a tab open from the start, 0..2)
        gTab = atoi(t) < 0 || atoi(t) > 2 ? 0 : atoi(t);
    }
    re4_3ds_gx_fill(gV[kPictureRow]);  // (2026-11-19: the picture at once - the title shows it)
    re4_add_frame_hook(frame);
    re4_log("3ds: settings screen until the gameplay: cheats %d%d%d%d, pesetas %d, gyro %d, frame counter %d, "
            "stick step %d, picture %d", gV[0], gV[1], gV[2], gV[3], gV[4], gV[5], gV[6], gV[kStickRow],
            gV[kPictureRow]);
}

// The panel's thread: the settings screen instead of the game's
extern "C" int re4_3ds_setup_visible(void)
{
    return gOn && !gInGame;
}

// The panel's thread: a new picture due since the last call
extern "C" int re4_3ds_setup_dirty(void)
{
    if (!gChanged) {
        return 0;
    }
    gChanged = false;
    return 1;
}

extern "C" void re4_3ds_setup_draw(unsigned short* fb)
{
    int v[kRows];
    memcpy(v, gV, sizeof(v));
    re4_ui_setup(fb, v, gLit, gTab);
}

// The panel's thread, each pass: the file half a second after the last change
extern "C" void re4_3ds_setup_poll(void)
{
    const u64 at = gWriteAt;
    if (at && svcGetSystemTick() >= at) {
        gWriteAt = 0;
        const bool ok = save();
        re4_log("3ds: settings: %s %s", kPath, ok ? "written" : "NOT written");
    }
}

// The game's thread (settings_3ds.cpp re4_3ds_hud_pad): phase 1 a new touch, 0 held, -1 none. 1: the settings screen
// is up and took it (nothing for the game: the title's pad stays the buttons')
extern "C" int re4_3ds_setup_touch(int phase, int x, int y)
{
    if (!re4_3ds_setup_visible()) {
        if (gLit >= 0) {
            gLit = -1;
        }
        return 0;
    }
    if (phase < 0) {
        if (gLit >= 0) {
            gLit = -1;
            gChanged = true;
        }
        return 1;
    }
    if (phase != 1) {
        return 1;
    }
    int left = 0;
    const int row = re4_ui_setup_hit(gTab, x, y, &left);
    if (row <= -2) {  // (2026-11-19) a tab: opened
        gTab = -2 - row;
        gLit = -1;
        gChanged = true;
        return 1;
    }
    if (row < 0 || row >= kRows) {
        return 1;
    }
    if (row == kGyroRow) {
        int g = gainIndex(gV[kGyroRow]);
        if (left) {
            g = g > 0 ? g - 1 : 0;  // the left arrow
        } else {
            g = (g + 1) % kGainCount;
        }
        gV[kGyroRow] = kGains[g];
    } else if (row == kStickRow) {  // (2026-11-18) the arrows as the gyro's: 2x -> 1x
        const int s = gV[kStickRow];
        gV[kStickRow] = left ? (s > 0 ? s - 1 : 0) : (s + 1) % kStickSteps;
    } else if (row == kPictureRow) {  // (2026-11-19) the same arrows: Zoom -> Fit
        const int s = gV[kPictureRow];
        gV[kPictureRow] = left ? (s > 0 ? s - 1 : 0) : (s + 1) % kPictureSteps;
    } else {
        gV[row] = !gV[row];
    }
    gLit = row;
    gChanged = true;
    gWriteAt = svcGetSystemTick() + SYSCLOCK_ARM11 / 2;
    if (row == kPictureRow) {
        re4_3ds_gx_fill(gV[kPictureRow]);  // (2026-11-19: at once - seen on the title while it is chosen)
    }
    re4_log("3ds: settings: row %d -> %d", row, gV[row]);
    return 1;
}
