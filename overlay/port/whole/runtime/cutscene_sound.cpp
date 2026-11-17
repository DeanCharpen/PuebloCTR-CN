// The cutscenes' sound at the game's speed (plan 6.1b, 2026-11-08, the project owner's choice), compiled with the
// game's flags into libre4_main. An event (cutscene) steps a frame a game frame - 29.97 a second on the GameCube - and
// starts its voices and music (streams, game/snd_str*.cpp) in step with it; from there the sound runs on the AI's
// clock, in real time (runtime/ai.cpp: 200 blocks of 5 ms a second from the video fields). On the 3DS a frame takes
// 35-60 ms: the picture fell behind the sound (r120's car: 4.8 s by the end of its first stream). Here every game
// frame during an event the AI's clock gets a speed (re4_audio_speed_q16):
//   s = 33.37 ms / the mean frame time - (the sound's lead in ms) / RE4_EVT_SNDTAU (1000 ms), in RE4_EVT_SNDMIN (0.6)
//   .. 1.05; the lead: the AI blocks since the event began x 5 ms, less its frames x 33.37 ms;
// out of events 1. The 3DS's output stretches the sound in time to it, the pitch kept (3ds/audio_3ds.cpp, WSOLA:
// re4_audio_stretch_on); where nothing stretches (Linux) this does nothing. RE4_EVT_SNDSPEED=0: off.
// (The first try, 2026-11-08, slowed only the streams' voices by their rate: the pitch went down with it - rejected.)
#include "types.h"
#include "global.h"

#include <stdlib.h>

extern GLOBAL_WK* pG;

extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" unsigned re4_ai_blocks(void);                     // runtime/ai.cpp
extern "C" volatile unsigned re4_audio_speed_q16;            // runtime/ai.cpp
extern "C" volatile int re4_audio_stretch_on;                // runtime/ai.cpp (3ds/audio_3ds.cpp sets it)
extern "C" unsigned long long re4_os_time_us(void);          // below, through OSGetTime

namespace {

const double kFrameMs = 1000.0 / 29.97;  // the event's frame (EVT_FRAME_RATE, game/event.cpp)
const double kBlockMs = 5.0;             // an AI block (runtime/ai.cpp kBlockFrames at 32 kHz)

double gTau = 1000.0, gMin = 0.6;
const double kMax = 1.05;
unsigned long long gLast;
double gMeanMs;
double gSpeed = 1.0;
bool gEvent;
unsigned gBlocks0;
// per event, for the log
unsigned gFrames, gSlowed;
double gSpeedSum, gSpeedMin, gLeadMax, gLead;

void setSpeed(double s)
{
    gSpeed = s;
    re4_audio_speed_q16 = (unsigned) (s * 65536.0 + 0.5);
}

void eventLine(const char* what)
{
    re4_log("snd speed: %s r%03x: %u frames, the sound %.0f ms ahead (at most %.0f); slower in %u frames, speed mean "
            "%.3f, least %.3f",
            what, pG ? pG->room_id : 0, gFrames, gLead, gLeadMax, gSlowed, gSlowed ? gSpeedSum / gSlowed : 1.0,
            gSlowed ? gSpeedMin : 1.0);
}

void hook(unsigned)
{
    if (!re4_audio_stretch_on) {
        return;
    }
    const unsigned long long now = re4_os_time_us();
    const double ms = gLast ? (double) (now - gLast) / 1000.0 : kFrameMs;
    gLast = now;
    gMeanMs = gMeanMs > 0.0 ? gMeanMs + 0.15 * (ms - gMeanMs) : ms;
    const bool event = pG && StaFlagChk(pG, STA_EVENT);
    if (event && !gEvent) {
        gBlocks0 = re4_ai_blocks();
        gFrames = gSlowed = 0;
        gSpeedSum = 0.0;
        gSpeedMin = 1.0;
        gLeadMax = gLead = 0.0;
    }
    if (!event) {
        if (gEvent) {
            eventLine("event ends in");
        }
        gEvent = false;
        if (gSpeed != 1.0) {
            setSpeed(1.0);
        }
        return;
    }
    gEvent = true;
    gFrames++;
    gLead = (double) (re4_ai_blocks() - gBlocks0) * kBlockMs - gFrames * kFrameMs;
    gLeadMax = gLead > gLeadMax ? gLead : gLeadMax;
    double s = kFrameMs / (gMeanMs > kFrameMs ? gMeanMs : kFrameMs) - gLead / gTau;
    s = s < gMin ? gMin : s > kMax ? kMax : s;
    s = s > 0.995 && s < 1.005 ? 1.0 : s;  // (at 1 the output is the input itself)
    setSpeed(s);
    if (s < 1.0) {
        gSlowed++;
        gSpeedSum += s;
        gSpeedMin = s < gSpeedMin ? s : gSpeedMin;
    }
    if (gFrames % 300 == 0) {
        eventLine("so far in");
    }
}

}  // namespace

// runtime/state_trace.cpp configure (with the cheats' hook)
extern "C" void re4_cutscene_sound_install(void)
{
    static bool installed;
    if (installed) {
        return;
    }
    installed = true;
    const char* on = getenv("RE4_EVT_SNDSPEED");
    if (on && *on == '0') {
        re4_log("snd speed: off (RE4_EVT_SNDSPEED=0)");
        return;
    }
    if (const char* tau = getenv("RE4_EVT_SNDTAU")) {
        gTau = atof(tau) > 1.0 ? atof(tau) : gTau;
    }
    if (const char* least = getenv("RE4_EVT_SNDMIN")) {
        gMin = atof(least) > 0.2 ? atof(least) : gMin;
    }
    re4_log("snd speed: the cutscenes' sound at the game's speed where the output stretches (correction over %.0f ms, "
            "at least %.2f)",
            gTau, gMin);
    re4_add_frame_hook(hook);
}
