// The culling of models and their parts outside the view before the game sets them up (stage 3.6 of the
// 3DS port, patch 0162 in game/trans.cpp commonModelTrans). The game culls whole models by a sphere when it
// puts them in the ordering tables (trans_ot.cpp AddOt*Radius); every info of a model in view, and every
// part of it, is then set up (matrices, textures, shaderSetup, the GX library's calls), and the renderer
// left a third of the village's draws out only after that (gx_3ds.cpp outsideView). Here, for an info drawn
// from its model file's own positions (no skinning): the box of all its parts through the position matrix
// and the projection wholly beyond one clip plane (GX's depth in [-w, 0]: beyond the far plane z > 0, the
// near one z < -w; the test of outsideView) leaves the info out; wholly inside all of them draws every part;
// else each part goes by its own box. The boxes come from the parts' primitive streams (the positions'
// indices; big-endian, as on the disc: formats_model.cpp keeps the streams), made once a model and kept by
// the address of its first part. RE4_PARTCULL=N: from frame N (0 never); the 3DS from frame 1, elsewhere
// never.
#include "../include/re4_part_cull.h"
#include "re4_runtime.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp

extern "C" {
int re4_part_cull_on = 1;
}

namespace {

struct Box {
    float c[3], h[3];  // centre, half size
    bool ok;           // false: a stream this does not read (never left out)
};

struct Model {
    uintptr_t parts;  // 0: free
    uintptr_t positions;
    uint32_t count, vertices, head;  // head: the first part's size word (the model reloaded elsewhere)
    uint16_t shift, vertexBytes;
    Box* boxes;  // [count] the parts, [count] all of them
};

const uint32_t kModels = 1024;  // a power of 2; emptied when three quarters full
Model* gModels;
uint32_t gUsed;
bool gInit;
unsigned gFrom, gFrame;
const Box* gParts;  // the info re4_info_cull gave 2 for
float gRow[6][4], gAbs[6][3];  // the clip planes' rows over (x, y, z, 1) (beyond: > 0), their magnitudes
uint32_t gInfos, gInfosOut, gInfosIn, gPartsAsked, gPartsOut, gMade, gUnread, gCleared;

inline uint32_t be16(const uint8_t* p)
{
    return (uint32_t) p[0] << 8 | p[1];
}

inline uint32_t sizeOf(const uint8_t* part)
{
    uint32_t size;
    memcpy(&size, part + 0x18, 4);  // cPolyHeader buff_size (host order)
    return size;
}

inline uint32_t bitsOf(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

// The box of the positions the stream's primitives index (VAT 0; NOPs skipped); false on any other command or
// an index out of the array
bool makeBox(Box& b, int lo[3], int hi[3], const uint8_t* p, uint32_t size, const int16_t* positions,
             uint32_t vertices, uint32_t stride)
{
    const uint8_t* end = p + size;
    lo[0] = lo[1] = lo[2] = 32767;
    hi[0] = hi[1] = hi[2] = -32768;
    bool any = false;
    while (p < end) {
        const uint32_t op = *p++;
        if (op == 0) {
            continue;
        }
        if ((op & 0x87) != 0x80 || op > 0xB8 || end - p < 2) {
            return false;
        }
        const uint32_t n = be16(p);
        p += 2;
        if ((uint32_t) (end - p) < n * stride) {
            return false;
        }
        for (uint32_t k = 0; k < n; k++, p += stride) {
            const uint32_t i = be16(p);
            if (i >= vertices) {
                return false;
            }
            const int16_t* v = positions + i * 4;
            for (int c = 0; c < 3; c++) {
                lo[c] = v[c] < lo[c] ? v[c] : lo[c];
                hi[c] = v[c] > hi[c] ? v[c] : hi[c];
            }
            any = true;
        }
    }
    return any;
}

void setBox(Box& b, const int lo[3], const int hi[3], float s)
{
    for (int c = 0; c < 3; c++) {
        b.c[c] = (float) (lo[c] + hi[c]) * 0.5f * s;
        b.h[c] = (float) (hi[c] - lo[c]) * 0.5f * s;
    }
}

void makeModel(Model& m, const uint8_t* parts)
{
    m.boxes = (Box*) malloc((m.count + 1) * sizeof(Box));
    if (!m.boxes) {
        return;
    }
    const float s = 1.0f / (float) (1u << m.shift);
    int all0[3] = {32767, 32767, 32767}, all1[3] = {-32768, -32768, -32768};
    bool allOk = true;
    const uint8_t* part = parts;
    for (uint32_t i = 0; i < m.count; i++) {
        const uint32_t size = sizeOf(part);
        int lo[3], hi[3];
        Box& b = m.boxes[i];
        b.ok = makeBox(b, lo, hi, part + 0x20, size, (const int16_t*) m.positions, m.vertices, m.vertexBytes);
        if (b.ok) {
            setBox(b, lo, hi, s);
            for (int c = 0; c < 3; c++) {
                all0[c] = lo[c] < all0[c] ? lo[c] : all0[c];
                all1[c] = hi[c] > all1[c] ? hi[c] : all1[c];
            }
        } else {
            allOk = false;
            gUnread++;
        }
        part += 0x20 + size;
    }
    Box& all = m.boxes[m.count];
    all.ok = allOk && m.count;
    if (all.ok) {
        setBox(all, all0, all1, s);
    }
    gMade++;
}

void emptyModels()
{
    for (uint32_t i = 0; i < kModels; i++) {
        free(gModels[i].boxes);
    }
    memset(gModels, 0, kModels * sizeof(Model));
    gUsed = 0;
    gCleared++;
}

const Model* modelOf(const uint8_t* parts, uint32_t count, const int16_t* positions, uint32_t vertices,
                     uint32_t shift, uint32_t vertexBytes)
{
    const uint32_t head = sizeOf(parts);
    uint32_t at = (uint32_t) ((uintptr_t) parts >> 5) * 2654435761u >> 22 & (kModels - 1);
    for (;; at = (at + 1) & (kModels - 1)) {
        Model& m = gModels[at];
        if (!m.parts) {
            break;
        }
        if (m.parts == (uintptr_t) parts) {
            if (m.positions != (uintptr_t) positions || m.count != count || m.vertices != vertices ||
                m.head != head || m.shift != shift || m.vertexBytes != vertexBytes) {
                free(m.boxes);  // (another model at the same address)
                m.positions = (uintptr_t) positions;
                m.count = count;
                m.vertices = vertices;
                m.head = head;
                m.shift = (uint16_t) shift;
                m.vertexBytes = (uint16_t) vertexBytes;
                makeModel(m, parts);
            }
            return m.boxes ? &m : nullptr;
        }
    }
    if (gUsed >= kModels / 4 * 3) {
        emptyModels();
        return modelOf(parts, count, positions, vertices, shift, vertexBytes);
    }
    Model& m = gModels[at];
    m.parts = (uintptr_t) parts;
    m.positions = (uintptr_t) positions;
    m.count = count;
    m.vertices = vertices;
    m.head = head;
    m.shift = (uint16_t) shift;
    m.vertexBytes = (uint16_t) vertexBytes;
    gUsed++;
    makeModel(m, parts);
    return m.boxes ? &m : nullptr;
}

// 1 wholly beyond one plane, 0 wholly inside all of them, 2 across; a plane's row r has all 8 corners beyond
// it when r(centre) - |r|(half size) > 0, none when r(centre) + |r|(half size) < 0 (the signs by the bits: no
// VFP compare; NaN neither)
int classify(const Box& b)
{
    uint32_t inside = 1;
    for (int i = 0; i < 6; i++) {
        const float* r = gRow[i];
        const float* a = gAbs[i];
        const float d = r[0] * b.c[0] + r[1] * b.c[1] + r[2] * b.c[2] + r[3];
        const float e = a[0] * b.h[0] + a[1] * b.h[1] + a[2] * b.h[2];
        if (bitsOf(d - e) - 1 < 0x7F800000u) {  // > 0 (+inf too)
            return 1;
        }
        inside &= bitsOf(d + e) - 0x80000001u < 0x7F800000u;  // < 0 (-inf too)
    }
    return inside ? 0 : 2;
}

#if defined(__3DS__)
// (occlusion cull) 3ds/gx_3ds.cpp, the end
extern "C" void re4_occ_frame(unsigned f);
extern "C" int re4_occ_info(const float mv[3][4], const float c[3], const float h[3], unsigned parts);
extern "C" int re4_occ_on;
#endif

void frame(unsigned f)
{
    gFrame = f;
#if defined(__3DS__)
    re4_occ_frame(f);
#endif
    if (f && f % 300 == 0 && gInfos) {
        const double n = 300.0;
        re4_log("part cull (per frame, 300 frames; from frame %u): %.1f infos asked, %.1f left out, %.1f wholly in "
                "view; %.1f parts asked, %.1f left out; %.1f models made, %lu parts not read; "
                "%lu models kept (emptied %lu times)",
                gFrom, gInfos / n, gInfosOut / n, gInfosIn / n, gPartsAsked / n, gPartsOut / n, gMade / n,
                (unsigned long) gUnread, (unsigned long) gUsed, (unsigned long) gCleared);
        gInfos = gInfosOut = gInfosIn = gPartsAsked = gPartsOut = gMade = gUnread = 0;
    }
}

void init()
{
    gInit = true;
#if defined(__3DS__)
    gFrom = 1;
#endif
    if (const char* e = getenv("RE4_PARTCULL")) {
        gFrom = (unsigned) atoi(e);
    }
    if (gFrom) {
        gModels = (Model*) calloc(kModels, sizeof(Model));
    }
    if (!gModels) {
        gFrom = 0;
    }
    re4_part_cull_on = gFrom != 0;
    re4_log("part cull: the infos and parts outside the view left out %s%u", gFrom ? "from frame " : "never", gFrom);
    if (gFrom) {
        re4_add_frame_hook(frame);
    }
}

}  // namespace

extern "C" int re4_info_cull(const void* parts, unsigned count, const short* positions, unsigned vertices,
                             unsigned shift, unsigned vertexBytes, const float mv[3][4], const float proj[7])
{
    if (!gInit) {
        init();
    }
    if (!gFrom || gFrame < gFrom || !parts || !positions || !count) {
        return 0;
    }
    gInfos++;
    int result = 0;
    if (const Model* m = modelOf((const uint8_t*) parts, count, (const int16_t*) positions, vertices, shift,
                                 vertexBytes)) {
        // the clip coordinates as rows over (x, y, z, 1), then the planes x > w, x < -w, y > w, y < -w, z > 0,
        // z < -w as rows beyond which (> 0) a point is outside
        const float* p = proj + 1;
        float X[4], Y[4], Z[4], Wr[4];
        if (proj[0] == 0.0f) {
            for (int k = 0; k < 4; k++) {
                X[k] = p[0] * mv[0][k] + p[1] * mv[2][k];
                Y[k] = p[2] * mv[1][k] + p[3] * mv[2][k];
                Z[k] = p[4] * mv[2][k];
                Wr[k] = -mv[2][k];
            }
        } else {
            for (int k = 0; k < 4; k++) {
                X[k] = p[0] * mv[0][k];
                Y[k] = p[2] * mv[1][k];
                Z[k] = p[4] * mv[2][k];
                Wr[k] = 0.0f;
            }
            X[3] += p[1];
            Y[3] += p[3];
            Wr[3] = 1.0f;
        }
        Z[3] += p[5];
        for (int k = 0; k < 4; k++) {
            gRow[0][k] = X[k] - Wr[k];
            gRow[1][k] = -X[k] - Wr[k];
            gRow[2][k] = Y[k] - Wr[k];
            gRow[3][k] = -Y[k] - Wr[k];
            gRow[4][k] = Z[k];
            gRow[5][k] = -Z[k] - Wr[k];
        }
        for (int i = 0; i < 6; i++) {
            for (int k = 0; k < 3; k++) {
                gAbs[i][k] = __builtin_fabsf(gRow[i][k]);
            }
        }
        const Box& all = m->boxes[m->count];
        result = all.ok ? classify(all) : 2;
#if defined(__3DS__)
        if (re4_occ_on && result != 1 && all.ok && re4_occ_info(mv, all.c, all.h, count)) {
            result = 1;  // (occlusion cull) hidden behind the room in an older picture's depth
        }
#endif
        gInfosOut += result == 1;
        gInfosIn += result == 0;
        gParts = m->boxes;
    } else {
        gParts = nullptr;
    }
    return result;
}

// RE4_DRAWDIST=P (1..100, percent): the fog's start and end and the far plane (game/light.cpp, patch 0321) times
// P/100 when the fog is on - the distance drawn, faded into the fog the same way; 100 (default): as it was, bit for bit
// (6.6: the 3DS panel changes it while the game runs, re4_draw_dist_set)
static volatile float gDrawScale = -1.0f;

extern "C" float re4_draw_dist(void)
{
    if (gDrawScale < 0.0f) {
        const char* e = getenv("RE4_DRAWDIST");
        const int p = e ? atoi(e) : 100;
        gDrawScale = p > 0 && p <= 100 ? (float) p / 100.0f : 1.0f;
        re4_log("draw distance: %d%% of the game's (RE4_DRAWDIST)", (int) (gDrawScale * 100.0f + 0.5f));
    }
    return gDrawScale;
}

extern "C" void re4_draw_dist_set(int percent)
{
    gDrawScale = percent > 0 && percent <= 100 ? (float) percent / 100.0f : 1.0f;
    re4_log("draw distance: %d%% of the game's (set while running)", (int) (gDrawScale * 100.0f + 0.5f));
}

extern "C" int re4_part_out(unsigned i)
{
    if (!gParts) {
        return 0;
    }
    gPartsAsked++;
    const Box& b = gParts[i];
    const int out = b.ok && classify(b) == 1;
    gPartsOut += out;
    return out;
}

// (occlusion cull, models: patch 0338, game/trans_ot.cpp AddOtModelPosRadius) 1: the model's sphere is behind the
// room in an older picture's depth - left out like one outside the view; the 3DS's in 3ds/gx_3ds.cpp, elsewhere 0
#if defined(__3DS__)
extern "C" int re4_occ_sphere3(const float* pos, float radius, const float (*view)[4]);
#endif
extern "C" int re4_occ_sphere(const float* pos, float radius, const float (*view)[4])
{
#if defined(__3DS__)
    return re4_occ_on ? re4_occ_sphere3(pos, radius, view) : 0;
#else
    (void) pos;
    (void) radius;
    (void) view;
    return 0;
#endif
}
