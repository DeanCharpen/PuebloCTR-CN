// R3 (2026-11-13): the checks at the start - what would else be a hang or a crash said on the
// bottom screen (English only, the owner: the game's language is not known yet), in the look of the settings screen
// (ui_bottom_3ds.cpp re4_ui_check). The owner's choice "strictly by the matter":
// - the end (START: exit): an Old 3DS / 2DS; too little memory; no data/disc0, or no sys/boot.bin / sys/fst.bin in it;
//   an edition the beta does not play (the debug build, an unknown disc; the Japanese one plays since stage 69,
//   2026-11-21); disc 2 in data/disc0; a
//   data/disc1 that is not disc 2 of the same edition;
// - a warning (A: on, START: exit), all on one screen: no data/disc1 (the game plays up to the disc change); movies
//   missing in movie/ (the CRI player then: an orange screen); no texture pack or one that does not fit (the textures
//   made here: stutters).
// Two steps from main_3ds.cpp, both before any thread of the game (a warning waits for the player as long as he
// likes): re4_3ds_check_system (the Old 3DS, the memory), re4_3ds_check_data (the discs, the movies, the texture pack's
// head - as gx_3ds.cpp openPak reads it - and the warnings shown). The end: re4_3ds_check_exit (_exit, as the game's own
// end in input_3ds.cpp: a return from main runs the static destructors, and they log through the game's objects
// that never ran - an access at 0 on the stand). RE4_3DS_CHECKS: 2 all (the release), 1 the end only - the warnings into the log
// (the other builds: the stand, the benches), 0 none. RE4_3DS_CHECKTEST=old|mem|nodata|bad|jp|debug|unknown|swap|mix|
// nodisc1|nomovie|somemovie|nopak|oldpak[,...] (not in the release): a problem as if found, for the screens.
// The version file of PuebloPrep (data/puebloprep.txt) - with PuebloPrep (R5, the owner's choice).
#include <3ds.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pica_texture.h"
#include "re4_runtime.h"
#include "ui_assets.h"

#ifndef RE4_3DS_RELEASE
#define RE4_3DS_RELEASE 0
#endif

extern "C" {
void re4_log_file_drain(void);  // runtime/log.cpp
void re4_log_file_trim(void);
void re4_ui_check(unsigned short* fb, int title, const int* lines, int n, int foot);  // ui_bottom_3ds.cpp
int re4_3ds_memory_ok(void);                                                          // system_3ds.cpp
}

namespace {

const int kHead = 0x1000;  // (re4_ui_check: a heading)

int gMode = -1;  // RE4_3DS_CHECKS
int lines[16], nLines;
int warns[16], nWarns;

int mode()
{
    if (gMode < 0) {
        const char* v = getenv("RE4_3DS_CHECKS");
        gMode = v ? atoi(v) : 1;
    }
    return gMode;
}

bool test(const char* what)
{
#if RE4_3DS_RELEASE
    (void) what;
    return false;
#else
    const char* v = getenv("RE4_3DS_CHECKTEST");
    if (!v) {
        return false;
    }
    const size_t n = strlen(what);
    for (const char* p = v; *p;) {
        const char* end = strchr(p, ',');
        const size_t len = end ? (size_t) (end - p) : strlen(p);
        if (len == n && !strncmp(p, what, n)) {
            return true;
        }
        p += len + (end ? 1 : 0);
    }
    return false;
#endif
}

void add(int* to, int& n, int id)
{
    if (n < 16) {
        to[n++] = id;
    }
}

void blackTop()  // (before the renderer: whatever the framebuffers held)
{
    for (int i = 0; i < 2; i++) {
        u16 w, h;
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
        memset(fb, 0, (size_t) w * h * 3);
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_TOP, false);
        gspWaitForVBlank();
    }
}

// The screen, then the buttons: 1 A (a warning only), 0 START or the system closing the app
int show(int title, const int* ids, int n, int foot, bool top)
{
    if (top) {
        blackTop();
    }
    u16 w, h;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);  // (the console's: RGB565, one buffer)
    re4_ui_check((unsigned short*) fb, title, ids, n, foot);
    GSPGPU_FlushDataCache(fb, (u32) w * h * 2);
    gfxFlushBuffers();
#if !RE4_3DS_RELEASE
    if (const char* shot = getenv("RE4_3DS_CHECKSHOT")) {  // (the stand, stageR3_stand.sh) the screen to a file; START
        FILE* f = fopen(shot, "wb");
        if (f) {
            fwrite(fb, 1, (size_t) w * h * 2, f);
            fclose(f);
        }
        return 0;
    }
#endif
    hidScanInput();  // (a button held from the launcher does not count)
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_START) {
            return 0;
        }
        if (foot == UI_C_F_WARN && (down & KEY_A)) {
            return 1;
        }
        gspWaitForVBlank();
    }
    return 0;
}

int stop(const char* why, bool top)
{
    re4_log("3ds: the start's check: %s - the end", why);
    show(UI_C_T_STOP, lines, nLines, UI_C_F_STOP, top);
    return 0;
}

// a disc's header: the id (6 bytes), the disc's number (byte 6), the FST's offset (0x424); 0: no file
bool readBoot(const char* root, unsigned char* id, u32* fst)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/sys/boot.bin", root);
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    unsigned char head[0x428];
    const size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (got < 8) {
        return false;
    }
    memcpy(id, head, 8);
    *fst = got >= 0x428 ? (u32) head[0x424] << 24 | (u32) head[0x425] << 16 | (u32) head[0x426] << 8 | head[0x427] : 0;
    return true;
}

bool exists(const char* dir, const char* name)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    return stat(path, &st) == 0;
}

// the movies of PAL and US (stage61_movies.py; r214_ev_jp is the Japanese edition's): NAME.mvd and NAME.pcm
const char* const kMovies[] = {"adaend_c", "adaend_m", "demo0", "demo1", "ending", "opening", "r214_ev", "r229_ev",
                               "r333_ev"};
const int kMovieCount = (int) (sizeof(kMovies) / sizeof(kMovies[0]));

// the movies found in movie/ (one listing of the folder: an fopen each costs ~37 ms on the SD card)
int moviesFound()
{
    DIR* d = opendir("sdmc:/3ds/pueblo/movie");
    if (!d) {
        return 0;
    }
    bool have[kMovieCount][2] = {};
    while (struct dirent* e = readdir(d)) {
        const char* dot = strrchr(e->d_name, '.');
        if (!dot) {
            continue;
        }
        const int kind = !strcasecmp(dot, ".mvd") ? 0 : !strcasecmp(dot, ".pcm") ? 1 : -1;
        if (kind < 0) {
            continue;
        }
        for (int i = 0; i < kMovieCount; i++) {
            const size_t n = strlen(kMovies[i]);
            if ((size_t) (dot - e->d_name) == n && !strncasecmp(e->d_name, kMovies[i], n)) {
                have[i][kind] = true;
            }
        }
    }
    closedir(d);
    int found = 0;
    for (int i = 0; i < kMovieCount; i++) {
        found += have[i][0] && have[i][1];
    }
    return found;
}

}  // namespace

// 1: on; 0: the end (the screen was shown). Before anything else of the game (main_3ds.cpp)
extern "C" int re4_3ds_check_system(void)
{
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    const bool old = !isNew || test("old");
    const bool memOk = re4_3ds_memory_ok() && !test("mem");  // (it prints the bytes missing on the console)
    if (!old && memOk) {
        return 1;
    }
    if (!mode()) {  // (no checks: as before R3, the missing bytes on the console)
        return memOk;
    }
    nLines = 0;
    if (old) {  // (an Old 3DS has too little memory too: this first)
        add(lines, nLines, UI_C_H_OLD | kHead);
        add(lines, nLines, UI_C_OLD1);
        add(lines, nLines, UI_C_OLD2);
    } else {
        add(lines, nLines, UI_C_H_MEM | kHead);
        add(lines, nLines, UI_C_MEM1);
        add(lines, nLines, UI_C_MEM2);
        add(lines, nLines, UI_C_MEM3);
    }
    return stop(old ? "not a New 3DS" : "not enough memory", true);
}

// 1: on; 0: the end. The discs and the movies (before the renderer: the top screen still ours)
extern "C" int re4_3ds_check_data(void)
{
    if (!mode()) {
        return 1;
    }
    const char* dir = getenv("RE4_DATA_DIR");
    if (!dir || !*dir) {
        dir = "sdmc:/3ds/pueblo/data";
    }
    nLines = 0;
    nWarns = 0;
    char disc0[160];
    snprintf(disc0, sizeof(disc0), "%s/disc0", dir);
    struct stat st;
    // (as runtime/dvd.cpp: without a disc0 folder the files right in the data folder - the stand's, a single disc)
    const bool flat = (stat(disc0, &st) != 0 || !S_ISDIR(st.st_mode)) && exists(dir, "sys/boot.bin");
    if (flat) {
        snprintf(disc0, sizeof(disc0), "%s", dir);
    }
    if (stat(disc0, &st) != 0 || !S_ISDIR(st.st_mode) || test("nodata")) {
        add(lines, nLines, UI_C_H_NODATA | kHead);
        add(lines, nLines, UI_C_NODATA1);
        add(lines, nLines, UI_C_NODATA2);
        add(lines, nLines, UI_C_PREP);
        return stop("no data/disc0", true);
    }
    unsigned char id0[8] = {}, id1[8] = {};
    u32 fst0 = 0, fst1 = 0;
    if (!readBoot(disc0, id0, &fst0) || !exists(disc0, "sys/fst.bin") || test("bad")) {
        add(lines, nLines, UI_C_H_BAD | kHead);
        add(lines, nLines, UI_C_BAD1);
        add(lines, nLines, UI_C_PREP);
        return stop("no sys/boot.bin or sys/fst.bin in data/disc0", true);
    }
    // the edition as runtime/dvd.cpp releaseOf: G4BE08 with the FST at 0x28FE00 is the debug build's
    const bool eu = !memcmp(id0, "G4BP08", 6), jp = !memcmp(id0, "G4BJ08", 6), e08 = !memcmp(id0, "G4BE08", 6);
    const bool debug = e08 && id0[6] == 0 && (fst0 == 0x28FE00 || fst0 == 0);
    re4_log("3ds: the start's check: %s %.6s disc %d (FST at 0x%lx): %s", disc0, (const char*) id0, (int) id0[6],
            (unsigned long) fst0, eu ? "Europe" : jp ? "Japan" : debug ? "the debug build" : e08 ? "USA" : "unknown");
    // (stage 69) the Japanese discs play: the core reads them as upstream does (language 0, ss/Jpn, the *_j fonts;
    // their gore as the US release's, patch 0212); RE4_3DS_CHECKTEST=jp still shows the old refusal
    const int edition = test("jp") ? 1 : test("debug") ? 2 : test("unknown") ? 3 : debug ? 2 : eu || e08 || jp ? 0 : 3;
    if (edition) {
        add(lines, nLines, UI_C_H_ED | kHead);
        add(lines, nLines, edition == 1 ? UI_C_ED_JP : edition == 2 ? UI_C_ED_DBG : UI_C_ED_UNK);
        add(lines, nLines, UI_C_ED_OK);
        return stop("an edition the beta does not play", true);
    }
    if (id0[6] != 0 || test("swap")) {
        add(lines, nLines, UI_C_H_SWAP | kHead);
        add(lines, nLines, UI_C_SWAP1);
        add(lines, nLines, UI_C_DISCS);
        return stop("disc 2 in data/disc0", true);
    }
    char disc1[160];
    snprintf(disc1, sizeof(disc1), "%s/disc1", dir);
    const bool has1 = !flat && stat(disc1, &st) == 0 && S_ISDIR(st.st_mode) && !test("nodisc1");
    if (has1) {
        const bool read1 = readBoot(disc1, id1, &fst1) && exists(disc1, "sys/fst.bin");
        re4_log("3ds: the start's check: data/disc1 %.6s disc %d", read1 ? (const char*) id1 : "none", read1 ? (int) id1[6] : -1);
        if (!read1 || memcmp(id0, id1, 6) != 0 || id1[6] != 1 || test("mix")) {
            add(lines, nLines, UI_C_H_MIX | kHead);
            add(lines, nLines, UI_C_MIX1);
            add(lines, nLines, UI_C_DISCS);
            return stop("data/disc1 is not disc 2 of the same edition", true);
        }
    } else {
        add(warns, nWarns, UI_C_H_NO2 | kHead);
        add(warns, nWarns, UI_C_NO2_1);
    }
    const int movies = test("nomovie") ? 0 : test("somemovie") ? kMovieCount - 1 : moviesFound();
    re4_log("3ds: the start's check: %d of %d movies in movie/", movies, kMovieCount);
    if (movies < kMovieCount) {
        add(warns, nWarns, (movies ? UI_C_H_SOMEMOV : UI_C_H_NOMOV) | kHead);
        add(warns, nWarns, UI_C_MOV1);
    }
    // the texture pack's head (gx_3ds.cpp openPak; "made with other options" is left to it: only pueblo.cfg's)
    const char* pak = getenv("RE4_3DS_TEXPAK");
    if (!(pak && *pak == '0')) {
        char path[160];
        pak = pak ? pak : "/3ds/pueblo/textures.pak";
        snprintf(path, sizeof(path), "%s%s", strncmp(pak, "sdmc:", 5) ? "sdmc:" : "", pak);
        re4pica::PakHead head;
        FILE* f = fopen(path, "rb");
        const bool found = f != nullptr;
        const bool fits = f && fread(&head, sizeof(head), 1, f) == 1 && head.magic == re4pica::kPakMagic &&
                          head.version == re4pica::kPakVersion;
        if (f) {
            fclose(f);
        }
        re4_log("3ds: the start's check: the texture pack %s: %s", path, !found ? "missing" : fits ? "fits" : "does not fit");
        if (!found || test("nopak")) {
            add(warns, nWarns, UI_C_H_NOPAK | kHead);
            add(warns, nWarns, UI_C_PAK1);
        } else if (!fits || test("oldpak")) {
            add(warns, nWarns, UI_C_H_OLDPAK | kHead);
            add(warns, nWarns, UI_C_PAK1);
        }
    }
    if (!nWarns) {
        return 1;
    }
    re4_log("3ds: the start's check: %d warning lines%s", nWarns, mode() >= 2 ? ", shown" : " (RE4_3DS_CHECKS=1: not shown)");
    if (mode() < 2) {
        return 1;
    }
    const int on = show(UI_C_T_WARN, warns, nWarns, UI_C_F_WARN, true);
    re4_log("3ds: the start's check: %s", on ? "A - on" : "START - the end");
    if (on) {  // (the console's lines are not drawn again: the bottom screen dark until the game's screen)
        u16 w, h;
        u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
        memset(fb, 0, (size_t) w * h * 2);
        GSPGPU_FlushDataCache(fb, (u32) w * h * 2);
    }
    return on;
}

// The end after a check's screen (main_3ds.cpp): the log written, the process ended as the game's own end does
extern "C" void re4_3ds_check_exit(void)
{
    re4_log_file_drain();
    re4_log_file_trim();
    gfxExit();
    _exit(0);
}

