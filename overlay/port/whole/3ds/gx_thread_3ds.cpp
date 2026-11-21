// The GX thread of the 3DS (stage 3.5): the GX decoder (runtime/gx_decode.cpp) and the renderer
// (gx_3ds.cpp) on core 2, the game on core 0.
//
// What the game hands the graphics processor (the FIFO's bytes, the GX library's register writes and
// list calls: gx_fifo.cpp) goes into a ring of records; the GX thread works through them in order. The
// game waits for the GX thread where the GameCube's CPU waited for its graphics processor, at the points
// the decoder caught up before (gx_fifo.cpp re4_gx_deliver_interrupts: an OS thread goes to sleep, the
// video retrace), and before anything on the game's thread reads or changes the decoder's state (its
// statistics, a display list recorded into memory): the GX thread then does what the decoder did on the
// game's thread, in the same order, and a run's log stays as it was.
//
// The register shadow (re4_gx_fifo.h) is checked here, on the GX thread (bit 2 of re4_gx_shadow_on keeps
// the game's side off it); the decoder's vertex window is off (the vertices come as records of FIFO
// bytes). RE4_3DS_GXTHREAD=0: all on the game's thread, as before.
//
// The game does not wait for the GX thread at the video retraces, nor when a thread sleeps while
// another can run (gx_fifo.cpp re4_gx_deliver_reached): the GX thread works on through the game's
// logic, as the GameCube's graphics processor did. RE4_3DS_GXBEHIND=N: from game frame N (1 by
// default; 0: never).
#include <3ds.h>

#include <stdlib.h>
#include <string.h>
#include <malloc.h>  // after stdlib.h: newlib's calloc there is noexcept, here not

#include "gx_internal.h"
#include "re4_runtime.h"

// settings_3ds.cpp (6.6): the panel's counters from the start
extern "C" volatile u64 re4_hud_gx_busy, re4_hud_game_wait;
extern "C" volatile u32 re4_hud_vram_kb, re4_hud_linear_kb;

extern "C" int re4_3ds_gx_init(void);         // gx_3ds.cpp
extern "C" void re4_3ds_gx_frame(int log);    // gx_3ds.cpp
extern "C" void re4_skin_finish(void);         // trans.cpp (patch 0325): the skinning jobs left, on this thread
extern "C" void (*re4_skin_finish_fn)(void);   // trans.cpp: called after Render_done instead of waiting for them
extern "C" int (*re4_skin_game_help_fn)(void); // trans.cpp: asked before the game takes a job while it waits
extern "C" int (*re4_skin_yield_fn)(void);     // trans.cpp: asked by this thread between pieces of a job
extern "C" void re4_3ds_gx_idle(void);        // gx_3ds.cpp
extern "C" int re4_skin_help(int who);         // trans.cpp (patch 0324): one skinning job, 1 if there was one
extern "C" void (*re4_skin_wake_fn)(void);     // trans.cpp: called when a job is put
// (stage 41, patch 0336) the heads of the lists that read a model's vertices, by who skinned them (gx_decode.cpp)
extern "C" void (*re4_skin_head_fn)(int side, unsigned gen, const void* src, const void* pos, unsigned posN,
                                    const void* nrm, unsigned nrmN, int nrmKind);
extern "C" unsigned re4_skin_gen;  // trans.cpp: the generation of the jobs being put
extern "C" int re4_gx_head_on;
extern "C" void re4_gx_head_job(int side, unsigned gen, const void* src, const void* pos, unsigned posN,
                                const void* nrm, unsigned nrmN, int nrmKind);
extern "C" void re4_gx_head_announce(unsigned gen);
extern "C" void (*re4_3ds_gx_wake_fn)(void);   // gx_3ds.cpp (the pipeline): called when a GX queue has run out

extern "C" {
// the decoder's register shadow (re4_gx_fifo.h, gx_decode.cpp)
extern int re4_gx_shadow_on;
extern unsigned int re4_gx_bp_shadow[256];
extern unsigned int re4_gx_cp_shadow[256];
extern unsigned char re4_gx_cp_known[256];
extern unsigned int* const re4_gx_xf_mem;
extern unsigned int* const re4_gx_xf_reg;
extern "C" int re4_gx_nrm_dead;  // gx_fifo.cpp (C2)
extern unsigned char* re4_gx_window;
extern unsigned char* re4_gx_window_end;  // (C1: the window into the ring)
extern unsigned int re4_gxq_dropped[3];  // gx_fifo.cpp
extern unsigned int re4_gxq_gen;         // gx_fifo.cpp (re4_gx_fifo.h)
extern unsigned int re4_gxq_dropped[3];  // gx_fifo.cpp

int re4_gxq_on;            // the GX thread runs: the game's GX goes through the ring
static LightEvent gParked, gUnpark;  // re4_gxq_park (HOME, stage 6.2c)
static volatile int gGxQuit;  // (R1b) re4_gxq_quit: 1 the thread ends parked, 2 with C3D_Fini first
extern "C" void re4_3ds_gx_quit(void);  // gx_3ds.cpp (R1b)
void re4_3ds_gx_home(int away);      // gx_3ds.cpp
void* re4_3ds_game_tls;    // the game's thread (profile_3ds.cpp: its zones only)
void* re4_3ds_gx_tls;      // the GX thread (profile_3ds.cpp: the function sampler's second stack)
void* re4_3ds_job_tls;     // the jobs' worker (profile_3ds.cpp: the sampler's third stack)
}

namespace {

enum Kind : u32 { kBytes, kBp, kCp, kXf, kCall, kOwner, kHint, kFn, kWrap, kQuads };

const u32 kRingBytes = 1u << 20;
const u32 kMask = kRingBytes - 1;
const u32 kBytesMax = 4096;         // the FIFO bytes of one record
const u32 kRecordMax = 64u << 10;   // a larger record (an XF block): applied at once, the GX thread idle
const u32 kPublishBytes = 1024;     // the GX thread sees the records each time this many more are written
const int kGameSpin = 1000;         // the game polls this many times before it sleeps on the GX thread
const int kGxSpin = 4000;           // the GX thread polls this many times before it sleeps on the game

u32* gRing;
// positions: bytes written since the start (wrapping at 2^32; in the ring at pos & kMask)
u32 gHead;              // the records published (the game writes, the GX thread reads)
u32 gTail;              // the records worked through (the GX thread writes)
u32 gGxSleeping;        // the GX thread sleeps on gWork
u32 gGameWaiting;       // the game sleeps on gProgress
LightEvent gWork, gProgress, gStarted;
Thread gThread;
void* gGxTls;
int gInitOk;

// the game's side
u32 gWrite;             // the next record
u32 gPublished;
u32 gTailSeen;
bool gOpen;             // a record of FIFO bytes being filled at gOpenAt (room for kBytesMax reserved)
u32 gOpenAt, gOpenBytes;
bool gWindowOn = true;  // RE4_3DS_RINGWINDOW (C1): the game's inline writes into the open record
u32 gWindowOpens, gWindowTakes;  // per log window: windows opened; taken back by PPCSync (re4_gxq_window_take)

u32 gBehindFrom = 1;    // RE4_3DS_GXBEHIND
u32 gGameFrame;         // the game's frames (re4_gxq_frame)

// statistics (each side counts its own; the GX thread logs both)
volatile u64 gGameWaitTicks;
volatile u32 gGameWaits, gGameRoomWaits;
// (stage 41) the skinning jobs' time by where they run: the game in its waits, this thread with an empty ring, this
// thread at the record after the picture (the rest of them: in the picture's way)
volatile u64 gGameHelpTicks;
volatile u32 gGameHelps;
u64 gIdleJobTicks, gFinishTicks;
u32 gIdleJobs;
// (stage 41) the jobs' worker: a thread on core 3 (free of ours; the system leaves an application ~70 % of it) that
// takes the skinning jobs (re4_skin_help(3), patch 0336: a scratch of its own) and with them the heads of the lists.
// RE4_3DS_JOBCORE: the core (3; 0 none).
LightEvent gJobWork;
volatile int gJobSleeping;
Thread gJobThread;
volatile int gJobQuit;  // (R1b) the end: the worker returns at its next wait
volatile u64 gJobTicks;
volatile u32 gJobsDone;
u64 gBusyTicks, gIdleTicks;
u32 gRecords, gRecordBytes, gFrames;
u64 gHudBusyBase, gHudWaitBase;  // the counters before the last log window (the panel's, 6.6)

inline void cpuRelax()
{
    __asm__ __volatile__("" ::: "memory");
}

inline u32* at(u32 pos)
{
    return gRing + ((pos & kMask) >> 2);
}

inline u32 tailNow()
{
    return __atomic_load_n(&gTail, __ATOMIC_SEQ_CST);
}

void closeBytes();

void publish()
{
    closeBytes();
    if (gWrite == gPublished) {
        return;
    }
    gPublished = gWrite;
    __atomic_store_n(&gHead, gWrite, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&gGxSleeping, __ATOMIC_SEQ_CST)) {
        LightEvent_Signal(&gWork);
    }
}

// The game waits until done() (the GX thread signals each step while gGameWaiting is set)
template <class Done>
void gameWait(Done done, bool room)
{
    const u64 start = svcGetSystemTick();
    u32 helped = 0;
    while (!done() && re4_skin_help(1)) {  // the skinning jobs while the game waits (patch 0324)
        helped++;
    }
    if (helped) {  // (stage 41)
        gGameHelpTicks += svcGetSystemTick() - start;
        gGameHelps += helped;
    }
    for (int i = 0; i < kGameSpin && !done(); i++) {
        cpuRelax();
    }
    while (!done()) {
        __atomic_store_n(&gGameWaiting, 1, __ATOMIC_SEQ_CST);
        if (done()) {
            break;
        }
        LightEvent_Wait(&gProgress);
    }
    __atomic_store_n(&gGameWaiting, 0, __ATOMIC_SEQ_CST);
    gGameWaitTicks += svcGetSystemTick() - start;
    gGameWaits++;
    gGameRoomWaits += room;
}

// Room for `bytes` (a multiple of 4) at gWrite, in one piece: a wrap record first when the ring ends
// nearer. No record of FIFO bytes open.
u32* reserve(u32 bytes)
{
    const u32 toEnd = kRingBytes - (gWrite & kMask);
    const u32 need = bytes > toEnd ? toEnd + bytes : bytes;
    if (kRingBytes - (gWrite - gTailSeen) < need) {
        gTailSeen = tailNow();
        if (kRingBytes - (gWrite - gTailSeen) < need) {
            publish();
            gameWait([&] {
                gTailSeen = tailNow();
                return kRingBytes - (gWrite - gTailSeen) >= need;
            }, true);
        }
    }
    if (bytes > toEnd) {
        *at(gWrite) = kWrap;
        gWrite += toEnd;
    }
    return at(gWrite);
}

void written(u32 bytes)
{
    gWrite += bytes;
    if (gWrite - gPublished >= kPublishBytes) {
        publish();
    }
}

// (C1) The game's inline FIFO writes (re4_gx_fifo.h) store into the open record themselves, between
// re4_gx_window and re4_gx_window_end; anything else that touches the ring takes the bytes written back first
inline void windowTake()
{
    if (re4_gx_window) {
        gOpenBytes = (u32) (re4_gx_window - ((u8*) at(gOpenAt) + 4));
        re4_gx_window = re4_gx_window_end = nullptr;
    }
}

void closeBytes()
{
    windowTake();  // (C1)
    if (!gOpen) {
        return;
    }
    gOpen = false;
    *at(gOpenAt) = kBytes | gOpenBytes << 8;
    written(4 + ((gOpenBytes + 3) & ~3u));
}

u32* record(u32 kind, u32 bytes)
{
    closeBytes();
    u32* p = reserve(4 + bytes);
    p[0] = kind | bytes << 8;
    return p;
}

// --- the GX thread -------------------------------------------------------------------------------

inline bool shadowOn()
{
    // (C1: re4_gx_window is the game's window into the ring; the decoder's is never open with this thread)
    return (re4_gx_shadow_on & ~4) == 1;
}

void feedWords(u8 op, const u32* words, int n, int firstBytes)
{
    // the command as bytes (a command of the stream half read: gx_fifo.cpp did the same), big-endian;
    // firstBytes: the first word is a byte (the CP register)
    u8 b[1 + 4 * 5];
    int k = 0;
    b[k++] = op;
    for (int i = 0; i < n; i++) {
        const u32 w = words[i];
        if (i == 0 && firstBytes == 1) {
            b[k++] = (u8) w;
            continue;
        }
        b[k++] = (u8) (w >> 24);
        b[k++] = (u8) (w >> 16);
        b[k++] = (u8) (w >> 8);
        b[k++] = (u8) w;
    }
    re4gx::feed(b, (size_t) k);
}

void applyXf(u32 head, const u32* words)
{
    const u32 address = head & 0xFFFF, count = (head >> 16) + 1;
    const u32* slot = address + count <= 0x800 ? re4_gx_xf_mem + address
                      : address >= 0x1000 && address + count <= 0x1058 ? re4_gx_xf_reg + (address - 0x1000)
                                                                       : nullptr;
    if (slot && memcmp(slot, words, 4 * count) == 0 && shadowOn()) {
        return;
    }
    if (re4gx::directIdle()) {
        re4gx::directXf(head, words);
        return;
    }
    feedWords(0x10, &head, 1, 0);
    for (u32 i = 0; i < count; i++) {
        const u32 w = words[i];
        const u8 b[4] = {(u8) (w >> 24), (u8) (w >> 16), (u8) (w >> 8), (u8) w};
        re4gx::feed(b, 4);
    }
}

void apply(u32 kind, const u32* p, u32 bytes)
{
    switch (kind) {
    case kBytes:
        re4gx::feed((const u8*) p, bytes);
        break;
    case kBp:
        if (re4_gx_bp_shadow[p[0] >> 24] == p[0] && shadowOn()) {
            break;
        }
        if (re4gx::directIdle()) {
            re4gx::directBp(p[0]);
        } else {
            feedWords(0x61, p, 1, 0);
        }
        break;
    case kCp: {
        const u32 reg = p[0] & 0xFF;
        if (re4_gx_cp_known[reg] && re4_gx_cp_shadow[reg] == p[1] && shadowOn()) {
            break;
        }
        if (re4gx::directIdle()) {
            re4gx::directCp((uint8_t) reg, p[1]);
        } else {
            feedWords(0x08, p, 2, 1);
        }
        break;
    }
    case kXf:
        applyXf(p[0], p + 1);
        break;
    case kCall:
        if (re4gx::directIdle()) {
            re4gx::directCall(p[0], p[1]);
        } else {
            feedWords(0x40, p, 2, 0);
        }
        break;
    case kOwner:
        if (re4gx::directIdle()) {
            re4gx::directOwner(p);
        } else {
            feedWords(0x02, p, 4, 0);
        }
        break;
    case kHint:
        if (re4gx::directIdle()) {
            re4gx::directHint(p[0]);
        } else {
            feedWords(0x03, p, 1, 0);
        }
        break;
    case kFn:
        ((void (*)(uint32_t, uint32_t)) p[0])(p[1], p[2]);
        break;
    case kQuads:  // (patch 0339 v2)
        if (re4gx::directIdle()) {
            re4gx::directQuads(p);
        } else {
            const u32 n = 9 + 13 * p[0];
            static u8 b[1 + 4 * (9 + 13 * 128)];
            u32 k = 0;
            b[k++] = 0x04;
            for (u32 i = 0; i < n && k + 4 <= sizeof(b); i++) {
                const u32 w = p[i];
                b[k++] = (u8) (w >> 24), b[k++] = (u8) (w >> 16), b[k++] = (u8) (w >> 8), b[k++] = (u8) w;
            }
            re4gx::feed(b, k);
        }
        break;
    }
}

void progress(u32 tail)
{
    __atomic_store_n(&gTail, tail, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&gGameWaiting, __ATOMIC_SEQ_CST)) {
        LightEvent_Signal(&gProgress);
    }
}

extern "C" void re4_3ds_log_fpscr(const char* who);  // main_3ds.cpp: FPSCR of the calling thread

// a skinning job was put (trans.cpp, patch 0324): the GX thread takes it in its idle time
void skinWake()
{
    if (__atomic_load_n(&gJobSleeping, __ATOMIC_SEQ_CST)) {  // (stage 41) the worker
        LightEvent_Signal(&gJobWork);
    }
    if (__atomic_load_n(&gGxSleeping, __ATOMIC_SEQ_CST)) {
        LightEvent_Signal(&gWork);
    }
}

// the skinning jobs left after Render_done (trans.cpp, patch 0325): this thread runs them at a record before the
// next picture's, which read their vertices
void skinFinishRecord(uint32_t gen, uint32_t)
{
    const u64 from = svcGetSystemTick();
    re4_skin_finish();
    gFinishTicks += svcGetSystemTick() - from;
    re4_gx_head_announce(gen);  // (stage 41) the draws from here read that generation's arrays
}

void skinFinish()
{
    re4_gxq_fn(skinFinishRecord, re4_skin_gen, 0);
    re4_gxq_publish();
}

// may the game take a skinning job while it waits for this thread (patch 0325)? RE4_SKIN_GAMEHELP: 0 never, 1 always
// (as 0324), 2 (default) only while this thread has more than RE4_SKIN_HELPKB (16) KB of records left: else it is
// in a job or nearly done, and a job (a whole model, up to ~0.8 ms) would outlast the wait
int skinGameHelp()
{
    static int mode = -1;
    static u32 bytes;
    if (mode < 0) {
        const char* e = getenv("RE4_SKIN_GAMEHELP");
        const char* k = getenv("RE4_SKIN_HELPKB");
        mode = e ? atoi(e) : 2;
        bytes = (u32) (k ? atoi(k) : 16) << 10;
        re4_log("3ds: the game takes skinning jobs while it waits: %s (RE4_SKIN_GAMEHELP, RE4_SKIN_HELPKB %u)",
                mode == 0 ? "never" : mode == 1 ? "always" : "while the GX thread is behind", bytes >> 10);
    }
    if (mode != 2) {
        return mode;
    }
    return gWrite - tailNow() > bytes;
}

// a GX queue has run out with another one waiting (gx_3ds.cpp, the pipeline; libctru's event thread): this thread, if
// it sleeps on the game or is about to, comes round to its idle call (re4_3ds_gx_idle makes the next queue run)
void pipeWake()
{
    LightEvent_Signal(&gWork);
}

// records for this thread while it skins (patch 0325): it gives way to them between pieces of a job
int skinYield()
{
    return __atomic_load_n(&gHead, __ATOMIC_SEQ_CST) != gTail;
}

// (stage 41) the jobs' worker (core 3): a job after another, asleep while there is none (skinWake)
void jobMain(void*)
{
    re4_3ds_job_tls = getThreadLocalStorage();
    re4_3ds_log_fpscr("jobs' worker thread");
    for (;;) {
        const u64 from = svcGetSystemTick();
        if (re4_skin_help(3)) {
            gJobTicks += svcGetSystemTick() - from;
            gJobsDone++;
            continue;
        }
        __atomic_store_n(&gJobSleeping, 1, __ATOMIC_SEQ_CST);
        if (re4_skin_help(3)) {  // (put since)
            __atomic_store_n(&gJobSleeping, 0, __ATOMIC_SEQ_CST);
            gJobTicks += svcGetSystemTick() - from;
            gJobsDone++;
            continue;
        }
        LightEvent_Wait(&gJobWork);
        __atomic_store_n(&gJobSleeping, 0, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&gJobQuit, __ATOMIC_SEQ_CST)) {  // (R1b)
            return;
        }
    }
}

void gxMain(void*)
{
    gGxTls = getThreadLocalStorage();
    re4_3ds_gx_tls = gGxTls;
    re4_3ds_log_fpscr("GX thread");
    gInitOk = re4_3ds_gx_init();
    LightEvent_Signal(&gStarted);
    if (!gInitOk) {
        return;
    }
    re4_skin_finish_fn = skinFinish;  // patch 0325
    re4_skin_game_help_fn = skinGameHelp;
    re4_skin_yield_fn = skinYield;
    re4_skin_wake_fn = skinWake;
    re4_3ds_gx_wake_fn = pipeWake;
    u32 tail = 0;
    for (;;) {
        u32 head = __atomic_load_n(&gHead, __ATOMIC_SEQ_CST);
        if (head == tail) {  // nothing in the ring: a skinning job (patch 0324), counted as busy
            const u64 skinStart = svcGetSystemTick();
            if (re4_skin_help(0)) {
                gBusyTicks += svcGetSystemTick() - skinStart;
                gIdleJobTicks += svcGetSystemTick() - skinStart;  // (stage 41)
                gIdleJobs++;
                continue;
            }
        }
        if (head == tail) {
            const u64 start = svcGetSystemTick();
            for (int i = 0; i < kGxSpin && head == tail; i++) {
                cpuRelax();
                head = __atomic_load_n(&gHead, __ATOMIC_SEQ_CST);
            }
            if (head == tail) {
                re4_3ds_gx_idle();  // the GPU given what it has before the sleep (stage 3.8)
                head = __atomic_load_n(&gHead, __ATOMIC_SEQ_CST);
            }
            if (head == tail) {
                __atomic_store_n(&gGxSleeping, 1, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&gHead, __ATOMIC_SEQ_CST) == tail) {
                    LightEvent_Wait(&gWork);
                }
                __atomic_store_n(&gGxSleeping, 0, __ATOMIC_SEQ_CST);
            }
            gIdleTicks += svcGetSystemTick() - start;
            continue;
        }
        const u64 start = svcGetSystemTick();
        u32 n = 0;
        while (tail != head) {
            const u32* p = at(tail);
            const u32 kind = p[0] & 0xFF, bytes = p[0] >> 8;
            if (kind == kWrap) {
                tail += kRingBytes - (tail & kMask);
                continue;
            }
            apply(kind, p + 1, bytes);
            const u32 size = 4 + ((bytes + 3) & ~3u);
            tail += size;
            gRecords++;
            gRecordBytes += size;
            if ((++n & 31) == 0) {
                progress(tail);
            }
        }
        progress(tail);
        gBusyTicks += svcGetSystemTick() - start;
    }
}

bool onGxThread()
{
    return gGxTls && getThreadLocalStorage() == gGxTls;
}

// One game frame (a record): the renderer's lines and this thread's
void frameFn(uint32_t log, uint32_t)
{
    gFrames++;
    re4_hud_gx_busy = gHudBusyBase + gBusyTicks;
    re4_hud_game_wait = gHudWaitBase + gGameWaitTicks;
    if ((gFrames & 31) == 1) {
        re4_hud_vram_kb = vramSpaceFree() >> 10;
        re4_hud_linear_kb = linearSpaceFree() >> 10;
    }
    if (log && gFrames) {
        const double ms = (double) CPU_TICKS_PER_MSEC * gFrames;
        re4_log("3ds: GX thread per picture: busy %.2f ms, idle %.2f ms, %.0f records (%.1f KB); the game waited "
                "%.2f ms (%.1f waits, %.2f for room in the ring)",
                gBusyTicks / ms, gIdleTicks / ms, (double) gRecords / gFrames, gRecordBytes / 1024.0 / gFrames,
                gGameWaitTicks / ms, (double) gGameWaits / gFrames, (double) gGameRoomWaits / gFrames);
        re4_log("3ds: the ring's register shadow per picture: CP %.0f, XF %.0f writes left out (BP: before the call)",
                (double) re4_gxq_dropped[1] / gFrames, (double) re4_gxq_dropped[2] / gFrames);
        re4_gxq_dropped[1] = re4_gxq_dropped[2] = 0;
        re4_log("3ds: the ring's window (RE4_3DS_RINGWINDOW %d) per picture: %.1f opened, %.1f taken back by PPCSync",
                (int) gWindowOn, (double) gWindowOpens / gFrames, (double) gWindowTakes / gFrames);
        gWindowOpens = gWindowTakes = 0;
        re4_log("3ds: skinning jobs' time per picture (stage 41): the worker %.2f ms (%.1f jobs), the game in its waits "
                "%.2f ms (%.1f jobs), the GX thread with an empty ring %.2f ms (%.1f calls), at the record after the "
                "picture %.2f ms",
                gJobTicks / ms, (double) gJobsDone / gFrames, gGameHelpTicks / ms, (double) gGameHelps / gFrames,
                gIdleJobTicks / ms, (double) gIdleJobs / gFrames, gFinishTicks / ms);
        gJobTicks = 0;
        gJobsDone = 0;
        gGameHelpTicks = 0;
        gGameHelps = 0;
        gIdleJobTicks = gFinishTicks = 0;
        gIdleJobs = 0;
        gHudBusyBase += gBusyTicks;
        gHudWaitBase += gGameWaitTicks;
        gBusyTicks = gIdleTicks = 0;
        gRecords = gRecordBytes = gFrames = 0;
        gGameWaitTicks = 0;
        gGameWaits = gGameRoomWaits = 0;
    }
    re4_3ds_gx_frame((int) log);
}

}  // namespace

extern "C" {

// Starts the renderer (main_3ds.cpp): on the GX thread on core 2, or here (RE4_3DS_GXTHREAD=0, or no
// thread on core 2). 0: the renderer did not start.
int re4_gxq_start(void)
{
    re4_3ds_game_tls = getThreadLocalStorage();
    const char* v = getenv("RE4_3DS_GXTHREAD");
    if (v && *v == '0') {
        re4_log("3ds: GX thread off (RE4_3DS_GXTHREAD=0): the decoder and the renderer on the game's thread");
        return re4_3ds_gx_init();
    }
    gRing = (u32*) memalign(64, kRingBytes);
    if (!gRing) {
        re4_log("3ds: GX thread: no memory for the ring, all on the game's thread");
        return re4_3ds_gx_init();
    }
    LightEvent_Init(&gWork, RESET_ONESHOT);
    LightEvent_Init(&gProgress, RESET_ONESHOT);
    LightEvent_Init(&gStarted, RESET_STICKY);
    LightEvent_Init(&gParked, RESET_ONESHOT);
    LightEvent_Init(&gUnpark, RESET_ONESHOT);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    gThread = threadCreate(gxMain, nullptr, 256u << 10, priority - 1, 2, true);
    if (gThread) {  // (stage 41) the jobs' worker
        const char* jc = getenv("RE4_3DS_JOBCORE");
        const int core = jc ? atoi(jc) : 3;
        LightEvent_Init(&gJobWork, RESET_ONESHOT);
        if (core > 0) {
            gJobThread = threadCreate(jobMain, nullptr, 64u << 10, priority - 1, core, true);
        }
        re4_log("3ds: the skinning jobs' worker (RE4_3DS_JOBCORE %d): %s", core,
                gJobThread ? "on" : core > 0 ? "no thread there" : "off");
    }
    if (!gThread) {
        re4_log("3ds: GX thread: no thread on core 2, all on the game's thread");
        free(gRing);
        gRing = nullptr;
        return re4_3ds_gx_init();
    }
    LightEvent_Wait(&gStarted);
    if (!gInitOk) {
        return 0;
    }
    if (const char* behind = getenv("RE4_3DS_GXBEHIND")) {
        gBehindFrom = (u32) atoi(behind);
    }
    re4_gx_shadow_on |= 4;
    re4_gxq_on = 1;
    const char* ringShadow = getenv("RE4_3DS_RINGSHADOW");
    if ((re4_gx_shadow_on & 1) && !(ringShadow && *ringShadow == '0')) {
        re4_gxq_gen = 1;  // the register shadow on the game's side (re4_gx_fifo.h, stage 3.6)
    }
    if (const char* rw = getenv("RE4_3DS_RINGWINDOW")) {  // (C1)
        gWindowOn = *rw != '0';
    }
    re4_log("3ds: the game's FIFO writes straight into the ring's open record: %s (RE4_3DS_RINGWINDOW)",
            gWindowOn ? "on" : "off");
    re4_gx_nrm_dead = 1;  // (C2)
    if (const char* nd = getenv("RE4_3DS_NRMDEAD")) {
        re4_gx_nrm_dead = *nd != '0';
    }
    re4_log("3ds: normal matrices left out (no one reads them here; patch 0335): %s (RE4_3DS_NRMDEAD)",
            re4_gx_nrm_dead ? "on" : "off");
    re4_gx_head_on = 1;  // (stage 41)
    if (const char* hj = getenv("RE4_3DS_HEADJOB")) {
        re4_gx_head_on = atoi(hj);
    }
    re4_skin_head_fn = re4_gx_head_on > 0 ? re4_gx_head_job : nullptr;
    re4_log("3ds: heads of the animated lists by the skinning jobs (patch 0336): %s (RE4_3DS_HEADJOB)",
            re4_gx_head_on == 2 ? "on, each checked" : re4_gx_head_on > 0 ? "on" : "off");
    re4_log("3ds: the ring's register shadow on the game's side: %s", re4_gxq_gen != ~0u ? "on" : "off");
    re4_log("3ds: GX thread on core 2 (priority 0x%lx): the decoder and the renderer; ring %u KB", (long) priority - 1,
            kRingBytes >> 10);
    return 1;
}

void re4_gxq_bytes(const uint8_t* bytes, size_t n)
{
    windowTake();  // (C1)
    while (n) {
        if (!gOpen || gOpenBytes == kBytesMax) {
            closeBytes();
            reserve(4 + kBytesMax);
            gOpen = true;
            gOpenAt = gWrite;
            gOpenBytes = 0;
        }
        const u32 take = n < kBytesMax - gOpenBytes ? (u32) n : kBytesMax - gOpenBytes;
        memcpy((u8*) at(gOpenAt) + 4 + gOpenBytes, bytes, take);
        gOpenBytes += take;
        bytes += take;
        n -= take;
    }
    if (gWindowOn && gOpen && gOpenBytes < kBytesMax) {  // (C1) the next writes into the record's room themselves
        u8* room = (u8*) at(gOpenAt) + 4;
        re4_gx_window = room + gOpenBytes;
        re4_gx_window_end = room + kBytesMax;
        gWindowOpens++;
    }
}

// (C1) The window's last byte written (gx_fifo.cpp re4_gx_window_full): the record closed, the next write
// opens another
void re4_gxq_window_full(void)
{
    windowTake();
    closeBytes();
}

// (C1) PPCSync (dolphin_os.cpp): the SDK's FIFO switches (GXSetCPUFifo, GXRedirectWriteGatherPipe) call it
// after the FIFO is unlinked from the GP and before the next write: the window taken back, the writes after it by
// put() (to memory while unlinked); the record stays open
void re4_gxq_window_take(void)
{
    if (re4_gxq_on && re4_gx_window && getThreadLocalStorage() == re4_3ds_game_tls) {
        windowTake();
        gWindowTakes++;
    }
}

void re4_gxq_bp(uint32_t value)
{
    u32* p = record(kBp, 4);
    p[1] = value;
    written(8);
    const u32 reg = value >> 24;
    if (reg == 0x45 || reg == 0x47 || reg == 0x48) {
        publish();  // draw done, a token: a point the game waits for, the GX thread to reach it at once
    }
}

void re4_gxq_cp(uint8_t reg, uint32_t value)
{
    u32* p = record(kCp, 8);
    p[1] = reg;
    p[2] = value;
    written(12);
}

void re4_gxq_xf(uint32_t head, const uint32_t* words)
{
    const u32 count = (head >> 16) + 1;
    const u32 bytes = 4 + 4 * count;
    if (bytes > kRecordMax) {
        re4_gxq_sync();
        applyXf(head, words);
        return;
    }
    u32* p = record(kXf, bytes);
    p[1] = head;
    memcpy(p + 2, words, 4 * count);
    written(4 + bytes);
}

void re4_gxq_call(uint32_t address, uint32_t size)
{
    u32* p = record(kCall, 8);
    p[1] = address;
    p[2] = size;
    written(12);
    publish();  // a list call is work for the GX thread: at once
}

void re4_gxq_owner(const uint32_t owner[4])
{
    u32* p = record(kOwner, 16);
    memcpy(p + 1, owner, 16);
    written(20);
}

void re4_gxq_hint(uint32_t hint)
{
    u32* p = record(kHint, 4);
    p[1] = hint;
    written(8);
}

// (patch 0339 v2) A batch of sprite quads (re4_gx_put_quads): work for the GX thread at once
void re4_gxq_quads(const uint32_t* words)
{
    const u32 bytes = 4 * (9 + 13 * words[0]);
    u32* p = record(kQuads, bytes);
    memcpy(p + 1, words, bytes);
    written(4 + bytes);
    publish();
}

void re4_gxq_fn(void (*fn)(uint32_t, uint32_t), uint32_t a, uint32_t b)
{
    u32* p = record(kFn, 12);
    p[1] = (u32) (uintptr_t) fn;
    p[2] = a;
    p[3] = b;
    written(16);
    publish();
}

// The game waits until the GX thread has worked through all records: the decoder's state is then the
// game's to read (the GX thread idle until the next record). Nothing on the GX thread itself.
void re4_gxq_sync(void)
{
    if (!re4_gxq_on || onGxThread()) {
        return;
    }
    publish();
    if (tailNow() == gWrite) {
        return;
    }
    gameWait([] { return tailNow() == gWrite; }, false);
}

// HOME (input_3ds.cpp, stage 6.2c): the GX thread works through all records, the renderer gives the GPU up
// (re4_3ds_gx_home(1): all with the GPU done, a neutral state), and the thread stands until re4_gxq_park(0): while the
// HOME menu has the GPU, nothing of the app may go to it (the idle's kicks, the DMAs); back, the renderer takes it
// again (re4_3ds_gx_home(0)) before the game goes on
void parkFn(uint32_t, uint32_t)
{
    re4_3ds_gx_home(1);
    LightEvent_Signal(&gParked);
    LightEvent_Wait(&gUnpark);
    if (const int quit = gGxQuit) {  // (R1b) the end: the thread stops here, the GPU given up
        if (quit == 2) {
            re4_3ds_gx_quit();
        }
        LightEvent_Signal(&gParked);
        threadExit(0);
    }
    re4_3ds_gx_home(0);
    LightEvent_Signal(&gParked);
}

void re4_gxq_park(int on)
{
    if (onGxThread()) {
        return;
    }
    static int depth;  // (R1) nested parks: the end inside HOME's, the sleep inside HOME; the outer one acts
    if (on ? depth++ > 0 : (depth > 0 && --depth > 0)) {
        return;
    }
    if (!re4_gxq_on) {  // (the renderer on the game's thread)
        re4_3ds_gx_home(on);
        return;
    }
    if (on) {
        re4_gxq_fn(parkFn, 0, 0);
    } else {
        LightEvent_Signal(&gUnpark);
    }
    LightEvent_Wait(&gParked);
}

// (R1b) The end of the app (system_3ds.cpp re4_3ds_finish, RE4_3DS_EXITMODE 2/3): inside re4_gxq_park(1). The jobs'
// worker and the GX thread end and are joined (a thread of the app alive while libctru frees the memory at _exit).
// fini: C3D_Fini on the GX thread before it ends. The log says how long each took.
void re4_gxq_quit(int fini)
{
    const u64 t0 = svcGetSystemTick();
    int job = -1, gx = -1;
    if (gJobThread) {
        __atomic_store_n(&gJobQuit, 1, __ATOMIC_SEQ_CST);
        LightEvent_Signal(&gJobWork);
        job = R_SUCCEEDED(threadJoin(gJobThread, 1000000000ull)) ? 1 : 0;
        if (job) {
            threadFree(gJobThread);
            gJobThread = nullptr;
        }
    }
    if (re4_gxq_on && gThread && !onGxThread()) {
        gGxQuit = fini ? 2 : 1;
        LightEvent_Signal(&gUnpark);
        LightEvent_Wait(&gParked);
        gx = R_SUCCEEDED(threadJoin(gThread, 1000000000ull)) ? 1 : 0;
        if (gx) {
            threadFree(gThread);
            gThread = nullptr;
            re4_gxq_on = 0;
        }
    }
    re4_log("3ds: the end: the jobs' worker joined %d, the GX thread joined %d (C3D_Fini %d), %.1f ms", job, gx, fini,
            (double) (svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
}

int re4_gxq_behind(void)
{
    return gBehindFrom && gGameFrame >= gBehindFrom;
}

void re4_gxq_publish(void)
{
    if (re4_gxq_on && !onGxThread()) {
        publish();
    }
}

// A game frame done (system_3ds.cpp): the renderer's statistics, on the thread that keeps them
void re4_gxq_frame(int log)
{
    if (++gGameFrame == gBehindFrom && re4_gxq_on) {
        re4_log("3ds: the GX thread works on behind the game from frame %lu (RE4_3DS_GXBEHIND)",
                (unsigned long) gGameFrame);
    }
    if (re4_gxq_on) {
        re4_gxq_fn(frameFn, (uint32_t) log, 0);
    } else {
        re4_3ds_gx_frame(log);
    }
}

}  // extern "C"
