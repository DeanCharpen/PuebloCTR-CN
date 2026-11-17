// R1 (2026-11-12; the owner's decision R0: no log in the release, a small crash.txt for
// the beta's reports): sdmc:/3ds/pueblo/crash.txt when the game ends in a fault - a data abort, a prefetch abort, an
// undefined instruction (the CPU's exception, through the main thread's handler: libctru's threadOnException, the
// process's default for every thread), std::terminate (system_3ds.cpp onTerminate, a failed operator new among them)
// or abort (profile_3ds.cpp __wrap_abort). What is written: the version, the disc, the room, the thread, the
// registers, the return addresses on the stack (names: arm-none-eabi-addr2line -f -C -e re4.elf of that version), the
// C heap (not after a CPU fault: the fault may be inside malloc, its lock held), the log's last lines (runtime/log.cpp
// keeps 24 even without the file).
//
// The file is made at the start (re4_3ds_crash_start, main_3ds.cpp, before the game) with room for the report and kept
// open through the FS service itself (no newlib, no allocation of the SD card's clusters at the fault: 6.2e); a clean
// launch leaves "no crash" in it. A report of the launch before is kept as crash-previous.txt (rename over a file
// does not work on the 3DS: that one removed first). After the report: svcBreak (Luma3DS's screen, as before).
// RE4_3DS_CRASHFILE=0: none of it (the CPU's faults to Luma3DS's dumps, /luma/dumps/arm11, as before); 1 by default in
// the release build, 0 in the others (where the Luma dumps are the tool). RE4_3DS_CRASHTEST=data|terminate|abort (not
// in the release): that fault 600 frames into the gameplay, to see the file.
#include <3ds.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>  // (after stdlib.h: its declarations of malloc and free first)
#include <unistd.h>

#include <exception>

#include "re4_version.h"  // build3ds.py: RE4_3DS_VERSION

extern "C" {
extern volatile int re4_hud_room;  // runtime/cheats.cpp hudSample (R1): the room in the gameplay, -1 before
void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
int re4_log_tail(char* out, int size);           // runtime/log.cpp: the last lines
void re4_log(const char* fmt, ...);
extern char __start__[], __exidx_start[];
extern char* fake_heap_end;
void re4_3ds_crash(const char* what, const char* detail);
}

#ifndef RE4_3DS_RELEASE
#define RE4_3DS_RELEASE 0
#endif

namespace {

const char kPath[] = "/3ds/pueblo/crash.txt";
const char kPrevious[] = "/3ds/pueblo/crash-previous.txt";
const char kClean[] = "PuebloCTR: no crash in the last launch";
const u32 kRoom = 8192;  // the file's size made at the start (a report is shorter)

Handle gFile;
bool gOn;
volatile int gBusy;  // a report under way (a fault in it: svcBreak at once)
char gDisc[16] = "unknown";
u32 gFrames;
int gTest;  // RE4_3DS_CRASHTEST: 1 data, 2 terminate, 3 abort
u32 gInGame;

// the handler's own stack (a fault from a stack overflow could not run on the thread's)
alignas(8) u8 gStack[32 << 10];
char gText[6144];

FS_Archive sdmc()
{
    FS_Archive a = 0;
    FSUSER_OpenArchive(&a, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    return a;
}

// ' ' .. '~' and the end of a line, else '?'
void plain(char* s)
{
    for (; *s; s++) {
        if ((*s < ' ' || *s > '~') && *s != '\n') {
            *s = '?';
        }
    }
}

int put(int len, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int put(int len, const char* fmt, ...)
{
    if (len >= (int) sizeof(gText) - 1) {
        return len;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(gText + len, sizeof(gText) - (size_t) len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return len;
    }
    return len + n < (int) sizeof(gText) ? len + n : (int) sizeof(gText) - 1;
}

bool readable(u32 addr, u32 bytes)
{
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) {
        return false;
    }
    return (mi.perm & MEMPERM_READ) && mi.state != MEMSTATE_FREE && addr + bytes <= mi.base_addr + mi.size;
}

// the words on the stack that are return addresses into the code (after a BL / BLX), newest first
int calls(int len, u32 sp)
{
    len = put(len, "return addresses on the stack (sp %08lx):", (unsigned long) sp);
    const u32 lo = (u32) __start__ + 4, hi = (u32) __exidx_start;
    int found = 0;
    for (u32 i = 0; i < 2048 && found < 24; i++) {
        const u32 at = sp + i * 4;
        if ((at & 0xFFF) == 0 || i == 0) {
            if (!readable(at, 4)) {
                break;
            }
        }
        const u32 v = *(const u32*) at;
        if (v < lo || v >= hi || (v & 3)) {
            continue;
        }
        const u32 insn = *(const u32*) (v - 4);
        const bool bl = (insn & 0x0F000000) == 0x0B000000 && (insn >> 28) != 0xF;
        const bool blx = (insn & 0xFE000000) == 0xFA000000 || (insn & 0x0FFFFFF0) == 0x012FFF30;
        if (bl || blx) {
            len = put(len, " %08lx", (unsigned long) v);
            found++;
        }
    }
    return put(len, "\n");
}

int head(int len, const char* what)
{
    len = put(len, "PuebloCTR %s%s\n", RE4_3DS_VERSION, RE4_3DS_RELEASE ? "" : " (a development build)");
    len = put(len, "what: %s\n", what);
    len = put(len, "disc: %s; room: ", gDisc);
    const int room = re4_hud_room;
    len = room >= 0 ? put(len, "r%03x", room) : put(len, "none (not in the gameplay)");
    u32 id = 0;
    s32 prio = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    return put(len, "; frame %lu; thread %lu (priority 0x%02lx)\n", (unsigned long) gFrames, (unsigned long) id,
               (unsigned long) prio);
}

int tail(int len)
{
    len = put(len, "the last lines of the log:\n");
    if (len < (int) sizeof(gText) - 64) {
        len += re4_log_tail(gText + len, (int) sizeof(gText) - len - 1);
        gText[len] = 0;
    }
    return len;
}

void writeOut(int len)
{
    plain(gText);
    u32 done = 0;
    FSFILE_Write(gFile, &done, 0, gText, (u32) len, FS_WRITE_FLUSH);
    FSFILE_SetSize(gFile, (u64) len);  // (smaller: no cluster to find)
    FSFILE_Flush(gFile);
}

// the CPU's fault (the main thread's handler is every thread's): on gStack
void onException(ERRF_ExceptionInfo* e, CpuRegisters* r)
{
    if (__atomic_exchange_n(&gBusy, 1, __ATOMIC_ACQ_REL)) {
        svcBreak(USERBREAK_PANIC);
    }
    static const char* const kType[4] = {"prefetch abort", "data abort", "undefined instruction", "VFP exception"};
    char what[96];
    snprintf(what, sizeof(what), "%s (fsr %08lx, far %08lx)", e->type <= 3 ? kType[e->type] : "exception",
             (unsigned long) e->fsr, (unsigned long) e->far);
    int len = head(0, what);
    len = put(len, "pc %08lx lr %08lx sp %08lx cpsr %08lx\n", (unsigned long) r->pc, (unsigned long) r->lr,
              (unsigned long) r->sp, (unsigned long) r->cpsr);
    for (int i = 0; i < 13; i++) {
        len = put(len, "r%d %08lx%s", i, (unsigned long) r->r[i], i == 6 || i == 12 ? "\n" : " ");
    }
    len = calls(len, r->sp);
    len = put(len, "C heap: %lu KB not taken yet\n", (unsigned long) (fake_heap_end - (char*) sbrk(0)) >> 10);
    len = tail(len);
    writeOut(len);
    svcBreak(USERBREAK_PANIC);
    for (;;) {
    }
}

void frame(unsigned)
{
    gFrames++;
    if (!gTest) {
        return;
    }
    gInGame = re4_hud_room >= 0 ? gInGame + 1 : 0;
    if (gInGame == 600) {
        re4_log("3ds: RE4_3DS_CRASHTEST: the fault now (%d)", gTest);
        if (gTest == 1) {
            *(volatile u32*) 0x10 = 1;
        } else if (gTest == 2) {
            std::terminate();
        } else {
            abort();
        }
    }
}

void disc()
{
    const char* dir = getenv("RE4_DATA_DIR");
    if (!dir) {
        return;
    }
    static const char* const kWhere[2] = {"%s/disc0/sys/boot.bin", "%s/sys/boot.bin"};
    for (int i = 0; i < 2; i++) {
        char path[256];
        snprintf(path, sizeof(path), kWhere[i], dir);
        FILE* f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        char id[8] = {0};
        const size_t n = fread(id, 1, 7, f);
        fclose(f);
        if (n == 7) {
            snprintf(gDisc, sizeof(gDisc), "%.6s disc %d", id, id[6] + 1);
            plain(gDisc);
        }
        return;
    }
}

}  // namespace

// main_3ds.cpp, the main thread before the game (the settings read): the file, the handler, the frame hook
extern "C" void re4_3ds_crash_start(void)
{
    const char* v = getenv("RE4_3DS_CRASHFILE");
    gOn = v && *v ? *v != '0' : RE4_3DS_RELEASE != 0;
#if !RE4_3DS_RELEASE
    if (const char* t = getenv("RE4_3DS_CRASHTEST")) {
        gTest = !strcmp(t, "data") ? 1 : !strcmp(t, "terminate") ? 2 : !strcmp(t, "abort") ? 3 : 0;
    }
#endif
    re4_add_frame_hook(frame);
    if (!gOn) {
        return;
    }
    disc();
    // the report of the launch before kept
    FILE* f = fopen("sdmc:/3ds/pueblo/crash.txt", "rb");
    if (f) {
        char first[sizeof(kClean)] = {0};
        const size_t n = fread(first, 1, sizeof(first) - 1, f);
        fclose(f);
        if (n && memcmp(first, kClean, sizeof(kClean) - 1) != 0) {
            remove("sdmc:/3ds/pueblo/crash-previous.txt");
            rename("sdmc:/3ds/pueblo/crash.txt", "sdmc:/3ds/pueblo/crash-previous.txt");
        }
    }
    FS_Archive a = sdmc();
    Result r = FSUSER_OpenFile(&gFile, a, fsMakePath(PATH_ASCII, kPath), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_SUCCEEDED(r)) {
        // the room first (the clusters now), then the clean line
        static char pad[1024];
        memset(pad, '\n', sizeof(pad));
        u32 done = 0;
        for (u32 at = 0; at < kRoom && R_SUCCEEDED(r); at += sizeof(pad)) {
            r = FSFILE_Write(gFile, &done, at, pad, sizeof(pad), 0);
        }
        const int len = snprintf(gText, sizeof(gText), "%s (PuebloCTR %s)\n", kClean, RE4_3DS_VERSION);
        FSFILE_Write(gFile, &done, 0, gText, (u32) len, FS_WRITE_FLUSH);
    }
    FSUSER_CloseArchive(a);
    if (R_FAILED(r)) {
        re4_log("3ds: crash file %s not made (0x%08lx)", kPath, (unsigned long) r);
        gFile = 0;
        gOn = false;
        return;
    }
    threadOnException(onException, gStack + sizeof(gStack), WRITE_DATA_TO_HANDLER_STACK);
    re4_log("3ds: crash file sdmc:%s ready (%s), the CPU's faults to it", kPath, gDisc);
}

// system_3ds.cpp onTerminate, profile_3ds.cpp __wrap_abort: the first of them writes (terminate ends in abort)
extern "C" void re4_3ds_crash(const char* what, const char* detail)
{
    if (!gOn || __atomic_exchange_n(&gBusy, 1, __ATOMIC_ACQ_REL)) {
        return;
    }
    int len = head(0, what);
    if (detail && *detail) {
        len = put(len, "%s\n", detail);
    }
    u32 sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    len = calls(len, sp);
    struct mallinfo mi = mallinfo();
    len = put(len, "C heap: %u KB in use, %u KB free in the arena, %lu KB not taken yet; linear memory %lu KB free\n",
              (unsigned) mi.uordblks >> 10, (unsigned) mi.fordblks >> 10,
              (unsigned long) (fake_heap_end - (char*) sbrk(0)) >> 10, (unsigned long) linearSpaceFree() >> 10);
    len = tail(len);
    writeOut(len);
}
