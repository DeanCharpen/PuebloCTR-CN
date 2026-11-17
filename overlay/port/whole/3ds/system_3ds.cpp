// The platform under the core on the 3DS (stage 4.4): memory layout, the POSIX pieces the
// runtime uses (include/: sys/mman.h, link.h), and the time of each game frame.
//
// Memory: the application heap of a 3DS program starts at 0x08000000 (OS_HEAP_AREA_BEGIN), where
// the core wants MEM1 (RE4_MEM1_BASE, the address checks of stage 2: every heap at or above the
// base). __system_allocateHeaps replaces libctru's: MEM1 spans RE4_MEM1_SIZE bytes (48 MB, the address checks), but
// memory is allocated only for its retail 24 MB (RE4_MEM1_RETAIL_SIZE, all the game uses on 32 bit) and the port's
// places of the European discs' larger core.das and option.dat (4 MB; stage 5.7c: the other 20 MB go to the linear heap
// and the C heap); the rest of MEM1 and one page after it have no memory (a write there faults at once, as in sms-3ds),
// and the C library's heap starts above them.
#include <3ds.h>

#include <errno.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>  // after stdlib.h: newlib's calloc there is noexcept, here not
#include <new>
#include <exception>
#include <typeinfo>
#include <cxxabi.h>
#include <unwind.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "re4_runtime.h"

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;
extern char __start__[];
extern char __end__[];
}

// The main thread's stack (libctru takes it from the start of the C heap, right above the guard
// page): the dispatcher of the OS thread emulation runs on it (re4_os_run). 32 KB by default.
extern "C" u32 __stacksize__ = 1u << 20;

namespace {

const u32 kPage = 0x1000;
u32 gShort;  // bytes of memory missing for MEM1 (re4_3ds_memory_ok)
// the renderer's textures and display list cache; 8, 12, 13 until 01.11.2026, 14 until 02.11.2026 (MEM1 all allocated)
const u32 kLinearHeap = 30u << 20;
const u32 kMem1End = RE4_MEM1_BASE + RE4_MEM1_SIZE;
// MEM1 with memory: the retail part, and the places of the larger core.das and option.dat (runtime/dvd.cpp)
const u32 kMem1Kept[2][2] = {{RE4_MEM1_BASE, RE4_MEM1_BASE + RE4_MEM1_RETAIL_SIZE},
                             {RE4_MEM1_CORE_ALT_ADDR, RE4_MEM1_OPTION_ALT_ADDR + RE4_MEM1_OPTION_ALT_MAX}};
static_assert(RE4_MEM1_CORE_ALT_ADDR + RE4_MEM1_CORE_ALT_MAX == RE4_MEM1_OPTION_ALT_ADDR, "one range");
const u32 kMem1KeptBytes = kMem1Kept[0][1] - kMem1Kept[0][0] + kMem1Kept[1][1] - kMem1Kept[1][0];
const u32 kHeapStart = kMem1End + kPage;  // the C library's heap

bool inMem1(const void* p, size_t length)
{
    u32 a = (u32) p;
    return a >= RE4_MEM1_BASE && a < kMem1End && length <= kMem1End - a;
}

}  // namespace

extern "C" void __system_allocateHeaps(void)
{
    // what the process may still commit (as libctru: the COMMIT resource limit)
    Handle limit = 0;
    s64 max = 0, used = 0;
    ResourceLimitType commit = RESLIMIT_COMMIT;
    if (R_FAILED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
        svcBreak(USERBREAK_PANIC);
    }
    svcGetResourceLimitLimitValues(&max, limit, &commit, 1);
    svcGetResourceLimitCurrentValues(&used, limit, &commit, 1);
    svcCloseHandle(limit);
    u32 free = (u32) (max - used) & ~(kPage - 1);
    const u32 need = kMem1KeptBytes + kLinearHeap + __stacksize__ + (8u << 20);
    if (free < need) {
        gShort = need - free;  // no MEM1: main says so on the screen
    }
    // (R3) short (an Old 3DS: free may be below the linear heap itself): a small linear heap, for the screens
    // (startcheck_3ds.cpp says so; MEM1 not mapped)
    __ctru_linear_heap_size = !gShort ? kLinearHeap : free / 2 < (4u << 20) ? (free / 2) & ~(kPage - 1) : 4u << 20;
    __ctru_heap_size = free - __ctru_linear_heap_size;
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    u32 at = 0;
    if (!gShort) {
        // MEM1's pieces with memory; libctru's heap variables then name only the C library's heap above MEM1
        for (const u32* r : kMem1Kept) {
            if (R_FAILED(svcControlMemory(&at, r[0], 0, r[1] - r[0], MEMOP_ALLOC, MEMPERM_READWRITE))) {
                svcBreak(USERBREAK_PANIC);
            }
        }
        __ctru_heap_size -= kMem1KeptBytes;
        __ctru_heap = kHeapStart;
    }
    if (R_FAILED(svcControlMemory(&at, __ctru_heap, 0, __ctru_heap_size, MEMOP_ALLOC, MEMPERM_READWRITE))) {
        svcBreak(USERBREAK_PANIC);
    }
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  MEMPERM_READWRITE))) {
        svcBreak(USERBREAK_PANIC);
    }
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char*) __ctru_heap;
    fake_heap_end = (char*) (__ctru_heap + __ctru_heap_size);
}

// 0 when MEM1 did not fit (the bytes missing are on the screen)
extern "C" int re4_3ds_memory_ok(void)
{
    if (gShort) {
        printf("not enough memory: MEM1 (%u MB) needs %u KB more than the system gives (%u MB heap)\n",
               RE4_MEM1_SIZE >> 20, (unsigned) gShort >> 10, (unsigned) __ctru_heap_size >> 20);
    }
    return !gShort;
}

extern "C" void re4_3ds_memory_report(void)
{
    re4_log("3ds: image 0x%08x..0x%08x (%u KB), MEM1 0x%08x..0x%08x (memory in %u KB of it), C heap 0x%08x..0x%08x "
            "(%u MB), linear heap %u MB at 0x%08x",
            (u32) __start__, (u32) __end__, (u32) (__end__ - __start__) >> 10, RE4_MEM1_BASE, kMem1End,
            kMem1KeptBytes >> 10,
            (u32) fake_heap_start, (u32) fake_heap_end, (u32) (fake_heap_end - fake_heap_start) >> 20,
            __ctru_linear_heap_size >> 20, __ctru_linear_heap);
}

// --- the stacks of the game's threads -------------------------------------------------------------
// runtime/os_thread.cpp kThreadStack (256 KB here since stage 6.2: a new thread's 1 MB found no room in the C heap on
// the radio's screen, r100-3ds-live6.log), mapped by port/src/stack_context.cpp with a page below and above. Painted
// when mapped; a stack used into its lowest KB ends the game (no guard page: mprotect does nothing here), and the
// deepest use of them goes into the log with the frame times (re4_3ds_frame).
// The stacks themselves from a pool made at the first one (stage 6.3: 19 minutes into the user's game a new thread's
// stack found no room in r106, the C heap 29 of 35 MB in use and broken up, play-texmiss1.log); an unmapped stack goes
// back into it, beyond it from the heap as before.

namespace {

const u32 kThreadStack = 256u << 10;
const u32 kPaint = 0x57ACC0DEu;
const u32 kStacksMax = 32;
u32* gStacks[kStacksMax];  // the lowest word of each live thread stack (above the page below it)
u32 gDeepest;              // bytes, the most any stack used so far
// (2026-11-11) the main thread's stack, os_thread.cpp kMainStack: painted, its lowest KB checked as the threads'
const u32 kMainStackBytes = 512u << 10;
u32* gMainBottom;          // its lowest word (above the page below it); NULL: not mapped yet
u32 gMainDeepest;          // bytes

u32 stackMapping()
{
    const u32 page = (u32) sysconf(_SC_PAGESIZE);
    return kThreadStack + 2 * page;
}

void stackMapped(void* p)
{
    u32* bottom = (u32*) ((char*) p + (u32) sysconf(_SC_PAGESIZE));
    for (u32 i = 0; i < kThreadStack / 4; i++) {
        bottom[i] = kPaint;
    }
    for (auto& s : gStacks) {
        if (!s) {
            s = bottom;
            return;
        }
    }
}

void stackUnmapped(void* p)
{
    u32* bottom = (u32*) ((char*) p + (u32) sysconf(_SC_PAGESIZE));
    for (auto& s : gStacks) {
        if (s == bottom) {
            s = nullptr;
        }
    }
}

const u32 kStackPool = 12;  // live at once: 10 at most in r104 (play-keepdrop1.log)
void* gStackPool[kStackPool];
bool gStackTaken[kStackPool];
u32 gStackBeyond;  // stacks taken from the heap beyond the pool
u32 gStackLive, gStackPeak;  // stacks mapped now, the most at once

void* stackTake()
{
    gStackPeak = ++gStackLive > gStackPeak ? gStackLive : gStackPeak;
    if (!gStackPool[0]) {
        for (auto& p : gStackPool) {
            p = memalign(kPage, stackMapping());
        }
    }
    for (u32 i = 0; i < kStackPool; i++) {
        if (gStackPool[i] && !gStackTaken[i]) {
            gStackTaken[i] = true;
            return gStackPool[i];
        }
    }
    gStackBeyond++;
    return memalign(kPage, stackMapping());
}

// false: not the pool's (the caller frees it)
bool stackGive(void* p)
{
    for (u32 i = 0; i < kStackPool; i++) {
        if (gStackPool[i] == p) {
            gStackTaken[i] = false;
            return true;
        }
    }
    return false;
}

// Each frame the lowest KB of every stack; with `log`, the deepest use (the whole stack scanned)
void checkStacks(bool log)
{
    u32 live = 0;
    for (u32* s : gStacks) {
        if (!s) {
            continue;
        }
        live++;
        for (u32 i = 0; i < 256; i++) {
            if (s[i] != kPaint) {
                re4_log("3ds: the stack of a game thread (%p..%p) ran into its last KB: the game ends", (void*) s,
                        (void*) (s + kThreadStack / 4));
                printf("a game thread's stack overflowed (log: sdmc:/3ds/pueblo/pueblo.log)\n");
                abort();
            }
        }
        if (log) {
            u32 i = 256;
            while (i < kThreadStack / 4 && s[i] == kPaint) {
                i++;
            }
            const u32 used = kThreadStack - i * 4;
            gDeepest = used > gDeepest ? used : gDeepest;
        }
    }
    if (log) {
        re4_log("3ds: game thread stacks: %u live, deepest use %u KB of %u; at most %u at once, a pool of %u (%u taken "
                "from the heap beyond it)", (unsigned) live, (unsigned) gDeepest >> 10, (unsigned) kThreadStack >> 10,
                (unsigned) gStackPeak, (unsigned) kStackPool, (unsigned) gStackBeyond);
    }
    if (gMainBottom) {  // (2026-11-11) the main thread's stack
        for (u32 i = 0; i < 256; i++) {
            if (gMainBottom[i] != kPaint) {
                re4_log("3ds: the main thread's stack (%p..%p) ran into its last KB: the game ends", (void*) gMainBottom,
                        (void*) (gMainBottom + kMainStackBytes / 4));
                printf("the main thread's stack overflowed (log: sdmc:/3ds/pueblo/pueblo.log)\n");
                abort();
            }
        }
        if (log) {
            u32 i = 256;
            while (i < kMainStackBytes / 4 && gMainBottom[i] == kPaint) {
                i++;
            }
            const u32 used = kMainStackBytes - i * 4;
            gMainDeepest = used > gMainDeepest ? used : gMainDeepest;
            re4_log("3ds: the main thread's stack: the deepest use %u KB of %u", (unsigned) gMainDeepest >> 10,
                    (unsigned) kMainStackBytes >> 10);
        }
    }
}

}  // namespace

// --- sys/mman.h ------------------------------------------------------------------------------

extern "C" void* mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void) prot;
    (void) offset;
    if (!(flags & MAP_ANONYMOUS) || fd != -1) {
        errno = ENODEV;
        return MAP_FAILED;
    }
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (!inMem1(address, length)) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        u32 a = (u32) address, end = a + length;
        for (const u32* r : kMem1Kept) {  // only MEM1's pieces with memory
            u32 lo = a > r[0] ? a : r[0], hi = end < r[1] ? end : r[1];
            if (lo < hi) {
                memset((void*) lo, 0, hi - lo);
            }
        }
        return address;
    }
    const bool stack = length == stackMapping() && prot == PROT_NONE;
    void* p = stack ? stackTake() : memalign(kPage, length);
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (stack) {
        stackMapped(p);  // a game thread's stack (port/src/stack_context.cpp): painted, not cleared
    } else if (prot == PROT_NONE && length == kMainStackBytes + 2 * kPage && !gMainBottom) {
        gMainBottom = (u32*) ((char*) p + kPage);  // the main thread's stack: painted, not cleared (2026-11-11)
        for (u32 i = 0; i < kMainStackBytes / 4; i++) {
            gMainBottom[i] = kPaint;
        }
    } else {
        memset(p, 0, length);
    }
    return p;
}

extern "C" int munmap(void* address, size_t length)
{
    if (!inMem1(address, length)) {
        if (length == stackMapping()) {
            stackUnmapped(address);
            gStackLive--;
            if (stackGive(address)) {
                return 0;
            }
        }
        free(address);
    }
    return 0;
}

extern "C" int mprotect(void* address, size_t length, int prot)
{
    (void) address;
    (void) length;
    (void) prot;
    return 0;
}

// --- link.h ----------------------------------------------------------------------------------

extern "C" int dl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data)
{
    static Elf32_Phdr image;
    image.p_type = PT_LOAD;
    image.p_vaddr = (u32) __start__;
    image.p_memsz = (u32) (__end__ - __start__);
    image.p_filesz = image.p_memsz;
    image.p_flags = PF_R | PF_W | PF_X;
    struct dl_phdr_info info;
    info.dlpi_addr = 0;
    info.dlpi_name = "libre4_main";
    info.dlpi_phdr = &image;
    info.dlpi_phnum = 1;
    return callback(&info, sizeof(info), data);
}

// --- sys/syscall.h ---------------------------------------------------------------------------

extern "C" long re4_3ds_syscall(long number)
{
    u32 id = 0;
    if (number == SYS_gettid && R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE))) {
        return (long) id;
    }
    errno = ENOSYS;
    return -1;
}

// --- operator new: a failure into the log before std::bad_alloc --------------------------------
extern "C" char __start__[], __exidx_start[];  // (the linker's: the code, for newFailed's return addresses)
// (stage 6.2: an abort from std::terminate on the radio's screen, r100-3ds-live3.log, said nothing more)

namespace {

__attribute__((noinline)) void newFailed(size_t n, void* from)
{
    struct mallinfo mi = mallinfo();
    u32 id = 0;
    s32 prio = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // (2026-11-08) the return addresses on the stack: a word in the code after a BL / BLX (the callers, newest first)
    const u32 lo = (u32) __start__ + 4, hi = (u32) __exidx_start;
    u32 sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    char calls[220];
    int len = 0, found = 0;
    calls[0] = 0;
    for (u32 i = 0; i < 768 && found < 16; i++) {
        const u32 v = ((const u32*) sp)[i];
        if (v < lo || v >= hi || (v & 3)) {
            continue;
        }
        const u32 insn = *(const u32*) (v - 4);
        const bool bl = (insn & 0x0F000000) == 0x0B000000 && (insn >> 28) != 0xF;
        const bool blx = (insn & 0xFE000000) == 0xFA000000 || (insn & 0x0FFFFFF0) == 0x012FFF30;
        if (bl || blx) {
            len += snprintf(calls + len, sizeof(calls) - len, " %08lx", (unsigned long) v);
            found++;
            if (len >= (int) sizeof(calls) - 10) {
                break;
            }
        }
    }
    re4_log("3ds: operator new of %u bytes failed (from %p, thread %lu, priority 0x%02lx): C heap %u KB in use, %u KB "
            "free in the arena, %u KB not taken yet; return addresses on the stack:%s",
            (unsigned) n, from, (unsigned long) id, (unsigned long) prio, (unsigned) mi.uordblks >> 10,
            (unsigned) mi.fordblks >> 10, (unsigned) (fake_heap_end - (char*) sbrk(0)) >> 10, calls);
}

}  // namespace

void* operator new(size_t n)
{
    void* from = __builtin_return_address(0);  // (taken here: the cold part read lr after its own call)
    asm volatile("" : "+r"(from));
    void* p = malloc(n ? n : 1);
    if (!p) {
        newFailed(n, from);
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](size_t n)
{
    void* from = __builtin_return_address(0);
    asm volatile("" : "+r"(from));
    void* p = malloc(n ? n : 1);
    if (!p) {
        newFailed(n, from);
        throw std::bad_alloc();
    }
    return p;
}

// --- std::terminate: the exception and the stack into the log --------------------------------
// (stage 6.2: the radio's screen ended in std::terminate without a failed operator new, r100-3ds-live4.log);
// the addresses by the unwinder (ARM EHABI tables), their names: arm-none-eabi-addr2line -f -C -e re4.elf

namespace {

struct Trace {
    u32 pc[24];
    int n;
};

_Unwind_Reason_Code traceStep(struct _Unwind_Context* context, void* arg)
{
    Trace& t = *(Trace*) arg;
    if (t.n >= 24) {
        return _URC_END_OF_STACK;
    }
    t.pc[t.n++] = (u32) _Unwind_GetIP(context);
    return _URC_NO_REASON;
}

extern "C" void re4_3ds_crash(const char* what, const char* detail);  // crash_3ds.cpp (R1)

[[noreturn]] void onTerminate()
{
    const std::type_info* type = abi::__cxa_current_exception_type();
    const char* what = "";
    if (type) {
        try {
            throw;
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
    }
    Trace t;
    t.n = 0;
    _Unwind_Backtrace(traceStep, &t);
    char line[400];
    int len = snprintf(line, sizeof(line), "3ds: std::terminate: exception %s (%s); stack:", type ? type->name() : "none",
                       what);
    for (int k = 0; k < t.n && len < (int) sizeof(line) - 12; k++) {
        len += snprintf(line + len, sizeof(line) - len, " %08lx", (unsigned long) t.pc[k]);
    }
    re4_log("%s", line);
    re4_3ds_crash("std::terminate", line);  // (R1) sdmc:/3ds/pueblo/crash.txt
    abort();
}

__attribute__((constructor)) void installTerminate()
{
    std::set_terminate(onTerminate);
}

}  // namespace

// --- pread/pwrite (runtime/host_file.cpp), thread names ---------------------------------------
// newlib has neither: seek, transfer, seek back, under one lock (the offset of the descriptor is
// what pread must leave alone, and another thread may use the same descriptor meanwhile)

namespace {

pthread_mutex_t gPositionLock = PTHREAD_MUTEX_INITIALIZER;

ssize_t atOffset(int fd, void* buffer, size_t count, off_t offset, bool write)
{
    pthread_mutex_lock(&gPositionLock);
    ssize_t result = -1;
    off_t was = lseek(fd, 0, SEEK_CUR);
    if (was >= 0 && lseek(fd, offset, SEEK_SET) == offset) {
        result = write ? ::write(fd, buffer, count) : ::read(fd, buffer, count);
        int error = errno;
        lseek(fd, was, SEEK_SET);
        errno = error;
    }
    pthread_mutex_unlock(&gPositionLock);
    return result;
}

}  // namespace

extern "C" ssize_t pread(int fd, void* buffer, size_t count, off_t offset)
{
    return atOffset(fd, buffer, count, offset, false);
}

extern "C" ssize_t pwrite(int fd, const void* buffer, size_t count, off_t offset)
{
    return atOffset(fd, (void*) buffer, count, offset, true);
}

extern "C" int pthread_setname_np(pthread_t thread, const char* name)
{
    (void) thread;
    (void) name;
    return 0;  // threads of libctru have no names
}

// --- the time of each game frame (state_trace.cpp, RE4_STOP_AFTER counts the same frames) ----

extern "C" int re4_3ds_hud_mode(void);  // settings_3ds.cpp (6.6): the panel on the bottom screen
extern "C" int re4_3ds_hud_owns_console(void);
extern "C" void re4_3ds_hud_frame(double ms);
extern "C" void re4_3ds_hud_stop(void);

namespace {

struct FrameStats {
    u64 last;
    u32 frames;
    double sum, min, max;
    u32 over[4];  // above 16.7, 33.3, 50, 100 ms
};
FrameStats gAll, gWindow;
const u32 kWindow = 300;

void add(FrameStats& s, double ms)
{
    if (s.frames == 0 || ms < s.min) {
        s.min = ms;
    }
    if (ms > s.max) {
        s.max = ms;
    }
    s.sum += ms;
    s.frames++;
    const double limits[4] = {1000.0 / 60, 1000.0 / 30, 50, 100};
    for (int i = 0; i < 4; i++) {
        s.over[i] += ms > limits[i];
    }
}

void report(const char* what, const FrameStats& s)
{
    if (!s.frames) {
        return;
    }
    char line[200];
    snprintf(line, sizeof(line), "%s: %lu frames, avg %.2f ms, min %.2f, max %.2f; >16.7: %lu, >33.3: %lu, >50: %lu, >100: %lu",
             what, (unsigned long) s.frames, s.sum / s.frames, s.min, s.max, (unsigned long) s.over[0],
             (unsigned long) s.over[1], (unsigned long) s.over[2], (unsigned long) s.over[3]);
    re4_log("3ds: %s", line);
    if (!re4_3ds_hud_owns_console()) {
        printf("%s\n", line);
    }
}

}  // namespace

extern "C" void re4_gxq_frame(int log);  // gx_thread_3ds.cpp: gx_3ds.cpp re4_3ds_gx_frame on the GX thread

namespace {

// The frame rate on the bottom screen, once a second (stage 6.2, a wish of the project owner): the game's
// frames in that second and their mean time, in the console's first row (its cursor kept). RE4_3DS_FPS=0: none.
void showFps(u64 now)
{
    static int on = -1;
    static u64 from;
    static u32 frames;
    if (on < 0) {
        on = re4_3ds_hud_mode() == 1;  // RE4_3DS_HUD=1 (RE4_3DS_FPS=0: 0; 2: the panel, settings_3ds.cpp)
    }
    if (!on) {
        return;
    }
    if (!from) {
        from = now;
        return;
    }
    frames++;
    const double ms = (double) (now - from) / CPU_TICKS_PER_MSEC;
    if (ms < 1000.0) {
        return;
    }
    printf("\x1b[s\x1b[1;1H%5.1f fps, %5.1f ms a frame   \x1b[u", frames * 1000.0 / ms, ms / frames);
    from = now;
    frames = 0;
}

}  // namespace

extern "C" void re4_3ds_log_fpscr(const char* who);  // main_3ds.cpp: FPSCR of the calling thread

extern "C" void re4_3ds_frame(void)
{
    u64 now = svcGetSystemTick();
    if (gAll.frames == 600) {
        re4_3ds_log_fpscr("game thread at frame 600");  // (stage 3.6: whether the game or the runtime changed it)
    }
    showFps(now);
    checkStacks(gWindow.frames + 1 == kWindow);
    re4_prof_frame(gWindow.frames + 1 == kWindow);
    re4_gxq_frame(gWindow.frames + 1 == kWindow);
    if (gAll.last) {
        double ms = (double) (now - gAll.last) / CPU_TICKS_PER_MSEC;
        add(gAll, ms);
        add(gWindow, ms);
        re4_3ds_hud_frame(ms);
        if (gWindow.frames == kWindow) {
            char what[32];
            snprintf(what, sizeof(what), "frames to %lu", (unsigned long) gAll.frames);
            report(what, gWindow);
            memset(&gWindow, 0, sizeof(gWindow));
        }
    }
    gAll.last = now;
}

// The end of a run (RE4_STOP_AFTER, or re4_game_main returned): the totals on the screen until START.
extern "C" void re4_3ds_audio_stop(void);  // audio_3ds.cpp
extern "C" void re4_log_file_drain(void);   // runtime/log.cpp: the log's last lines (the writer thread, main_3ds.cpp)
extern "C" void re4_log_file_trim(void);    // runtime/log.cpp: the room kept ahead of the text off the file

extern "C" void re4_gxq_park(int on);  // gx_thread_3ds.cpp
extern "C" void re4_gxq_quit(int fini);  // gx_thread_3ds.cpp (R1b)
// (R1b) RE4_3DS_EXITMODE (stageR1b_exit_edits.py): 2 (the default) the threads joined + gfxExit - 8 of 8 ends clean on
// the console (2026-11-16; before: 2 of 2 crashes); 0 _exit at once (as before), 1 500 ms first, 3 as 2 with C3D_Fini
static int exitMode()
{
    const char* v = getenv("RE4_3DS_EXITMODE");
    return v && *v ? atoi(v) : 2;
}

extern "C" void re4_3ds_finish(int code)
{
    re4_gxq_park(1);  // (R1) the GX thread and the GPU done before the process ends (the release ends at once)
    re4_3ds_audio_stop();
    re4_3ds_hud_stop();
    report("all frames", gAll);
    re4_log("3ds: the run ended (code %d)", code);
    re4_log_file_drain();
#ifndef RE4_3DS_RELEASE
    printf("\nrun ended (code %d), log: sdmc:/3ds/pueblo/pueblo.log\npress START to exit\n", code);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gspWaitForVBlank();
    }
#endif  // (R1: the release ends at once)
    const int mode = exitMode();  // (R1b)
    re4_log("3ds: the end, mode %d (RE4_3DS_EXITMODE)", mode);
    if (mode >= 2) {
        re4_gxq_quit(mode >= 3);
    }
    re4_log_file_drain();
    re4_log_file_trim();  // (R1c) the last touch of the log: a line after it makes the file's room again (seconds)
    if (mode == 1) {
        svcSleepThread(500000000LL);
    } else if (mode >= 2) {
        gfxExit();
    }
}
