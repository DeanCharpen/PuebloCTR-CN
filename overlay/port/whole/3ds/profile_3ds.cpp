// Where a frame's time goes on the 3DS (stage 3.2), two views of the same frames, logged with the
// frame times of system_3ds.cpp (every 300 frames):
//
// - the runtime's parts (RE4_PROF_ZONE in the shared files): the AI's audio frame and the DSP's
//   mixing inside it, the disc reads, the Yz2 decoder, the end of a GX chunk. Exclusive of each
//   other (a part inside another is taken out of the outer one).
// - the parts called tens of thousands of times a frame (RE4_PROF_SAMPLE): a FIFO write (a put per
//   value the game writes, the decoder's work included) and the decoder's drain. One call in
//   `every` is timed and the sum scaled by `every`: timing each call cost more than the call (the
//   first run, 01.11.2026: 56 000 drains a frame, 81 ms frames became 133 ms).
//   The game's own code is the frame minus the zones minus the FIFO writes.
// - the game's own marks (main.cpp ProcessTickGet, patch 0108: wall time from the frame's start,
//   the runtime's parts included): the time up to each mark, so per section of the game loop.
//
// The clock is svcGetSystemTick (268 MHz); a read costs a supervisor call, measured once at the
// start and reported with the number of reads per frame.
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re4_runtime.h"

extern u32 proc_tick[32];  // game/debug.cpp (patch 0108): the marks of the last frame, OSGetTick
extern const char* proc_name[32];
extern int proc_tick_idx_bak;

namespace {

const char* const kZoneNames[RE4_PROF_ZONES] = {"game", "GX decoder", "GX frame end", "audio frame", "DSP mixing",
                                                  "disc reads", "Yz2 decoder", "FIFO writes", "[FIFO: BP",
                                                  "CP", "XF", "list calls", "owner tags", "stream bytes",
                                                  "window draws", "renderer hooks]"};

u64 gLast;  // the clock when the time was last charged; 0 until the first frame
int gStack[16];
int gDepth;
u64 gTicks[RE4_PROF_ZONES];
u32 gCalls[RE4_PROF_ZONES];
u32 gFrames;
u64 gFrameTicks, gFrameAt;
double gReadNs;

// The sampled parts: calls, the timed calls' clock and their scaled sum
struct Sampled {
    u32 calls;
    u64 start;
    double ticks;
};
Sampled gSampled[RE4_PROF_ZONES];
u32 gSampledReads;  // one svcGetSystemTick

// The game's marks: the time up to each mark, by name (the names are the game's literals)
const int kMarks = 40;
struct Mark {
    const char* name;
    double ticks;  // OSGetTick: the bus clock / 4
    u32 frames;
};
Mark gMarks[kMarks];
int gMarkCount;
u32 gMarkFrames, gMarkSkipped;

void charge(u64 now)
{
    if (!gLast) {
        return;
    }
    gTicks[gDepth ? gStack[gDepth - 1] : 0] += now - gLast;
    gLast = now;
}

void calibrate()
{
    const int n = 20000;
    u64 from = svcGetSystemTick(), t = from;
    for (int i = 0; i < n; i++) {
        t = svcGetSystemTick();
    }
    gReadNs = (double) (t - from) / n * 1e6 / CPU_TICKS_PER_MSEC;
}

Mark* mark(const char* name)
{
    for (int i = 0; i < gMarkCount; i++) {
        if (gMarks[i].name == name || strcmp(gMarks[i].name, name) == 0) {
            return &gMarks[i];
        }
    }
    if (gMarkCount == kMarks) {
        return nullptr;
    }
    gMarks[gMarkCount] = Mark{name, 0, 0};
    return &gMarks[gMarkCount++];
}

// The marks of the frame before (the frame hook runs from PadRead, after ProcessTickInit: the
// values of the frame before are all still there), in the order of their times.
void takeMarks()
{
    struct At {
        u32 tick;
        const char* name;
    } at[32];
    int n = 0;
    const int level5 = proc_tick_idx_bak < 27 ? proc_tick_idx_bak : 27;
    for (int i = 0; i < 5 + level5; i++) {
        if (proc_name[i] && proc_tick[i] <= proc_tick[0]) {
            at[n++] = At{proc_tick[i], proc_name[i]};
        }
    }
    if (!proc_name[0] || proc_tick[0] > 162000000u / 4) {  // no total, or above a second
        gMarkSkipped++;
        return;
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && at[j].tick < at[j - 1].tick; j--) {
            At s = at[j];
            at[j] = at[j - 1];
            at[j - 1] = s;
        }
    }
    u32 from = 0;
    for (int i = 0; i < n; i++) {
        if (Mark* m = mark(at[i].name)) {
            m->ticks += at[i].tick - from;
            m->frames++;
        }
        from = at[i].tick;
    }
    gMarkFrames++;
}

void report()
{
    if (!gFrames) {
        return;
    }
    const double f = gFrames;
    const double toMs = 1.0 / CPU_TICKS_PER_MSEC / f;
    char line[1000];
    // the game: the frame minus the zones minus the FIFO writes (the decoder is inside those)
    double game = (double) gFrameTicks - gSampled[RE4_PROF_FIFO].ticks;
    for (int z = 1; z < RE4_PROF_ZONES; z++) {
        game -= (double) gTicks[z];
    }
    int len = snprintf(line, sizeof(line), "parts (ms per frame, %lu frames, frame %.2f): game %.2f",
                       (unsigned long) gFrames, gFrameTicks * toMs, game * toMs);
    u64 reads = gSampledReads;
    for (int z = 1; z < RE4_PROF_ZONES && len < (int) sizeof(line); z++) {
        reads += 2ull * gCalls[z];
        const bool sampled = gSampled[z].calls != 0;
        const double zoneMs = (sampled ? gSampled[z].ticks : (double) gTicks[z]) * toMs;
        const u32 calls = sampled ? gSampled[z].calls : gCalls[z];
        len += snprintf(line + len, sizeof(line) - len, ", %s%s %.2f (%.1f times)", kZoneNames[z],
                        z == RE4_PROF_FIFO ? " (decoder inside)" : "", zoneMs, calls / f);
    }
    re4_log("3ds: %s", line);
    re4_log("3ds: parts: clock reads %.0f per frame, %.3f ms of the frame (%.0f ns each)", reads / f,
            reads / f * gReadNs / 1e6, gReadNs);
    len = snprintf(line, sizeof(line), "game marks (ms per frame up to the mark, %lu frames, %lu without marks):",
                   (unsigned long) gMarkFrames, (unsigned long) gMarkSkipped);
    const double ms = 1000.0 / (162000000.0 / 4);
    for (int i = 0; i < gMarkCount && len < (int) sizeof(line); i++) {
        len += snprintf(line + len, sizeof(line) - len, " %s %.2f%s", gMarks[i].name,
                        gMarkFrames ? gMarks[i].ticks * ms / gMarkFrames : 0.0, i + 1 < gMarkCount ? "," : "");
    }
    re4_log("3ds: %s", line);
    memset(gTicks, 0, sizeof(gTicks));
    memset(gCalls, 0, sizeof(gCalls));
    for (Sampled& z : gSampled) {
        z.calls = 0;
        z.ticks = 0;
    }
    gSampledReads = 0;
    gFrames = 0;
    gFrameTicks = 0;
    for (int i = 0; i < gMarkCount; i++) {
        gMarks[i].ticks = 0;
        gMarks[i].frames = 0;
    }
    gMarkFrames = gMarkSkipped = 0;
}

}  // namespace

extern "C" void* re4_3ds_game_tls;  // gx_thread_3ds.cpp: the game's thread
extern "C" int re4_gxq_on;

namespace {
// The zones are the game thread's (the GX thread's work is timed there, gx_thread_3ds.cpp)
inline bool otherThread()
{
    return re4_gxq_on && getThreadLocalStorage() != re4_3ds_game_tls;
}
}  // namespace

extern "C" void re4_prof_enter(int zone)
{
    if (otherThread()) {
        return;
    }
    charge(svcGetSystemTick());
    if (gDepth < (int) (sizeof(gStack) / sizeof(gStack[0]))) {
        gStack[gDepth] = zone;
    }
    gDepth++;
    gCalls[zone]++;
}

extern "C" void re4_prof_leave(void)
{
    if (otherThread()) {
        return;
    }
    charge(svcGetSystemTick());
    if (gDepth > 0) {
        gDepth--;
    }
}

// The sampled parts (RE4_PROF_SAMPLE): 1 when this call is timed. Not on the zones' stack: the
// zone around it (the game, mostly) is charged its time, the report takes it out again.
extern "C" int re4_prof_sample_begin(int zone, unsigned every)
{
    if (otherThread()) {
        return 0;
    }
    Sampled& z = gSampled[zone];
    if (z.calls++ & (every - 1)) {  // every: a power of two (ARMv6K divides in a library call)
        return 0;
    }
    z.start = svcGetSystemTick();
    return 1;
}

extern "C" void re4_prof_sample_end(int zone, unsigned every)
{
    Sampled& z = gSampled[zone];
    // less the one clock read inside the timed span (the second run, 01.11.2026: 247 ns times
    // 56 000 drains made 14 ms of the decoder's 50)
    const double read = gReadNs * CPU_TICKS_PER_MSEC / 1e6;
    const double span = (double) (svcGetSystemTick() - z.start) - read;
    z.ticks += (span > 0 ? span : 0) * every;
    gSampledReads += 2;
}

// --- the function sampler (stage 3.6) ------------------------------------------------------------
// A build with RE4_3DS_INSTRUMENT=1 (build3ds.py) has the game, the SDK and the modules call the
// hooks below at each function's entry and exit (-finstrument-functions-after-inlining): a shadow
// stack of the function addresses. A thread on another core reads it every ~50 us: the function on
// top (its own time) and each function in the stack once (inclusive). The game's OS threads are
// contexts on the one thread: a switch leaves the stack off, an exit looks a few entries down.
// The GX thread (stage 3.8: core 2, re4_3ds_gx_tls) has a stack of its own, sampled the same way;
// so has the jobs' worker (stage 41: core 3, re4_3ds_job_tls; it skins and lays heads out).
extern "C" void* re4_3ds_gx_tls;
extern "C" void* re4_3ds_job_tls;
namespace {

const int kShadow = 96;
struct Shadow {
    volatile u32 at[kShadow];
    volatile int depth;
};
Shadow gGame, gGx, gJob, gOther;  // gOther: any other thread's, not sampled
bool gShadowUsed;  // a hook ran: an instrumented build

__attribute__((no_instrument_function)) inline Shadow& shadowHere()
{
    void* const tls = getThreadLocalStorage();
    if (re4_3ds_gx_tls && tls == re4_3ds_gx_tls) {
        return gGx;
    }
    if (re4_3ds_job_tls && tls == re4_3ds_job_tls) {
        return gJob;
    }
    // the game's OS threads are contexts on the one thread; before it is known (start-up), the game's too.
    // Not the GPU busy sampler (gx_3ds.cpp, core 1) and the like: they left entries in the game's stack
    return !re4_3ds_game_tls || tls == re4_3ds_game_tls ? gGame : gOther;
}

struct Bin {
    u32 fn, self, incl;
};
const u32 kBins = 8192;
Bin gBins[kBins], gGxBins[kBins], gJobBins[kBins];
volatile bool gSampling, gSamplerRun;
u32 gSamples, gSamplesTop, gSamplesDeep;
u32 gGxSamples, gGxSamplesTop;
u32 gJobSamples, gJobSamplesTop;
u32 gAbsFrames, gSampleFrom = 900;
int gSamplerCore = -1;

Bin* bin(u32 fn, Bin* bins = gBins)
{
    for (u32 h = (fn >> 2) * 2654435761u >> 19;; h++) {
        Bin& b = bins[h & (kBins - 1)];
        if (b.fn == fn) {
            return &b;
        }
        if (!b.fn) {
            b.fn = fn;
            return &b;
        }
    }
}

// One stack's sample into its bins (false: outside every instrumented function)
bool sampleStack(const Shadow& s, Bin* bins, u32* deep)
{
    u32 stack[kShadow];
    int d = s.depth;
    if (d <= 0) {
        return false;
    }
    if (d > kShadow) {
        d = kShadow;
        ++*deep;
    }
    for (int i = 0; i < d; i++) {
        stack[i] = s.at[i];
    }
    bin(stack[d - 1], bins)->self++;
    for (int i = 0; i < d; i++) {
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = stack[j] == stack[i];
        }
        if (!seen) {
            bin(stack[i], bins)->incl++;
        }
    }
    return true;
}

void sampler(void*)
{
    u32 gxDeep = 0, jobDeep = 0;
    while (gSamplerRun) {
        svcSleepThread(50000);
        if (!gSampling) {
            continue;
        }
        gSamples++;
        if (!sampleStack(gGame, gBins, &gSamplesDeep)) {
            gSamplesTop++;
        }
        if (re4_3ds_gx_tls) {
            gGxSamples++;
            if (!sampleStack(gGx, gGxBins, &gxDeep)) {
                gGxSamplesTop++;  // the thread's loop, waiting for work
            }
        }
        if (re4_3ds_job_tls) {
            gJobSamples++;
            if (!sampleStack(gJob, gJobBins, &jobDeep)) {
                gJobSamplesTop++;  // asleep or between jobs
            }
        }
    }
}

void samplerStart()
{
    const char* from = getenv("RE4_3DS_SAMPLE_FROM");
    if (from) {
        gSampleFrom = (u32) atoi(from);
    }
    gSamplerRun = true;
    // core 2 has the GX thread (stage 3.5): the sampler on core 1 then, if the system gives time there
    const int cores[2] = {re4_gxq_on ? 1 : 2, re4_gxq_on ? 2 : 1};
    for (int i = 0; i < 2 && gSamplerCore < 0; i++) {
        if (cores[i] == 1) {
            APT_SetAppCpuTimeLimit(30);
        }
        if (threadCreate(sampler, nullptr, 16 * 1024, 0x18, cores[i], true)) {
            gSamplerCore = cores[i];
        }
    }
    re4_log("3ds: sampler: thread on core %d, frames %lu..%lu", gSamplerCore, (unsigned long) gSampleFrom,
            (unsigned long) gSampleFrom + 300);
}

void samplerReport(const Bin* bins = gBins, const char* who = "")
{
    static Bin sorted[kBins];
    u32 n = 0;
    for (u32 i = 0; i < kBins; i++) {
        if (bins[i].fn) {
            sorted[n++] = bins[i];
        }
    }
    if (bins == gBins) {
        re4_log("3ds: sampler: %lu samples in 300 frames, %lu outside instrumented code, %lu deeper than %d, %lu functions",
                (unsigned long) gSamples, (unsigned long) gSamplesTop, (unsigned long) gSamplesDeep, kShadow, (unsigned long) n);
    } else if (bins == gGxBins) {
        re4_log("3ds: sampler gx: %lu samples in 300 frames, %lu outside instrumented code (the thread's loop), %lu functions",
                (unsigned long) gGxSamples, (unsigned long) gGxSamplesTop, (unsigned long) n);
    } else {
        re4_log("3ds: sampler job: %lu samples in 300 frames, %lu outside instrumented code (asleep), %lu functions",
                (unsigned long) gJobSamples, (unsigned long) gJobSamplesTop, (unsigned long) n);
    }
    for (int pass = 0; pass < 2; pass++) {
        // a few selection passes: the top 60 by own or by inclusive samples
        const int top = 60;
        for (int k = 0; k < top && k < (int) n; k++) {
            u32 best = k;
            for (u32 i = k + 1; i < n; i++) {
                if ((pass ? sorted[i].incl : sorted[i].self) > (pass ? sorted[best].incl : sorted[best].self)) {
                    best = i;
                }
            }
            Bin t = sorted[k];
            sorted[k] = sorted[best];
            sorted[best] = t;
        }
        for (int k = 0; k < top && k < (int) n; k += 6) {
            char line[400];
            int len = snprintf(line, sizeof(line), "3ds: sampler %s%s:", who, pass ? "incl" : "self");
            for (int j = k; j < k + 6 && j < (int) n; j++) {
                len += snprintf(line + len, sizeof(line) - len, " %08lx %lu/%lu", (unsigned long) sorted[j].fn,
                                (unsigned long) sorted[j].self, (unsigned long) sorted[j].incl);
            }
            re4_log("%s", line);
        }
    }
}

void samplerFrame()
{
    gAbsFrames++;
    if (!gShadowUsed) {
        return;
    }
    if (gSamplerCore < 0 && !gSamplerRun) {
        samplerStart();
    }
    if (gAbsFrames == gSampleFrom) {
        memset(gBins, 0, sizeof(gBins));
        memset(gGxBins, 0, sizeof(gGxBins));
        memset(gJobBins, 0, sizeof(gJobBins));
        gSamples = gSamplesTop = gSamplesDeep = 0;
        gGxSamples = gGxSamplesTop = 0;
        gJobSamples = gJobSamplesTop = 0;
        gSampling = true;
    } else if (gAbsFrames == gSampleFrom + 300) {
        gSampling = false;
        svcSleepThread(1000000);  // the sampler out of its last pass
        samplerReport();
        if (gGxSamples) {
            samplerReport(gGxBins, "gx ");
        }
        if (gJobSamples) {
            samplerReport(gJobBins, "job ");
        }
    }
}

}  // namespace

extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void* site)
{
    (void) site;
    Shadow& s = shadowHere();
    const int d = s.depth;
    if (d < kShadow) {
        s.at[d] = (u32) fn;
    }
    s.depth = d + 1;
    gShadowUsed = true;
}

extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* fn, void* site)
{
    (void) site;
    Shadow& s = shadowHere();
    const int d = s.depth;
    if (d <= 0) {
        return;
    }
    if (d <= kShadow && s.at[d - 1] != (u32) fn) {
        for (int k = d - 2; k >= 0 && k >= d - 8; k--) {
            if (s.at[k] == (u32) fn) {
                s.depth = k;
                return;
            }
        }
        return;
    }
    s.depth = d - 1;
}

namespace {

void shadowLog(const char* what, void* from, int code)
{
    char line[600];
    const Shadow& s = shadowHere();  // the thread that aborted
    const int d = s.depth;
    int len = snprintf(line, sizeof(line), "3ds: %s(%d) from %p, frame %lu; shadow stack (%d, top first):", what, code, from,
                       (unsigned long) gAbsFrames, d);
    for (int k = (d < kShadow ? d : kShadow) - 1; k >= 0 && k >= d - 24 && len < (int) sizeof(line) - 12; k--) {
        len += snprintf(line + len, sizeof(line) - len, " %08lx", (unsigned long) s.at[k]);
    }
    re4_log("%s", line);
}

}  // namespace

extern "C" void __real_abort(void);
extern "C" void re4_3ds_crash(const char* what, const char* detail);  // crash_3ds.cpp (R1)
extern "C" void __real_exit(int code);
extern "C" void re4_log_file_drain(void);  // runtime/log.cpp: the lines the writer thread has not written yet
extern "C" void re4_log_file_trim(void);   // runtime/log.cpp: the room kept ahead of the text off the file

extern "C" __attribute__((no_instrument_function)) void __wrap_abort(void)
{
    shadowLog("abort", __builtin_return_address(0), 0);
    {
        char at[48];  // (R1) sdmc:/3ds/pueblo/crash.txt (after std::terminate: written by it already)
        snprintf(at, sizeof(at), "abort called from %p", __builtin_return_address(0));
        re4_3ds_crash("abort", at);
    }
    re4_log_file_drain();
    re4_log_file_trim();
    __real_abort();
    for (;;) {
    }
}

extern "C" __attribute__((no_instrument_function)) void __wrap_exit(int code)
{
    shadowLog("exit", __builtin_return_address(0), code);
    re4_log_file_drain();
    re4_log_file_trim();
    __real_exit(code);
    for (;;) {
    }
}

// A game frame done (system_3ds.cpp re4_3ds_frame); with `log`, the lines of the frames so far.
extern "C" void re4_prof_frame(int log)
{
    samplerFrame();
    const u64 now = svcGetSystemTick();
    if (!gLast) {
        calibrate();
        gLast = gFrameAt = svcGetSystemTick();
        return;
    }
    gFrameTicks += now - gFrameAt;
    gFrameAt = now;
    charge(now);
    gFrames++;
    takeMarks();
    if (log) {
        report();
    }
}
