// The 3DS buttons as the GameCube controller of channel 0 (stage 6.2): read once per game frame (a frame
// hook of PADRead, runtime/pad.cpp) and handed to the core like the Android app's controller (re4_pad_live);
// a test script (RE4_PAD_*) still wins while it runs.
//
// Layout (decision of the project owner, 02.11.2026: the Android app's default, CoreInput.java): circle pad =
// control stick, C stick (New 3DS) = C stick, A B X Y by their labels, R = R (ready the weapon), L = L (the
// knife), ZR = Z (map), ZL free, D-pad, START. SELECT is the port's: START + SELECT held for a second ends the
// game (START does not reach the game while SELECT is held).
//
// HOME, sleep and the system's close (stage 6.2c), once per game frame here, as libctru's aptMainLoop does them:
// the sleep (aptHandleSleep waits on this thread until the wake-up); HOME: the memory card onto the SD card, the
// bottom screen's thread standing, the GX thread through all it was handed and the renderer's GPU work done
// (re4_gxq_park), a plain black frame of libctru on the top screen (RE4_3DS_HOMEPLAIN), then aptJumpToHomeMenu
// (citro3d's, ndsp's and the screen's hooks; this thread stands until the return); a close (HOME menu "close", the
// power button): the end of the game as with START + SELECT. The game's clock catches up at most 4 fields after the
// pause (runtime/vi.cpp).
//
// Stage 6.2f (a wish of the project owner, 2026-11-09): the gyro turns the aim on top of the stick (runtime/pad.cpp
// re4_gyro_feed, patch 0333). RE4_GYRO: the gain x10 (0 off; 20 = 2x: the console turned by 10 degrees, the aim by 20).
// The words of angularRate in memory order (console run 2026-11-09, r-gyro1.log): 0 about the hinge (plus: the aim
// down), 1 across the bottom screen (plus: to the left with the console flat), 2 along the bottom screen toward the
// hinge (the steering wheel); the accelerometer's x y z the same axes (flat: y -512). RE4_GYRO_SPACE=1 (the default):
// the yaw about the true vertical (the accelerometer's up in words 1 and 2: in the hands the console leans about 45
// degrees, a turn shows on both); 0: RE4_GYRO_AXES=+1,-0 - the words for the yaw and the pitch, with their signs.
// RE4_GYRO_LOG=1: every 30 frames the turn about each word and the aim's share (degrees), the accelerometer, the zero.
#include <3ds.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "re4_core.h"
#include "re4_runtime.h"

extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" void re4_pad_live(const Re4CorePad* pad);
extern "C" void re4_memcard_flush(int force);
extern "C" int re4_dvd_close_cached(void);  // runtime/dvd.cpp (2026-11-21): the disc files kept open, closed
extern "C" void re4_pad_record_flush(void);  // runtime/pad.cpp: RE4_PAD_RECORD
extern "C" void re4_3ds_finish(int code);  // system_3ds.cpp
extern "C" void re4_gxq_park(int on);       // gx_thread_3ds.cpp: the GX thread through all records, the GPU given up
extern "C" void re4_3ds_hud_pause(int on);  // settings_3ds.cpp: the bottom screen's thread stands
extern "C" void re4_log_file_drain(void);   // runtime/log.cpp: the log's lines onto the SD card now
extern "C" unsigned re4_3ds_hud_pad(int phase, int x, int y);  // settings_3ds.cpp (8.2): the buttons by touch
extern "C" void re4_gyro_feed(float yaw, float pitch);  // runtime/pad.cpp (6.2f)
extern "C" unsigned re4_3ds_controls_pad(unsigned keys, int select);  // controls_3ds.cpp (the button mapping)
extern "C" int re4_3ds_controls_turn(void);                  // controls_3ds.cpp: the quick turn's stick back
extern "C" void re4_3ds_controls_sticks(int* px, int* py, int* cx, int* cy, int rim);  // controls_3ds.cpp: moves, looks

namespace {

// GameCube pad bits (PAD_BUTTON_*)
const u16 kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kZ = 0x0010, kR = 0x0020, kL = 0x0040,
          kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kStart = 0x1000;

// (2026-11-21) the keys a button mapping uses, in the order of controls_map.h's ctl::Key (the layout above is its
// default and the menus': controls_3ds.cpp)
const u32 kMapKeys[] = {KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_DUP, KEY_DDOWN, KEY_DLEFT,
                        KEY_DRIGHT, KEY_START};

// A genuine controller at its gate in raw units (CoreInput.java MAIN_RAW, C_RAW): full deflection reaches the
// clamp of PADClamp as on the GameCube. The 3DS's sticks at their rim: about 156 (0x9C, libctru); the largest
// values seen go to the log at the end.
const int kMainRaw = 101, kCRaw = 92, kRim = 156;
const u64 kExitHold = 1000ull * CPU_TICKS_PER_MSEC;

int gMax[4];  // circle pad x, y, C stick x, y: the largest magnitude seen
u64 gExitSince;
// (2026-11-18) The circle pad's sensitivity: its deflection that is a full stick to the game - the rim over the gain
// (percent). The game takes a direction past 45 % of a full stick on an axis (PADClamp's 15 + game/pad.cpp STICK_ON 30
// of 72; 51 % on a diagonal): at 1x nearly the whole way of a pad that no longer reaches its rim
const int kStickGain[4] = {100, 125, 150, 200};
int gStickRim = kRim;
// (2026-11-21, a player's report of an aim that moves by itself: a worn Circle Pad's drift) the Circle Pad's dead zone,
// percent of its rim, from the settings screen (0: none, as before): within it nothing, beyond it the rest stretched
// to the rim (a full stick still at the rim); before the sensitivity
int gDeadPct;

void deadZone(circlePosition& c)
{
    if (gDeadPct <= 0) {
        return;
    }
    const float dz = (float) kRim * (float) gDeadPct / 100.0f;
    const float m = sqrtf((float) c.dx * c.dx + (float) c.dy * c.dy);
    if (m <= dz) {
        c.dx = c.dy = 0;
        return;
    }
    const float f = (m - dz) / ((float) kRim - dz) * (float) kRim / m;
    c.dx = (s16) lrintf((float) c.dx * f);
    c.dy = (s16) lrintf((float) c.dy * f);
}

s8 axis(int v, int raw, int rim, int& seen)
{
    const int m = v < 0 ? -v : v;
    seen = m > seen ? m : seen;
    int r = v * raw / rim;
    r = r > 127 ? 127 : r < -127 ? -127 : r;
    return (s8) r;
}

extern "C" void re4_3ds_texmiss_flush(void);  // gx_3ds.cpp: the texture misses kept in memory

// --- (6.2f) the gyro ---
struct Gyro {
    bool on;
    float gain;     // RE4_GYRO / 10
    float perDps;   // raw units a degree per second (HID's coefficient)
    bool world;     // RE4_GYRO_SPACE: the yaw about the true vertical
    int axis[2];    // RE4_GYRO_AXES: yaw (without world), pitch: the word of angularRate in memory order
    float sign[2];
    bool log;
    bool biasSet;
    float bias[3];  // raw zero, from the still console
    float sum[3], lo[3], hi[3];  // the calibration window: raw sum, least and most
    int still;      // frames in the window
    float up[3];    // the accelerometer, smoothed (raw, about 512 a g; minus is up)
    bool upSet;
    float logTurn[3], logFed[2];
    int logFrames;
    u64 last;       // tick of the last read
} gGyro;

// the zero: a window of a still console (each word within 5 degrees a second, the hand's tremble and the sensor's
// noise are below), not while R is held (a slow turn of the aim is no zero)
const float kGyroStillDps = 5.0f, kGyroSoftDps = 0.6f, kGyroMaxDt = 0.1f, kGyroUpSmooth = 0.15f;
const int kGyroStillFrames = 30;  // about a second of game frames
// (2026-11-21) the zero kept between sessions (gyro.bin) and a soft one when no strict one comes: the stillest second
// seen (a rolling window) after kGyroSoftAfter frames, if its spread is under kGyroSoftDps2
const int kGyroSoftAfter = 150;
const float kGyroSoftDps2 = 15.0f;
const char kGyroZeroPath[] = "sdmc:/3ds/pueblo/gyro.bin";
struct GyroZero {
    float ring[kGyroStillFrames][3];  // the last frames' raw values (not aiming)
    int ringN, ringAt;
    float bestSpread, best[3];        // the stillest window so far: its spread (dps), its mean
    int frames;                       // gyroRead calls since the start
    bool loaded, strict, soft;        // a zero from the file / a strict one this session / a soft one
    volatile bool dirty;              // a strict zero to keep (the bottom screen's thread writes it)
    float saved[3];                   // the zero in the file
    u64 savedAt;
} gZero = {{}, 0, 0, 1e9f, {}, 0, false, false, false, false, {}, 0};

u32 zeroSum(const u8* p, size_t n)
{
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

struct GyroZeroFile {
    char magic[4];  // "PGZ1"
    float bias[3];
    u32 sum;
};

void gyroStart()
{
    const char* g = getenv("RE4_GYRO");
    const int gain = g && *g ? atoi(g) : 20;
    gGyro.gain = (float) gain / 10.0f;
    const char* w = getenv("RE4_GYRO_SPACE");
    gGyro.world = !(w && *w == '0');
    gGyro.axis[0] = 1;
    gGyro.axis[1] = 0;
    gGyro.sign[0] = 1.0f;
    gGyro.sign[1] = -1.0f;
    if (const char* a = getenv("RE4_GYRO_AXES")) {
        for (int k = 0; k < 2 && *a; k++) {
            float s = 1.0f;
            while (*a == ' ' || *a == '+' || *a == '-') {
                s = *a == '-' ? -1.0f : s;
                a++;
            }
            if (*a >= '0' && *a <= '2') {
                gGyro.axis[k] = *a - '0';
                gGyro.sign[k] = s;
                a++;
            }
            while (*a && *a != ',') {
                a++;
            }
            a += *a == ',';
        }
    }
    const char* l = getenv("RE4_GYRO_LOG");
    gGyro.log = l && *l == '1';
    if (gain <= 0 && !gGyro.log) {
        re4_log("3ds: gyro aim off (RE4_GYRO=0)");
        return;
    }
    const Result r = HIDUSER_EnableGyroscope();
    HIDUSER_EnableAccelerometer();
    float coef = 0.0f;
    HIDUSER_GetGyroscopeRawToDpsCoefficient(&coef);
    gGyro.perDps = coef > 0.0f ? coef : 14.375f;
    gGyro.on = R_SUCCEEDED(r) && gain > 0;
    {  // (2026-11-21) the zero of the last session: the aim at once, refined as before when the console is still
        GyroZeroFile f;
        FILE* fp = fopen(kGyroZeroPath, "rb");
        if (fp) {
            const bool ok = fread(&f, 1, sizeof(f), fp) == sizeof(f) && !memcmp(f.magic, "PGZ1", 4) &&
                            f.sum == zeroSum((const u8*) &f, offsetof(GyroZeroFile, sum));
            fclose(fp);
            if (ok) {
                for (int k = 0; k < 3; k++) {
                    gGyro.bias[k] = gZero.saved[k] = f.bias[k];
                }
                gGyro.biasSet = gZero.loaded = true;
                re4_log("3ds: gyro zero from %s: %.1f %.1f %.1f", kGyroZeroPath, f.bias[0], f.bias[1], f.bias[2]);
            }
        }
    }
    re4_log("3ds: gyro aim %s (0x%08lx): gain %.1f, raw/dps %.3f (HID %.3f), yaw %s%c%d, pitch %c%d%s",
            gGyro.on ? "on" : "off", (unsigned long) r, gGyro.gain, gGyro.perDps, coef,
            gGyro.world ? "about the vertical, else " : "", gGyro.sign[0] < 0 ? '-' : '+', gGyro.axis[0],
            gGyro.sign[1] < 0 ? '-' : '+', gGyro.axis[1], gGyro.log ? ", log every 30 frames" : "");
}

// Once a game frame (readPad): the turn since the last read to runtime/pad.cpp
void gyroRead(unsigned frame, bool aiming)
{
    if (!gGyro.on && !gGyro.log) {
        return;
    }
    const u64 now = svcGetSystemTick();
    float dt = gGyro.last ? (float) (now - gGyro.last) / (float) SYSCLOCK_ARM11 : 0.0f;
    gGyro.last = now;
    dt = dt > kGyroMaxDt ? kGyroMaxDt : dt;
    angularRate rate;
    hidGyroRead(&rate);
    const s16* raw = (const s16*) &rate;  // in memory order (libctru's names x, z, y do not fit: see the head)
    accelVector acc;
    hidAccelRead(&acc);
    const float a[3] = {(float) acc.x, (float) acc.y, (float) acc.z};
    for (int k = 0; k < 3; k++) {
        gGyro.up[k] = gGyro.upSet ? gGyro.up[k] + (a[k] - gGyro.up[k]) * kGyroUpSmooth : a[k];
    }
    gGyro.upSet = true;
    // the zero
    gZero.frames++;
    if (!aiming) {  // (2026-11-21) the rolling window for a soft zero: the stillest second seen so far
        for (int k = 0; k < 3; k++) {
            gZero.ring[gZero.ringAt][k] = (float) raw[k];
        }
        gZero.ringAt = (gZero.ringAt + 1) % kGyroStillFrames;
        gZero.ringN += gZero.ringN < kGyroStillFrames;
        if (gZero.ringN == kGyroStillFrames && !gZero.strict) {
            float spread = 0.0f, mean[3];
            for (int k = 0; k < 3; k++) {
                float lo = gZero.ring[0][k], hi = lo, sum = 0.0f;
                for (int i = 0; i < kGyroStillFrames; i++) {
                    const float v = gZero.ring[i][k];
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                    sum += v;
                }
                mean[k] = sum / (float) kGyroStillFrames;
                spread = (hi - lo) / gGyro.perDps > spread ? (hi - lo) / gGyro.perDps : spread;
            }
            if (spread < gZero.bestSpread) {
                gZero.bestSpread = spread;
                memcpy(gZero.best, mean, sizeof(mean));
            }
        }
    }
    if (!gGyro.biasSet && gZero.frames >= kGyroSoftAfter && gZero.bestSpread < kGyroSoftDps2) {
        memcpy(gGyro.bias, gZero.best, sizeof(gZero.best));
        gGyro.biasSet = gZero.soft = true;
        re4_log("3ds: gyro zero (soft, no still second in %d frames; spread %.1f dps): %.1f %.1f %.1f", gZero.frames,
                gZero.bestSpread, gGyro.bias[0], gGyro.bias[1], gGyro.bias[2]);
    }
    if (aiming) {
        gGyro.still = 0;
    } else {
        for (int k = 0; k < 3; k++) {
            const float v = (float) raw[k];
            if (!gGyro.still) {
                gGyro.sum[k] = 0.0f;
                gGyro.lo[k] = gGyro.hi[k] = v;
            }
            gGyro.sum[k] += v;
            gGyro.lo[k] = v < gGyro.lo[k] ? v : gGyro.lo[k];
            gGyro.hi[k] = v > gGyro.hi[k] ? v : gGyro.hi[k];
        }
        bool still = true;
        for (int k = 0; k < 3; k++) {
            still = still && (gGyro.hi[k] - gGyro.lo[k]) / gGyro.perDps < kGyroStillDps;
        }
        if (!still) {
            gGyro.still = 0;
        } else if (++gGyro.still >= kGyroStillFrames) {
            for (int k = 0; k < 3; k++) {
                gGyro.bias[k] = gGyro.sum[k] / (float) gGyro.still;
            }
            if (!gZero.strict) {  // (2026-11-21: the first strict one of the session - logged and kept)
                re4_log("3ds: gyro zero at frame %u: %.1f %.1f %.1f%s", frame, gGyro.bias[0], gGyro.bias[1],
                        gGyro.bias[2], gZero.loaded ? " (the file's refined)" : gZero.soft ? " (the soft one replaced)" : "");
            }
            {
                bool moved = !gZero.strict;
                for (int k = 0; k < 3; k++) {
                    moved = moved || fabsf(gGyro.bias[k] - gZero.saved[k]) > 3.0f;
                }
                if (moved && (!gZero.strict || svcGetSystemTick() - gZero.savedAt > 30ull * SYSCLOCK_ARM11)) {
                    gZero.dirty = true;
                }
            }
            gZero.strict = true;
            gGyro.biasSet = true;
            gGyro.still = 0;
        }
    }
    float dps[3];
    for (int k = 0; k < 3; k++) {
        dps[k] = ((float) raw[k] - gGyro.bias[k]) / gGyro.perDps;
    }
    // yaw (plus: to the left) and pitch (plus: up), degrees a second
    float v[2] = {dps[gGyro.axis[0]] * gGyro.sign[0], dps[gGyro.axis[1]] * gGyro.sign[1]};
    if (gGyro.world) {
        const float uy = -gGyro.up[1], uz = -gGyro.up[2], n = sqrtf(uy * uy + uz * uz);
        if (n > 150.0f) {  // not with the hinge upright (the console on its side): then the plain word
            v[0] = (dps[1] * uy + dps[2] * uz) / n;
        }
    }
    float turn[2];
    for (int k = 0; k < 2; k++) {
        const float m = fabsf(v[k]);
        if (m < kGyroSoftDps) {
            v[k] *= m / kGyroSoftDps;  // the hand's tremble fades, a slow turn still goes
        }
        turn[k] = v[k] * dt * gGyro.gain * (3.14159265f / 180.0f);
    }
    if (gGyro.log) {
        for (int k = 0; k < 3; k++) {
            gGyro.logTurn[k] += dps[k] * dt;
        }
        gGyro.logFed[0] += turn[0] * (180.0f / 3.14159265f);
        gGyro.logFed[1] += turn[1] * (180.0f / 3.14159265f);
        if (++gGyro.logFrames >= 30) {
            re4_log("3ds: gyro frame %u: turn %+.1f %+.1f %+.1f deg, aim yaw %+.1f pitch %+.1f%s, accel %d %d %d, zero "
                    "%.1f %.1f %.1f",
                    frame, gGyro.logTurn[0], gGyro.logTurn[1], gGyro.logTurn[2], gGyro.logFed[0], gGyro.logFed[1],
                    aiming ? " (R)" : "", acc.x, acc.y, acc.z, gGyro.bias[0], gGyro.bias[1], gGyro.bias[2]);
            gGyro.logTurn[0] = gGyro.logTurn[1] = gGyro.logTurn[2] = 0.0f;
            gGyro.logFed[0] = gGyro.logFed[1] = 0.0f;
            gGyro.logFrames = 0;
        }
    }
    if (gGyro.on && dt > 0.0f && gGyro.biasSet) {
        re4_gyro_feed(turn[0], turn[1]);
    }
}

void endGame(unsigned frame, const char* why)
{
    re4_log("3ds: %s: the game ends at frame %u; sticks at most: circle pad %d/%d, C stick %d/%d", why, frame, gMax[0],
            gMax[1], gMax[2], gMax[3]);
    re4_memcard_flush(1);
    re4_pad_record_flush();
    re4_3ds_texmiss_flush();
    fflush(stdout);
    fflush(stderr);
    re4_3ds_finish(0);
    _exit(0);
}

void systemEvents(unsigned frame)
{
    static int plain = -1;
    if (plain < 0) {
        const char* v = getenv("RE4_3DS_HOMEPLAIN");
        plain = !(v && *v == '0');
    }
    aptHandleSleep();
    if (aptShouldJumpToHome()) {
        re4_log("3ds: HOME at frame %u", frame);
        re4_memcard_flush(1);
        // (2026-11-21) the FS's room for open files freed for the HOME menu: its mount of its NAND extdata failed with
        // "out of resource" (0xD860466C) and it panicked (after a Game Reset, after ftpd); opened again at the next read
        re4_log("3ds: HOME: %d disc files closed for the HOME menu", re4_dvd_close_cached());
        re4_3ds_hud_pause(1);
        re4_gxq_park(1);
        if (plain) {  // the top screen as libctru's own frame (black), not the renderer's
            for (int i = 0; i < 2; i++) {
                u16 w = 0, h = 0;
                u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
                memset(fb, 0, (size_t) w * h * 3);
                GSPGPU_FlushDataCache(fb, (u32) w * h * 3);
                gfxScreenSwapBuffers(GFX_TOP, false);
                gspWaitForVBlank();
            }
            gspWaitForVBlank();
        }
        GSPGPU_CaptureInfo cap;  // what the HOME menu's screenshot of the app reads
        if (R_SUCCEEDED(GSPGPU_ImportDisplayCaptureInfo(&cap))) {
            for (int i = 0; i < 2; i++) {
                const GSPGPU_CaptureInfoEntry& e = cap.screencapture[i];
                re4_log("3ds: HOME: screen %d: framebuffers %p %p, format 0x%lx, pitch %lu", i, e.framebuf0_vaddr,
                        e.framebuf1_vaddr, (unsigned long) e.format, (unsigned long) e.framebuf_widthbytesize);
            }
        }
        re4_log("3ds: HOME: to the HOME menu (a plain frame %d: RE4_3DS_HOMEPLAIN)", plain);
        re4_log_file_drain();
        const u64 t0 = svcGetSystemTick();
        aptJumpToHomeMenu();
        if (aptShouldClose()) {  // closed from the HOME menu: the GX thread stays where it stands, no GPU work more
            re4_log("3ds: closed from the HOME menu after %.1f s", (double) (svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
            endGame(frame, "the system closes the game");  // (sms-3ds: a drawing thread woken there hung the close)
        }
        re4_gxq_park(0);
        re4_3ds_hud_pause(0);
        re4_log("3ds: back from the HOME menu after %.1f s", (double) (svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
    }
    if (aptShouldClose()) {
        endGame(frame, "the system closes the game");
    }
}

void readPad(unsigned frame)
{
    systemEvents(frame);
    static long exitAt = -2;  // (R1b) RE4_3DS_EXIT_AT=N: the end by itself at frame N (series of ends)
    if (exitAt == -2) {
        const char* v = getenv("RE4_3DS_EXIT_AT");
        exitAt = v && *v ? atol(v) : -1;
    }
    if (exitAt >= 0 && (long) frame >= exitAt) {
        endGame(frame, "RE4_3DS_EXIT_AT");
    }
    hidScanInput();
    const u32 held = hidKeysHeld();
    // (8.2) the bottom screen's buttons: held for the game while the finger is on them; the panel's switches
    u16 touchButtons = 0;
    if (held & KEY_TOUCH) {
        touchPosition touch;
        hidTouchRead(&touch);
        touchButtons = (u16) re4_3ds_hud_pad((hidKeysDown() & KEY_TOUCH) ? 1 : 0, touch.px, touch.py);
    } else {
        re4_3ds_hud_pad(-1, 0, 0);
    }
    if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
        const u64 now = svcGetSystemTick();
        if (!gExitSince) {
            gExitSince = now;
        } else if (now - gExitSince >= kExitHold) {
            endGame(frame, "START + SELECT");
        }
    } else {
        gExitSince = 0;
    }
    Re4CorePad pad = {};
    unsigned keys = 0;
    for (unsigned i = 0; i < sizeof(kMapKeys) / sizeof(kMapKeys[0]); i++) {
        keys |= (held & kMapKeys[i]) ? 1u << i : 0u;
    }
    {  // (2026-11-21) the Circle Pad's directions past half way (its dead zone off): keys of the mapping too (13..16)
        circlePosition c;
        hidCircleRead(&c);
        deadZone(c);
        keys |= (c.dy > kRim / 2 ? 1u << 13 : 0u) | (c.dy < -kRim / 2 ? 1u << 14 : 0u) |
                (c.dx < -kRim / 2 ? 1u << 15 : 0u) | (c.dx > kRim / 2 ? 1u << 16 : 0u);
        circlePosition cs;  // the C stick's too (17..20)
        hidCstickRead(&cs);
        keys |= (cs.dy > kRim / 2 ? 1u << 17 : 0u) | (cs.dy < -kRim / 2 ? 1u << 18 : 0u) |
                (cs.dx < -kRim / 2 ? 1u << 19 : 0u) | (cs.dx > kRim / 2 ? 1u << 20 : 0u);
    }
    pad.buttons = (u16) re4_3ds_controls_pad(keys, (held & KEY_SELECT) != 0);  // (2026-11-21: the button mapping)
    pad.buttons |= touchButtons;
    if (held & KEY_SELECT) {
        pad.buttons &= (u16) ~kStart;
    }
    circlePosition circle, cstick;
    hidCircleRead(&circle);
    hidCstickRead(&cstick);
    deadZone(circle);  // (2026-11-21) the settings' "Circle Pad dead zone": a worn pad's drift ignored
    {  // (2026-11-21) the moves and looks of the button mapping: the two sticks from the sticks' directions and the keys
        int px = circle.dx, py = circle.dy, cx = cstick.dx, cy = cstick.dy;
        re4_3ds_controls_sticks(&px, &py, &cx, &cy, kRim);
        circle.dx = (s16) px;
        circle.dy = (s16) py;
        cstick.dx = (s16) cx;
        cstick.dy = (s16) cy;
    }
    pad.stickX = axis(circle.dx, kMainRaw, gStickRim, gMax[0]);
    pad.stickY = axis(circle.dy, kMainRaw, gStickRim, gMax[1]);
    pad.substickX = axis(cstick.dx, kCRaw, kRim, gMax[2]);
    pad.substickY = axis(cstick.dy, kCRaw, kRim, gMax[3]);
    if (re4_3ds_controls_turn()) {  // (2026-11-21) the quick turn on its own button: the stick fully back (then B)
        pad.stickX = 0;
        pad.stickY = (s8) -kMainRaw;
    }
    // digital shoulders: the trigger all the way (pad.cpp: the game takes the click or the value)
    pad.triggerL = (pad.buttons & kL) ? 0xFF : 0;
    pad.triggerR = (pad.buttons & kR) ? 0xFF : 0;
    re4_pad_live(&pad);
    gyroRead(frame, (pad.buttons & kR) != 0);  // (the aim: the game's R, wherever the mapping put it)
}

}  // namespace

// The APT events into the log (stage 6.2c). The sleep (aptHandleSleep, the game's thread): the GPU work done and
// the GX thread and the bottom screen's standing until the wake-up, as sms-3ds and MKW-3DS do it (their drawing
// threads drained in the hook at ONSLEEP as at ONSUSPEND). Hooks run newest first: this one before citro3d's.
aptHookCookie gAptCookie;
bool gSleepParked;

void aptEvent(APT_HookType hook, void*)
{
    static const char* const kNames[] = {"suspend", "restore", "sleep", "wake-up", "exit"};
    re4_log("3ds: APT: %s", hook < APTHOOK_COUNT ? kNames[hook] : "?");
    if (hook == APTHOOK_ONSLEEP && !gSleepParked) {
        gSleepParked = true;
        re4_3ds_hud_pause(1);
        re4_gxq_park(1);
    } else if (hook == APTHOOK_ONWAKEUP && gSleepParked) {
        gSleepParked = false;
        re4_gxq_park(0);
        re4_3ds_hud_pause(0);
    }
}

// R2 (setup_3ds.cpp take, the game's thread as the gameplay starts): the gyro aim's gain from the settings screen
// (x10, 0 off); the sensor switched on here when RE4_GYRO had it off at the start
extern "C" void re4_3ds_gyro_set(int gain10)
{
    if (gain10 > 0 && !gGyro.on) {
        const Result r = HIDUSER_EnableGyroscope();
        HIDUSER_EnableAccelerometer();
        if (gGyro.perDps <= 0.0f) {
            float coef = 0.0f;
            HIDUSER_GetGyroscopeRawToDpsCoefficient(&coef);
            gGyro.perDps = coef > 0.0f ? coef : 14.375f;
        }
        gGyro.on = R_SUCCEEDED(r);
        if (!gGyro.on) {
            re4_log("3ds: gyro aim: the sensor did not start (0x%08lx)", (unsigned long) r);
        }
    } else if (gain10 <= 0) {
        gGyro.on = false;
    }
    gGyro.gain = (float) gain10 / 10.0f;
}

// (2026-11-21; setup_3ds.cpp: at once, the game's thread) the Circle Pad's dead zone from the settings screen, percent
extern "C" void re4_3ds_stick_deadzone(int pct)
{
    gDeadPct = pct < 0 ? 0 : pct > 50 ? 50 : pct;
    re4_log("3ds: circle pad: dead zone %d %%", gDeadPct);
}

// (2026-11-21) the bottom screen's thread (setup_3ds.cpp re4_3ds_setup_poll): a strict gyro zero into gyro.bin
extern "C" void re4_3ds_gyro_poll(void)
{
    if (!gZero.dirty) {
        return;
    }
    gZero.dirty = false;
    GyroZeroFile f;
    memcpy(f.magic, "PGZ1", 4);
    for (int k = 0; k < 3; k++) {
        f.bias[k] = gGyro.bias[k];
    }
    f.sum = zeroSum((const u8*) &f, offsetof(GyroZeroFile, sum));
    FILE* fp = fopen(kGyroZeroPath, "r+b");  // (in place: the file keeps its cluster)
    if (!fp) {
        fp = fopen(kGyroZeroPath, "wb");
    }
    const bool ok = fp && fwrite(&f, 1, sizeof(f), fp) == sizeof(f);
    if (fp) {
        fclose(fp);
    }
    if (ok) {
        memcpy(gZero.saved, f.bias, sizeof(f.bias));
        gZero.savedAt = svcGetSystemTick();
    }
    re4_log("3ds: gyro zero %s %s", ok ? "kept in" : "NOT written to", kGyroZeroPath);
}

// (2026-11-18; setup_3ds.cpp take, the game's thread as the gameplay starts) the circle pad's sensitivity from the
// settings screen: 0 1x, 1 1.25x, 2 1.5x, 3 2x
extern "C" void re4_3ds_stick_set(int index)
{
    const int gain = kStickGain[index < 0 || index > 3 ? 0 : index];
    gStickRim = kRim * 100 / gain;
    re4_log("3ds: circle pad: sensitivity %d %% (a full stick at %d of the rim's %d)", gain, gStickRim, kRim);
}

extern "C" void re4_3ds_input_start(void)
{
    if (const char* s = getenv("RE4_3DS_STICK")) {  // (2026-11-18) percent, for the runs without the settings screen
        const int gain = atoi(s) < 100 ? 100 : atoi(s) > 300 ? 300 : atoi(s);
        gStickRim = kRim * 100 / gain;
        re4_log("3ds: circle pad: sensitivity %d %% (RE4_3DS_STICK; a full stick at %d of the rim's %d)", gain,
                gStickRim, kRim);
    }
    gyroStart();
    re4_add_frame_hook(readPad);
    aptHook(&gAptCookie, aptEvent, nullptr);
    re4_log("3ds: live controls (a test script still wins while it runs); START + SELECT for a second ends the game");
}
