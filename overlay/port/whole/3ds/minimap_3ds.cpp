// Stage 8.3 (2026-11-08, the second screen; the project owner's wish): the area's map on the bottom screen while the
// game runs, by the CPU (the GPU is the frame's limit): drawn by the panel's thread (settings_3ds.cpp drawGame) into
// its picture of the bottom screen (copied into the framebuffer at once).
//
// What the game's thread hands over (runtime/cheats.cpp mmSample, the rules of the map screen copied from
// src/Sscrn/ss_map.cpp: runtime/minimap_game.h): the area (getAreaNo) and its file (SS/cmn/map_objNN.dat), its
// rooms (room number, sub-file) with their state (mapColor: 0 the current room, 1 visited, 2 open, 3 cleared,
// 4 not shown), Leon's room, position and a point 1000 ahead, the marks to show (the goal model's part, the
// merchant model's parts, the typewriters); under a sequence number (odd while written).
//
// The file (read here once per area, from the SD card in pieces; ss_map.cpp: SS_ARC_PTR, mapBinAddr, mapHitAddr,
// markGoalInit...):
// - a room's sub-file: its map models, then two floor collisions: 0 the map's (hitA, local to the room's model,
//   placed on the map by its parts 0: + ModelDataHead.center; r10e x10) and 1 the room's own (hitB: the same vertices
//   at the room's size). Leon on the map as mapPositionCheck finds him: hitB's floor / slope triangle under him, his
//   (s, t) in it, the same vertex indices in hitA's vertices.
// - the rooms' shapes: the map models' triangles (cModelData: s16 positions at vtxOrig, 8 bytes each, frac `shift`;
//   the parts' GX display lists after their 0x20-byte headers, indices of 16 bits: position, normal, colour when
//   flags bit 31, texture; trans.cpp commonModelTrans) + the part's centre; a model not read: hitA's floors instead.
// - the marks: archive entries 6 (goal), 7 (merchant), 10 (typewriter), when the area has them (mark_model_tbl): their
//   parts' centres are the positions (markGoalPosition...).
// Drawn: the rooms' triangles (the lower floors first) into a buffer of room numbers, then by colour per state with an
// outline where the room changes (or ends) - kept as a picture until the room or a state changes; the marks (the goal a
// red disc, the typewriters "S", the merchant "$", 8.3d: the treasures gold diamonds - hollow grey once taken, the blue
// medallions blue discs while unbroken, their count in the village); Leon as an arrow. Nothing allocated while drawing
// (a low thread holding malloc's lock stalled the game: frames of 200-300 ms, 2026-11-08).
// The floors (8.3d, mapModelInit): a room of several map models is one of several floors, a model's floor its part 0's
// height / 100, Leon's his height on the map / 100; in the current room the models of Leon's floor (the nearest to his)
// are drawn last in the current room's colour, the others dimmer, "этаж N из M" in the corner.
// The zoom (8.3e, the owner's wish: only by its buttons - a column at the box's right: "+", "-", "to Leon"): the area
// whole, the current room whole, x2 and x4 of that (these follow Leon: the centre moved onto him when he nears the
// edge). Scrolled by the finger (re4_3ds_minimap_touch on the game's thread): while dragged the kept picture is shifted
// (cheap), let go - made again around the new centre; the scroll holds until "to Leon" or another room.
#include <3ds.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "re4_runtime.h"
#include "ui_assets.h"  // (stage 9) the text's ids (UI_*), drawn by 3ds/ui_bottom_3ds.cpp

extern "C" {
// runtime/cheats.cpp (8.3)
extern volatile int re4_mm_area, re4_mm_count, re4_mm_room, re4_mm_goal, re4_mm_merch[8], re4_mm_save, re4_mm_doors;
extern volatile signed char re4_mm_door_part[64];
extern volatile unsigned char re4_mm_door_ang[64], re4_mm_door_state[64];
extern const char* volatile re4_mm_file;
extern volatile unsigned short re4_mm_rooms[48], re4_mm_subs[48];
extern volatile unsigned char re4_mm_color[48];
extern volatile float re4_mm_pos[3], re4_mm_fwd[3];
extern volatile unsigned re4_mm_seq;
extern volatile int re4_mm_tre_shown, re4_mm_coin_shown;  // (8.3d)
extern volatile unsigned re4_mm_tre_left, re4_mm_coin_left;
// (stage 9) 3ds/ui_bottom_3ds.cpp: the screen's text in its language
void re4_ui_clip(int x0, int y0, int x1, int y1);
int re4_ui_text(unsigned short* fb, int id, int x, int base, int align, unsigned short c, int shadow);
int re4_ui_tpl(unsigned short* fb, int tpl, int a0, int a1, int x, int base, int align, unsigned short c, int shadow);
}

namespace {

struct RoomMap {
    u16 room;
    std::vector<float> mapV;   // hitA's vertices on the map (x y z)
    std::vector<float> floor;  // hitB's floors and slopes in the room: 9 floats a triangle
    std::vector<u16> floorIdx; // their vertex indices (3 a triangle): the same vertices of hitA place Leon
    float min[2], max[2];      // the shape's box on the map (x, z)
    std::vector<s8> floors;    // (8.3d) its models' floors, from the lowest (one: not a room of floors)
};

const s8 kNoFloor = 127;  // (a shape from the collision: no model, no floor)

// the shapes' triangles of the whole area, the lower first: room index, then x z of the corners
struct ShapeTri {
    float y;
    u16 room;
    s8 floor;  // (8.3d) its model's floor (kNoFloor)
    float x[3], z[3];
};

std::vector<RoomMap> gRooms;
std::vector<ShapeTri> gShape;
std::vector<float> gGoal, gMerch, gSave, gDoor;  // the marks' and the doors' models: x z of each part
std::vector<float> gTreasure, gCoin;             // (8.3d) the treasures', the medallions'
int gArea = -2;  // the area loaded (its rooms empty: the file missing or unreadable)
float gAreaMin[2], gAreaMax[2];

struct Snap {
    int area, count, room, goal, merch[8], save, doors;
    int treShown, coinShown;  // (8.3d)
    unsigned treLeft, coinLeft;
    s8 doorPart[64];
    u8 doorAng[64], doorState[64];
    const char* file;
    u16 rooms[48], subs[48];
    u8 color[48];
    float pos[3], fwd[3];
};
unsigned gSeqDrawn = ~0u;
u64 gDrawnAt;
float gLeon[2], gDir[2] = {0.0f, -1.0f};
float gLeonY;  // (8.3d) his height on the map (the floor)
bool gLeonKnown;
// (8.3e) the zoom: 0 the area, 1 the room, 2 x2, 3 x4 (its buttons: re4_3ds_minimap_touch on the game's thread)
volatile int gZoomReq = -1;
int gZoomDrawn = -1;
float gNear[2];  // the following view's centre
int gNearRoom = -1;
// (8.3e) the scroll: the finger's pixels not yet taken (the game's thread adds), dragging now, "to Leon" asked
volatile int gDragPx[2], gDragging, gCenterReq, gPressedBtn = -1;
int gCenterDone, gDrawnDrag[2], gDrawnPressed = -1;
bool gPanned;     // the centre is the finger's, not the zoom's own
float gPan[2];    // that centre (on the map)
int gPanRoom = -1;
volatile int gBoxX, gBoxY, gBoxW, gBoxH;  // the box last drawn (0 wide: no map, no touches taken)
bool gShotDue;  // (the stand: RE4_3DS_MAP_SHOT)
const int kBtnW = 30;  // the buttons' column at the box's right
u8 gIds[320 * 240];  // the room (index + 1) at each pixel of the box, 0 none
// the rooms' picture of the box (columns of gH, from the top), made again only when its key changes (the area, the room,
// the states, the box): a redraw copies it and adds the marks and Leon (all of it each time: ~48 ms on the console)
u16 gMapImg[320 * 240];
u32 gImgKey = ~0u;
float gViewSc, gViewOx, gViewOy;
u32 gDraws;
double gDrawMs, gDrawMax;

u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

u16 be16(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}

float bef(const u8* p)
{
    const u32 v = be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

bool snap(Snap& s)
{
    for (int tries = 0; tries < 4; tries++) {
        const unsigned a = re4_mm_seq;
        __sync_synchronize();
        if (a & 1) {
            svcSleepThread(1000000LL);
            continue;
        }
        s.area = re4_mm_area;
        s.count = re4_mm_count < 0 ? 0 : re4_mm_count > 48 ? 48 : re4_mm_count;
        s.room = re4_mm_room;
        s.file = re4_mm_file;
        s.goal = re4_mm_goal;
        s.save = re4_mm_save;
        s.treShown = re4_mm_tre_shown;
        s.coinShown = re4_mm_coin_shown;
        s.treLeft = re4_mm_tre_left;
        s.coinLeft = re4_mm_coin_left;
        for (int k = 0; k < 8; k++) {
            s.merch[k] = re4_mm_merch[k];
        }
        s.doors = re4_mm_doors < 0 ? 0 : re4_mm_doors > 64 ? 64 : re4_mm_doors;
        for (int k = 0; k < s.doors; k++) {
            s.doorPart[k] = re4_mm_door_part[k];
            s.doorAng[k] = re4_mm_door_ang[k];
            s.doorState[k] = re4_mm_door_state[k];
        }
        for (int i = 0; i < s.count; i++) {
            s.rooms[i] = re4_mm_rooms[i];
            s.subs[i] = re4_mm_subs[i];
            s.color[i] = re4_mm_color[i];
        }
        for (int k = 0; k < 3; k++) {
            s.pos[k] = re4_mm_pos[k];
            s.fwd[k] = re4_mm_fwd[k];
        }
        __sync_synchronize();
        if (re4_mm_seq == a) {
            return true;
        }
    }
    return false;
}

// The file whole, in pieces of 64 KB (the SD card's service takes one request at a time: the game's reads between)
bool readFile(const char* name, std::vector<u8>& out)
{
    // as runtime/dvd.cpp: <data>/disc0/files/..., or a folder with files/ directly inside
    const char* dir = getenv("RE4_DATA_DIR");
    char path[256];
    snprintf(path, sizeof(path), "%s/disc0/files/%s", dir ? dir : "sdmc:/3ds/pueblo/data", name);
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "%s/files/%s", dir ? dir : "sdmc:/3ds/pueblo/data", name);
        f = fopen(path, "rb");
    }
    if (!f) {
        re4_log("3ds: the minimap: %s not found (nor below disc0/)", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t) size : 0);
    size_t got = 0;
    while (got < out.size()) {
        const size_t n = fread(out.data() + got, 1, std::min<size_t>(64 << 10, out.size() - got), f);
        if (n == 0) {
            break;
        }
        got += n;
        svcSleepThread(2000000LL);
    }
    fclose(f);
    return got == out.size();
}

// A floor collision (cSatHeader at `hdr`): its first SAT's vertices, (v + off) * scale, and its floors' and slopes'
// vertex indices (3 a triangle)
bool readSat(const std::vector<u8>& d, u32 hdr, const float off[3], float scale, std::vector<float>& vtxOut,
             std::vector<u16>& idxOut)
{
    if (hdr + 8 > d.size() || !(d[hdr] & 0x80)) {
        return false;
    }
    const u32 f = hdr + be32(&d[hdr + 4]);
    if (f + 0x14 > d.size()) {
        return false;
    }
    const u8* p = &d[f];
    const u32 nv = be16(p + 2), nn = be16(p + 4), ne = be16(p + 6), np = be16(p + 0xA);
    const u32 nUsed = std::min<u32>(np, (u32) be16(p + 0xC) + be16(p + 0xE));
    const u32 vtx = f + 0x14, poly = vtx + 12 * (nv + nn + ne);
    if (poly + 0x14 * np > d.size()) {
        return false;
    }
    for (u32 k = 0; k < nv; k++) {
        for (int a = 0; a < 3; a++) {
            vtxOut.push_back((bef(&d[vtx + 12 * k + 4 * a]) + off[a]) * scale);
        }
    }
    for (u32 i = 0; i < nUsed; i++) {
        for (int c = 0; c < 3; c++) {
            const u32 k = be16(&d[poly + 0x14 * i + 2 * c]);
            if (k >= nv) {
                return false;
            }
            idxOut.push_back((u16) k);
        }
    }
    return true;
}

// A model (cModelData at `m`): the centre of the part it hangs on (`centre`, may be null) and its triangles on the map
// ((v + centre) * scale; may be null): false when it is not one we read (skinned, broken)
bool readModel(const std::vector<u8>& d, u32 m, float scale, float* centreOut, std::vector<ShapeTri>* out, u16 room)
{
    if (m + 0x40 > d.size()) {
        return false;
    }
    const u32 head = m + be32(&d[m]);
    const u32 nParts = d[m + 0x19], nDl = be16(&d[m + 0x1A]);
    const u32 parts = m + be32(&d[m + 0x1C]), flags = be32(&d[m + 0x20]), shift = d[m + 0x28];
    const u32 vtx = m + be32(&d[m + 0x30]), nVtx = be16(&d[m + 0x38]);
    if (nParts == 0 || head + 16 * nParts > d.size()) {
        return false;
    }
    const u32 partsNo = d[head] < nParts ? d[head] : 0;
    float centre[3];
    for (int a = 0; a < 3; a++) {
        centre[a] = bef(&d[head + 16 * partsNo + 4 + 4 * a]);
    }
    if (centreOut) {
        memcpy(centreOut, centre, sizeof(centre));
    }
    if (!out) {
        return true;
    }
    // rigid models only (trans.cpp: one part, no weight palette: the positions as they are)
    if (nParts != 1 || d[m + 0x18] > 1 || be16(&d[m + 0x2A]) > 0xFF || shift > 15 || vtx + 8 * nVtx > d.size()) {
        return false;
    }
    const float k = 1.0f / (float) (1 << shift);
    const u32 stride = 2 * ((flags & 0x80000000u) ? 4 : 3);  // position, normal, (colour), texture
    auto corner = [&](u32 at, float* xyz) {
        const u32 i = be16(&d[at]);
        if (i >= nVtx) {
            return false;
        }
        for (int a = 0; a < 3; a++) {
            xyz[a] = ((float) (s16) be16(&d[vtx + 8 * i + 2 * a]) * k + centre[a]) * scale;
        }
        return true;
    };
    const size_t before = out->size();
    const s8 floor = (s8) (centre[1] * scale / 100.0f + 0.5f);  // (mapModelInit: parts 0's mat[1][3] / 100 + 0.5)
    auto tri = [&](u32 a, u32 b, u32 c) {
        float p[3][3];
        if (!corner(a, p[0]) || !corner(b, p[1]) || !corner(c, p[2])) {
            return false;
        }
        out->push_back({p[0][1] + p[1][1] + p[2][1], room, floor, {p[0][0], p[1][0], p[2][0]},
                        {p[0][2], p[1][2], p[2][2]}});
        return true;
    };
    u32 p = parts;
    for (u32 dl = 0; dl < nDl; dl++) {
        if (p + 0x20 > d.size()) {
            break;
        }
        const u32 size = be32(&d[p + 0x18]);
        u32 s = p + 0x20, e = s + size;
        if (e > d.size()) {
            break;
        }
        while (s < e) {
            const u8 op = d[s];
            if (op == 0) {
                s++;
                continue;
            }
            if (op < 0x80 || op > 0xBF || s + 3 > e) {
                break;  // (not a draw: the rest of this list unread)
            }
            const u32 prim = op & 0xF8, n = be16(&d[s + 1]);
            s += 3;
            if (s + n * stride > e) {
                break;
            }
            bool ok = true;
            if (prim == 0x90) {
                for (u32 i = 0; i + 2 < n && ok; i += 3) {
                    ok = tri(s + i * stride, s + (i + 1) * stride, s + (i + 2) * stride);
                }
            } else if (prim == 0x98) {
                for (u32 i = 2; i < n && ok; i++) {
                    ok = tri(s + (i - 2) * stride, s + (i - 1) * stride, s + i * stride);
                }
            } else if (prim == 0xA0) {
                for (u32 i = 2; i < n && ok; i++) {
                    ok = tri(s, s + (i - 1) * stride, s + i * stride);
                }
            } else if (prim == 0x80) {
                for (u32 i = 0; i + 3 < n && ok; i += 4) {
                    ok = tri(s + i * stride, s + (i + 1) * stride, s + (i + 2) * stride) &&
                         tri(s + i * stride, s + (i + 2) * stride, s + (i + 3) * stride);
                }
            }
            if (!ok) {
                out->resize(before);
                return false;
            }
            s += n * stride;
        }
        p = e;
    }
    return out->size() > before;
}

// A mark model of the archive (entry `no`): its parts' centres (x z), the parents' added
void readMarks(const std::vector<u8>& d, u32 no, std::vector<float>& out)
{
    out.clear();
    if (4 * no + 4 > d.size()) {
        return;
    }
    const u32 m = be32(&d[4 * no]);
    if (m + 0x40 > d.size()) {
        return;
    }
    const u32 head = m + be32(&d[m]), nParts = d[m + 0x19];
    if (head + 16 * nParts > d.size()) {
        return;
    }
    for (u32 i = 0; i < nParts; i++) {
        float x = 0.0f, z = 0.0f;
        u32 k = i;
        for (int depth = 0; depth < 8 && k < nParts; depth++) {
            x += bef(&d[head + 16 * k + 4]);
            z += bef(&d[head + 16 * k + 12]);
            k = d[head + 16 * k + 1];  // the parent (0xFF: the model)
        }
        out.push_back(x);
        out.push_back(z);
    }
}

void load(const Snap& s)
{
    gArea = s.area;
    gRooms.clear();
    gShape.clear();
    gGoal.clear();
    gMerch.clear();
    gSave.clear();
    gDoor.clear();
    gTreasure.clear();
    gCoin.clear();
    gLeonKnown = false;
    std::vector<u8> d;
    if (!s.file || !readFile(s.file, d)) {
        return;  // (gArea kept: not tried again until the area changes; "no map")
    }
    gAreaMin[0] = gAreaMin[1] = 1e30f;
    gAreaMax[0] = gAreaMax[1] = -1e30f;
    u32 fromModels = 0, fromHits = 0;
    for (int i = 0; i < s.count; i++) {
        const u32 subAt = 4u * s.subs[i];
        if (subAt + 4 > d.size()) {
            continue;
        }
        const u32 sub = be32(&d[subAt]);
        if (sub + 0x20 > d.size()) {
            continue;
        }
        const u32 models = be32(&d[sub]) - 2;
        if (models > 64 || sub + 4 * (models + 6) > d.size()) {
            continue;
        }
        RoomMap r;
        r.room = s.rooms[i];
        const u16 index = (u16) gRooms.size();
        const float scale = r.room == 0x10E ? 10.0f : 1.0f;  // (ss_map.cpp mapModelInit: r10e's model at x10)
        float centre[3] = {0.0f, 0.0f, 0.0f};
        readModel(d, sub + be32(&d[sub + 16]), scale, centre, nullptr, index);
        // (mapPositionCheck: the room's floor / slope triangle under Leon, its vertex indices into the map's vertices)
        std::vector<u16> mapIdx;
        std::vector<float> floorV;
        if (!readSat(d, sub + be32(&d[sub + 4 * (models + 4)]), centre, scale, r.mapV, mapIdx) || mapIdx.empty()) {
            re4_log("3ds: the minimap: r%03x's map collision unreadable", (unsigned) r.room);
            continue;
        }
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        if (!readSat(d, sub + be32(&d[sub + 4 * (models + 5)]), zero, 1.0f, floorV, r.floorIdx) ||
            floorV.size() != r.mapV.size()) {
            re4_log("3ds: the minimap: r%03x's room collision unreadable or unlike the map's", (unsigned) r.room);
            r.floorIdx.clear();
        } else {
            for (u16 k : r.floorIdx) {
                r.floor.insert(r.floor.end(), {floorV[k * 3], floorV[k * 3 + 1], floorV[k * 3 + 2]});
            }
        }
        // the shape: the room's map models, else hitA's floors
        const size_t before = gShape.size();
        for (u32 k = 0; k < models; k++) {
            readModel(d, sub + be32(&d[sub + 4 * (4 + k)]), scale, nullptr, &gShape, index);
        }
        if (gShape.size() > before) {
            fromModels++;
        } else {
            fromHits++;
            for (size_t t = 0; t + 2 < mapIdx.size(); t += 3) {
                const float* a = &r.mapV[mapIdx[t] * 3];
                const float* b = &r.mapV[mapIdx[t + 1] * 3];
                const float* c = &r.mapV[mapIdx[t + 2] * 3];
                gShape.push_back({a[1] + b[1] + c[1], index, kNoFloor, {a[0], b[0], c[0]}, {a[2], b[2], c[2]}});
            }
        }
        r.min[0] = r.min[1] = 1e30f;
        r.max[0] = r.max[1] = -1e30f;
        for (size_t t = before; t < gShape.size(); t++) {
            const s8 fl = gShape[t].floor;
            if (fl != kNoFloor && std::find(r.floors.begin(), r.floors.end(), fl) == r.floors.end()) {
                r.floors.push_back(fl);
            }
            for (int c = 0; c < 3; c++) {
                r.min[0] = std::min(r.min[0], gShape[t].x[c]);
                r.max[0] = std::max(r.max[0], gShape[t].x[c]);
                r.min[1] = std::min(r.min[1], gShape[t].z[c]);
                r.max[1] = std::max(r.max[1], gShape[t].z[c]);
            }
        }
        std::sort(r.floors.begin(), r.floors.end());
        for (int a = 0; a < 2; a++) {
            gAreaMin[a] = std::min(gAreaMin[a], r.min[a]);
            gAreaMax[a] = std::max(gAreaMax[a], r.max[a]);
        }
        gRooms.push_back(std::move(r));
    }
    std::stable_sort(gShape.begin(), gShape.end(), [](const ShapeTri& a, const ShapeTri& b) { return a.y < b.y; });
    readMarks(d, 6, gGoal);
    readMarks(d, 7, gMerch);
    readMarks(d, 10, gSave);
    readMarks(d, 4, gDoor);  // (doorModelInit: the doors hang on this model's parts)
    if (s.area >= 0 && s.area < 18) {  // (8.3d: markTreasureInit, markCoinInit - the area's own models, else none)
        static const u8 kHas[18][2] = {{0, 0}, {1, 1}, {1, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}, {0, 0}, {0, 0},
                                       {1, 1}, {0, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {0, 0}, {0, 0}, {0, 0}};
        if (kHas[s.area][0]) {
            readMarks(d, 8, gTreasure);
        }
        if (kHas[s.area][1]) {
            readMarks(d, 9, gCoin);
        }
    }
    char floors[160];  // (8.3d) the rooms of floors
    int at = 0;
    floors[0] = 0;
    for (const auto& r : gRooms) {
        if (r.floors.size() > 1 && at < (int) sizeof(floors) - 24) {
            at += snprintf(floors + at, sizeof(floors) - at, " r%03x:%d", (unsigned) r.room, (int) r.floors.size());
        }
    }
    re4_log("3ds: the minimap: area %d (%s): %u rooms (%u by their models, %u by the collision), %u triangles, marks "
            "%u/%u/%u, treasures %u, medallions %u, %u KB read; floors:%s",
            gArea, s.file, (unsigned) gRooms.size(), fromModels, fromHits, (unsigned) gShape.size(),
            (unsigned) gGoal.size() / 2, (unsigned) gMerch.size() / 2, (unsigned) gSave.size() / 2,
            (unsigned) gTreasure.size() / 2, (unsigned) gCoin.size() / 2, (unsigned) (d.size() >> 10),
            floors[0] ? floors : " none");
}

// Leon on the map (mapPositionCheck): the room's floor triangle under him, the same vertices of the map
void placeLeon(const Snap& s)
{
    const RoomMap* r = nullptr;
    for (const auto& x : gRooms) {
        if (x.room == s.room) {
            r = &x;
        }
    }
    if (!r || r->floor.empty()) {
        gLeonKnown = false;  // (a room not on the area's map: no arrow)
        return;
    }
    int best = -1, near = -1;
    float bestY = -1e30f, bestS = 0, bestT = 0, nearY = 1e30f, nearS = 0, nearT = 0;
    const size_t n = r->floor.size() / 9;
    for (size_t t = 0; t < n; t++) {
        const float* v = &r->floor[t * 9];
        const float ax = v[3] - v[0], az = v[5] - v[2], bx = v[6] - v[0], bz = v[8] - v[2];
        const float det = ax * bz - az * bx;
        if (fabsf(det) < 1e-6f) {
            continue;
        }
        const float dx = s.pos[0] - v[0], dz = s.pos[2] - v[2];
        const float ss = (dx * bz - dz * bx) / det, tt = (ax * dz - az * dx) / det;
        if (ss < -0.02f || tt < -0.02f || ss + tt > 1.02f) {
            continue;
        }
        const float y = v[1] + ss * (v[4] - v[1]) + tt * (v[7] - v[1]);
        if (y <= s.pos[1] + 150.0f && y > bestY) {
            best = (int) t, bestY = y, bestS = ss, bestT = tt;
        }
        if (fabsf(y - s.pos[1]) < nearY) {
            near = (int) t, nearY = fabsf(y - s.pos[1]), nearS = ss, nearT = tt;
        }
    }
    if (best < 0) {
        best = near, bestS = nearS, bestT = nearT;
    }
    if (best < 0) {
        return;
    }
    const float* f = &r->floor[best * 9];
    float m[9];  // the same triangle on the map: hitA's vertices by the room triangle's indices
    for (int c = 0; c < 3; c++) {
        const u16 k = r->floorIdx[best * 3 + c];
        m[c * 3] = r->mapV[k * 3];
        m[c * 3 + 1] = r->mapV[k * 3 + 1];
        m[c * 3 + 2] = r->mapV[k * 3 + 2];
    }
    auto onMap = [&](float ss, float tt, float* out) {
        out[0] = m[0] + ss * (m[3] - m[0]) + tt * (m[6] - m[0]);
        out[1] = m[2] + ss * (m[5] - m[2]) + tt * (m[8] - m[2]);
    };
    onMap(bestS, bestT, gLeon);
    gLeonY = m[1] + bestS * (m[4] - m[1]) + bestT * (m[7] - m[1]);
    const float ax = f[3] - f[0], az = f[5] - f[2], bx = f[6] - f[0], bz = f[8] - f[2];
    const float det = ax * bz - az * bx;
    const float dx = s.fwd[0] - f[0], dz = s.fwd[2] - f[2];
    float ahead[2];
    onMap((dx * bz - dz * bx) / det, (ax * dz - az * dx) / det, ahead);
    const float ux = ahead[0] - gLeon[0], uz = ahead[1] - gLeon[1], len = sqrtf(ux * ux + uz * uz);
    if (len > 1e-3f) {
        gDir[0] = ux / len;
        gDir[1] = uz / len;
    }
    gLeonKnown = true;
}

// --- drawing ---------------------------------------------------------------------------------------------------------

u16* gFb;
int gX0, gY0, gW, gH;

inline void px(int x, int y, u16 c)
{
    if (x >= gX0 && x < gX0 + gW && y >= gY0 && y < gY0 + gH) {
        gFb[x * 240 + 239 - y] = c;
    }
}

// a triangle into the box: `ids` (the room numbers, box-relative, columns of gH) or the picture's colour
void fillTri(float x0, float y0, float x1, float y1, float x2, float y2, u16 c, u8 id, bool toIds)
{
    if (y1 < y0) {
        std::swap(x0, x1), std::swap(y0, y1);
    }
    if (y2 < y0) {
        std::swap(x0, x2), std::swap(y0, y2);
    }
    if (y2 < y1) {
        std::swap(x1, x2), std::swap(y1, y2);
    }
    const int ya = std::max((int) ceilf(y0 - 0.5f), gY0), yb = std::min((int) ceilf(y2 - 0.5f), gY0 + gH);
    for (int y = ya; y < yb; y++) {
        const float fy = y + 0.5f;
        const float xa = y2 != y0 ? x0 + (x2 - x0) * (fy - y0) / (y2 - y0) : x0;
        float xb;
        if (fy < y1) {
            xb = y1 != y0 ? x0 + (x1 - x0) * (fy - y0) / (y1 - y0) : x1;
        } else {
            xb = y2 != y1 ? x1 + (x2 - x1) * (fy - y1) / (y2 - y1) : x1;
        }
        const int l = std::max((int) ceilf(std::min(xa, xb) - 0.5f), gX0);
        const int rr = std::min((int) ceilf(std::max(xa, xb) - 0.5f), gX0 + gW);
        if (toIds) {
            u8* p = gIds + (l - gX0) * gH + (y - gY0);
            for (int x = l; x < rr; x++, p += gH) {
                *p = id;
            }
        } else {
            u16* col = gFb + l * 240 + 239 - y;
            for (int x = l; x < rr; x++, col += 240) {
                *col = c;
            }
        }
    }
}

void disc(float cx, float cy, float r, u16 c)
{
    for (int y = (int) (cy - r); y <= (int) (cy + r) + 1; y++) {
        for (int x = (int) (cx - r); x <= (int) (cx + r) + 1; x++) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r * r) {
                px(x, y, c);
            }
        }
    }
}

// a mark's letter (stage 9: UI_BADGE_S / UI_BADGE_D of the screen's face) centred at (cx, cy) in a dark box with a rim
void badge(float cx, float cy, int id, u16 letter, u16 rim)
{
    const int x0 = (int) cx - 5, y0 = (int) cy - 5;
    for (int y = 0; y < 11; y++) {
        for (int x = 0; x < 11; x++) {
            px(x0 + x, y0 + y, (x == 0 || y == 0 || x == 10 || y == 10) ? rim : 0x0000);
        }
    }
    re4_ui_text(gFb, id, x0 + 6, y0 + 9, 1, letter, 0);
}

// (8.3d) a diamond of `r` (|dx| + |dy| <= r): filled, or its rim only
void diamond(float cx, float cy, float r, u16 c, bool hollow)
{
    for (int y = (int) (cy - r) - 1; y <= (int) (cy + r) + 1; y++) {
        for (int x = (int) (cx - r) - 1; x <= (int) (cx + r) + 1; x++) {
            const float d = fabsf(x + 0.5f - cx) + fabsf(y + 0.5f - cy);
            if (d <= r && (!hollow || d > r - 1.6f)) {
                px(x, y, c);
            }
        }
    }
}

// the state's colours: 0 the current room, 1 visited, 2 open (not entered), 3 cleared, (4 not shown), 5 (8.3d) another
// floor of the current room
const u16 kFill[6] = {0x2D9C, 0x1A6F, 0x5104, 0x3186, 0x0000, 0x11EA};
const u16 kEdge[6] = {0xBFFF, 0x6CBF, 0xC28A, 0x9CF3, 0x0000, 0x5CB6};
const char* const kZoomName[4] = {"the area", "the room", "x2", "x4"};  // (the log's)
const int kZoomId[4] = {UI_AREA, UI_ROOM, UI_X2, UI_X4};
// (stage 9) the labels' colours: bone, dim, warm, the medallions' blue
const u16 kLabelBone = 0xD678, kLabelDim = 0x8BCE, kLabelWarm = 0xACB0, kLabelBlue = 0x7D1B;

}  // namespace

// (8.3e) the zoom asked for; the first: RE4_3DS_MAP_ZOOM (0 the area, 1 the room - the default, 2 x2, 3 x4)
static int zoomNow()
{
    if (gZoomReq < 0) {
        const char* v = getenv("RE4_3DS_MAP_ZOOM");
        gZoomReq = v && *v >= '0' && *v <= '3' ? *v - '0' : 1;
    }
    return gZoomReq;
}

// (8.3e) The touch screen each game frame (settings_3ds.cpp re4_3ds_hud_pad, the game's thread; phase 1 a new touch, 0
// held, -1 none): a touch begun in the map scrolls it, on its buttons zooms / goes back to Leon. 1 while it is the map's
extern "C" int re4_3ds_minimap_touch(int phase, int x, int y)
{
    static int mode, lx, ly;  // (mode: 0 none, 1 dragging, 2 on a button)
    if (phase < 0) {
        gDragging = 0;
        gPressedBtn = -1;
        mode = 0;
        return 0;
    }
    if (phase == 1) {
        mode = 0;
        const int bx = gBoxX, by = gBoxY, bw = gBoxW, bh = gBoxH;
        if (bw <= kBtnW || x < bx || x >= bx + bw || y < by || y >= by + bh) {
            return 0;
        }
        if (x >= bx + bw - kBtnW) {
            const int k = std::min(2, (y - by) * 3 / bh);
            const int z = zoomNow();
            if (k == 0) {
                gZoomReq = std::min(3, z + 1);
            } else if (k == 1) {
                gZoomReq = std::max(0, z - 1);
            } else {
                gCenterReq = gCenterReq + 1;
            }
            gPressedBtn = k;
            mode = 2;
        } else {
            lx = x, ly = y;
            gDragging = 1;
            mode = 1;
        }
        return 1;
    }
    if (mode == 1) {
        __atomic_add_fetch(&gDragPx[0], x - lx, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gDragPx[1], y - ly, __ATOMIC_RELAXED);
        lx = x, ly = y;
    }
    return mode != 0;
}

// (the stand) RE4_3DS_MAP_SHOT's picture due: the caller writes the whole screen (once)
extern "C" int re4_3ds_minimap_shot_due(void)
{
    const bool due = gShotDue;
    gShotDue = false;
    return due ? 1 : 0;
}

// The panel's thread: the minimap wants drawing (Leon moved, the state changed: at most every 100 ms; the zoom, the
// scroll, a button: at once)
extern "C" int re4_3ds_minimap_dirty(void)
{
    if (zoomNow() != gZoomDrawn || gCenterReq != gCenterDone || gPressedBtn != gDrawnPressed ||
        gDragPx[0] != gDrawnDrag[0] || gDragPx[1] != gDrawnDrag[1] ||
        (!gDragging && (gDragPx[0] || gDragPx[1]))) {
        return 1;
    }
    const unsigned seq = re4_mm_seq;
    if (seq == gSeqDrawn || (seq & 1)) {
        return 0;
    }
    return svcGetSystemTick() - gDrawnAt >= 100ull * CPU_TICKS_PER_MSEC ? 1 : 0;
}

// The minimap into the box (x, y, w, h) of the bottom screen's picture `fb` (RGB565, its columns of 240 from the bottom
// row up); 0 when there is no map (the caller says so)
extern "C" int re4_3ds_minimap_draw(unsigned short* fb, int x, int y, int w, int h)
{
    const u64 from = svcGetSystemTick();
    gSeqDrawn = re4_mm_seq;
    gDrawnAt = from;
    // (taken before any way out: else "dirty" again every 50 ms while there is no map)
    const int zoom = zoomNow();
    if (zoom != gZoomDrawn) {
        re4_log("3ds: the minimap: the zoom %d (%s)", zoom, kZoomName[zoom]);
        gZoomDrawn = zoom;
    }
    gDrawnPressed = gPressedBtn;
    const int dragX = gDragPx[0], dragY = gDragPx[1];
    const bool dragging = gDragging;
    gDrawnDrag[0] = dragX, gDrawnDrag[1] = dragY;
    const int centerReq = gCenterReq;
    gBoxW = 0;
    Snap s;
    if (!snap(s) || s.area < 0) {
        return 0;
    }
    if (s.area != gArea) {
        load(s);
    }
    if (gRooms.empty() || w > 320 || h > 240 || w <= kBtnW + 40) {
        return 0;
    }
    gBoxX = x, gBoxY = y, gBoxH = h, gBoxW = w;
    const int fullW = w;
    w -= kBtnW + 2;  // (8.3e) the map's part; the buttons right of it
    placeLeon(s);
    gFb = fb;
    gX0 = x, gY0 = y, gW = w, gH = h;
    // the rooms' state by index; the current room
    u8 state[64];
    int curIdx = -1;
    for (size_t i = 0; i < gRooms.size() && i < 64; i++) {
        state[i] = 4;
        for (int k = 0; k < s.count; k++) {
            if (s.rooms[k] == gRooms[i].room) {
                state[i] = s.color[k];
            }
        }
        if (gRooms[i].room == s.room) {
            curIdx = (int) i;
        }
    }
    const RoomMap* cur = curIdx >= 0 ? &gRooms[curIdx] : nullptr;
    // (8.3d) Leon's floor (mapModelInit: his height on the map / 100, rounded away from 0), the nearest of the room's
    int floorAt = -1;
    if (cur && cur->floors.size() > 1 && gLeonKnown) {
        const float fy = gLeonY / 100.0f;
        const int leonFloor = fy > 0.0f ? (int) (fy + 0.5f) : (int) (fy - 0.5f);
        for (size_t k = 0; k < cur->floors.size(); k++) {
            if (floorAt < 0 || abs(cur->floors[k] - leonFloor) < abs(cur->floors[floorAt] - leonFloor)) {
                floorAt = (int) k;
            }
        }
    }
    const int curFloor = floorAt >= 0 ? cur->floors[floorAt] : kNoFloor;
    // the zoom (8.3e): the area whole; the current room whole (else the area); x2, x4 of that, following Leon
    const float areaW = std::max(gAreaMax[0] - gAreaMin[0], 1.0f), areaH = std::max(gAreaMax[1] - gAreaMin[1], 1.0f);
    const float sArea = std::min(w / (areaW * 1.05f), h / (areaH * 1.05f));
    float sc = sArea, cx = (gAreaMin[0] + gAreaMax[0]) * 0.5f, cz = (gAreaMin[1] + gAreaMax[1]) * 0.5f;
    if (cur && zoom >= 1) {
        const float rw = std::max(cur->max[0] - cur->min[0], 1.0f), rh = std::max(cur->max[1] - cur->min[1], 1.0f);
        sc = std::max(sArea, std::min(w / (rw * 1.25f), h / (rh * 1.25f)));
        cx = (cur->min[0] + cur->max[0]) * 0.5f;
        cz = (cur->min[1] + cur->max[1]) * 0.5f;
    }
    if (zoom >= 2) {
        sc *= zoom == 2 ? 2.0f : 4.0f;
        if (cur && gLeonKnown) {  // (the centre onto Leon when he leaves the middle: the picture made again only then)
            if (gNearRoom != s.room || fabsf(gLeon[0] - gNear[0]) * sc > w * 0.3f ||
                fabsf(gLeon[1] - gNear[1]) * sc > h * 0.3f) {
                gNear[0] = gLeon[0], gNear[1] = gLeon[1];
                gNearRoom = s.room;
            }
            cx = gNear[0], cz = gNear[1];
        }
    }
    // (8.3e) the scroll: "to Leon" or another room drop it; the finger let go - its pixels into the centre
    if (centerReq != gCenterDone || gPanRoom != s.room) {
        gCenterDone = centerReq;
        gPanned = false;
        gPanRoom = s.room;
    }
    if (!dragging && (dragX || dragY)) {
        const float k = 1.0f / std::max(gViewSc, 1e-6f);  // (the picture's scale: the one the finger moved)
        if (!gPanned) {
            gPan[0] = cx, gPan[1] = cz;
        }
        gPan[0] -= dragX * k, gPan[1] -= dragY * k;
        gPanned = true;
        __atomic_sub_fetch(&gDragPx[0], dragX, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&gDragPx[1], dragY, __ATOMIC_RELAXED);
        gDrawnDrag[0] = gDrawnDrag[1] = 0;
    }
    if (gPanned) {
        gPan[0] = std::min(std::max(gPan[0], gAreaMin[0]), gAreaMax[0]);
        gPan[1] = std::min(std::max(gPan[1], gAreaMin[1]), gAreaMax[1]);
        cx = gPan[0], cz = gPan[1];
    }
    u32 key = 2166136261u;  // the picture's key (FNV-1a): area, room, states, box, view, floor
    auto mix = [&](u32 v) { key = (key ^ v) * 16777619u; };
    auto mixf = [&](float f) {
        u32 v;
        memcpy(&v, &f, 4);
        mix(v);
    };
    mix((u32) gArea), mix((u32) s.room), mix((u32) (w << 16 | h)), mix((u32) (x << 16 | y));
    mix((u32) zoom), mix((u32) curFloor), mixf(sc), mixf(cx), mixf(cz);
    for (size_t i = 0; i < gRooms.size() && i < 64; i++) {
        mix(state[i]);
    }
    if (key != gImgKey) {
        gImgKey = key;
        gViewSc = sc;
        gViewOx = x + w * 0.5f - cx * sc;
        gViewOy = y + h * 0.5f - cz * sc;
        // the room numbers (the lower floors first; 8.3d: Leon's floor of the current room last, its other floors as
        // index + 65), then the colours with the outlines
        memset(gIds, 0, (size_t) w * h);
        for (int pass = 0; pass < 2; pass++) {
            for (const ShapeTri& t : gShape) {
                if (t.room >= 64 || state[t.room] > 3) {
                    continue;
                }
                const bool mine = (int) t.room == curIdx && curFloor != kNoFloor && t.floor != kNoFloor;
                if ((pass == 1) != (mine && t.floor == curFloor)) {
                    continue;
                }
                const u8 id = (u8) (t.room + 1 + (mine && t.floor != curFloor ? 64 : 0));
                fillTri(gViewOx + t.x[0] * sc, gViewOy + t.z[0] * sc, gViewOx + t.x[1] * sc, gViewOy + t.z[1] * sc,
                        gViewOx + t.x[2] * sc, gViewOy + t.z[2] * sc, 0, id, true);
            }
        }
        for (int i = 0; i < w; i++) {
            const u8* col = gIds + i * h;
            u16* out = gMapImg + i * h;
            for (int j = 0; j < h; j++) {
                const u8 id = col[j];
                if (!id) {
                    out[j] = 0x0000;
                    continue;
                }
                const bool edge = (j > 0 && col[j - 1] != id) || (j < h - 1 && col[j + 1] != id) ||
                                  (i > 0 && col[j - h] != id) || (i < w - 1 && col[j + h] != id);
                const u8 st = id > 64 ? 5 : state[id - 1];
                out[j] = edge ? kEdge[st] : kFill[st];
            }
        }
    }
    sc = gViewSc;  // (the picture's: the key holds it)
    // (8.3e) while the finger drags: the kept picture shifted by its pixels, the marks with it
    const int sx = dragging ? dragX : 0, sy = dragging ? dragY : 0;
    const float ox = gViewOx + sx, oy = gViewOy + sy;
    for (int i = 0; i < w; i++) {  // the rooms' picture (its columns run down, the framebuffer's up)
        u16* out = fb + (x + i) * 240 + 239 - y;
        const int si = i - sx;
        if (si < 0 || si >= w) {
            for (int j = 0; j < h; j++) {
                *out-- = 0x0000;
            }
            continue;
        }
        const u16* in = gMapImg + si * h;
        for (int j = 0; j < h; j++) {
            const int sj = j - sy;
            *out-- = sj >= 0 && sj < h ? in[sj] : 0x0000;
        }
    }
    // the doors (doorModelDisp; the owner: "locked doors are red on the game's map"): a short bar across each, turned by
    // its angle about y; green open, red locked, orange locked with the key carried
    for (int k = 0; k < s.doors; k++) {
        const int part = s.doorPart[k], st = s.doorState[k];
        if (st < 1 || st > 3 || part < 0 || (size_t) part * 2 + 1 >= gDoor.size()) {
            continue;
        }
        const float a = s.doorAng[k] * 3.1415927f / 180.0f, ux = cosf(a), uy = -sinf(a);
        const float mx = ox + gDoor[part * 2] * sc, my = oy + gDoor[part * 2 + 1] * sc;
        const u16 c = st == 1 ? 0x2FE5 : st == 2 ? 0xF800 : 0xFC80;
        for (float t = -4.5f; t <= 4.5f; t += 0.5f) {
            for (int o = -1; o <= 1; o++) {
                px((int) (mx + ux * t - uy * o), (int) (my + uy * t + ux * o), c);
            }
        }
    }
    re4_ui_clip(x, y, x + w, y + h);  // (stage 9: the marks' letters within the map too)
    // (8.3d) the treasures (ss_map.cpp markTreasureDisp: 14 at most, the taken ones marked): gold diamonds, hollow grey
    // once taken; the blue medallions (markCoinDisp: 15 at most, the broken ones not shown): blue discs
    if (s.treShown) {
        for (size_t k = 0; k < 14 && k * 2 + 1 < gTreasure.size(); k++) {
            const float mx = ox + gTreasure[k * 2] * sc, my = oy + gTreasure[k * 2 + 1] * sc;
            if (s.treLeft >> k & 1) {
                diamond(mx, my, 6.0f, 0x0000, false);
                diamond(mx, my, 4.5f, 0xFEA0, false);
            } else {
                diamond(mx, my, 4.5f, 0x8410, true);
            }
        }
    }
    int coins = 0, coinsLeft = 0;
    if (s.coinShown) {
        for (size_t k = 0; k < 15 && k * 2 + 1 < gCoin.size(); k++) {
            coins++;
            if (!(s.coinLeft >> k & 1)) {
                continue;
            }
            coinsLeft++;
            const float mx = ox + gCoin[k * 2] * sc, my = oy + gCoin[k * 2 + 1] * sc;
            disc(mx, my, 4.5f, 0xCE7F);
            disc(mx, my, 3.5f, 0x033F);
        }
    }
    // the marks (the owner's choice: blue is the side quest's medallions): the typewriters "S", the merchant "$",
    // the goal a red disc
    if (s.save) {
        for (size_t k = 0; k + 1 < gSave.size(); k += 2) {
            badge(ox + gSave[k] * sc, oy + gSave[k + 1] * sc, UI_BADGE_S, 0xFFFF, 0xA514);
        }
    }
    for (int k = 0; k < 8; k++) {
        const int part = s.merch[k];
        if (part >= 0 && (size_t) part * 2 + 1 < gMerch.size()) {
            badge(ox + gMerch[part * 2] * sc, oy + gMerch[part * 2 + 1] * sc, UI_BADGE_D, 0x47E8, 0x2DA5);
        }
    }
    if (s.goal >= 0 && (size_t) s.goal * 2 + 1 < gGoal.size()) {
        const float mx = ox + gGoal[s.goal * 2] * sc, my = oy + gGoal[s.goal * 2 + 1] * sc;
        disc(mx, my, 6.0f, 0x0000);
        disc(mx, my, 5.0f, 0xF800);
    }
    bool onMap = false;
    for (const auto& r : gRooms) {
        onMap = onMap || r.room == s.room;
    }
    // the labels (stage 9: the screen's face, in its language)
    re4_ui_clip(x, y, x + w, y + h);
    if (!onMap) {  // (the game's map: the whole area, no mark of Leon)
        re4_ui_text(fb, UI_OFFMAP, x + 6, y + 13, 0, kLabelWarm, 1);
    } else if (floorAt >= 0) {  // (8.3d)
        re4_ui_tpl(fb, UI_T_FLOOR, floorAt + 1, (int) cur->floors.size(), x + 6, y + 13, 0, kLabelBone, 1);
    }
    re4_ui_text(fb, kZoomId[zoom], x + w - 6, y + 12, 2, kLabelDim, 1);  // (8.3e) the zoom
    if (s.coinShown && s.area == 1 && coins) {  // (markCoinDisp: the count in the village - broken / all)
        re4_ui_tpl(fb, UI_T_MEDALS, coins - coinsLeft, coins, x + 6, y + h - 6, 0, kLabelBlue, 1);
    }
    re4_ui_clip(0, 0, 320, 240);
    // Leon: an arrow (tip ahead, two corners behind), dark rim then yellow
    if (gLeonKnown && onMap) {
        const float lx = ox + gLeon[0] * sc, ly = oy + gLeon[1] * sc;
        const float dx = gDir[0], dy = gDir[1], nx = -dy, ny = dx;
        for (int pass = 0; pass < 2; pass++) {
            const float k = pass == 0 ? 9.0f : 7.0f;
            fillTri(lx + dx * k, ly + dy * k, lx - dx * k * 0.7f + nx * k * 0.65f, ly - dy * k * 0.7f + ny * k * 0.65f,
                    lx - dx * k * 0.7f - nx * k * 0.65f, ly - dy * k * 0.7f - ny * k * 0.65f,
                    pass == 0 ? 0x0000 : 0xFFE0, 0, false);
        }
    }
    // (8.3e) the buttons' column: "+", "-", "to Leon" (an arrow; lit while scrolled away); lit while pressed, dim at the
    // zoom's ends
    gW = fullW;
    {
        const int bx = x + fullW - kBtnW, bh = (h - 4) / 3;
        for (int k = 0; k < 3; k++) {
            const int by = y + k * (bh + 2);
            const bool lit = gDrawnPressed == k;
            const bool off = (k == 0 && zoom == 3) || (k == 1 && zoom == 0);
            // (stage 9, "the dark": a dark slab with a blood-red rim, lit - redder)
            const u16 slab = lit ? 0x28E3 : 0x1082, rim = lit ? 0xA8A2 : 0x48A2;
            for (int i = 0; i < kBtnW; i++) {
                for (int j = 0; j < bh; j++) {
                    const bool edge = i == 0 || j == 0 || i == kBtnW - 1 || j == bh - 1;
                    px(bx + i, by + j, edge ? rim : slab);
                }
            }
            const u16 c = off ? 0x4208 : lit ? 0xF75C : 0xD678;
            const int mx = bx + kBtnW / 2, my = by + bh / 2;
            if (k <= 1) {
                for (int i = -6; i <= 6; i++) {
                    for (int t = -1; t <= 0; t++) {  // (stage 9: thinner)
                        px(mx + i, my + t, c);
                        if (k == 0) {
                            px(mx + t, my + i, c);
                        }
                    }
                }
            } else {
                const u16 a = gPanned ? 0xFFE0 : c;
                fillTri(mx, my - 8, mx - 6, my + 6, mx + 6, my + 6, a, 0, false);
                fillTri(mx, my + 2, mx - 3, my + 7, mx + 3, my + 7, slab, 0, false);
            }
        }
    }
    gW = w;
    // (8.3d, the stand) RE4_3DS_MAP_SHOT=ROOM:N (hex room): the Nth drawing in that room -> sdmc:/3ds/pueblo/hud.fb (the
    // bottom screen's picture so far, the format of RE4_3DS_HUD_SHOT)
    static int shotRoom = -2, shotLeft;
    if (shotRoom == -2) {
        const char* v = getenv("RE4_3DS_MAP_SHOT");
        shotRoom = v ? (int) strtol(v, nullptr, 16) : -1;
        shotLeft = v && strchr(v, ':') ? atoi(strchr(v, ':') + 1) : 1;
    }
    if (shotRoom >= 0 && s.room == shotRoom && --shotLeft == 0) {
        gShotDue = true;  // (written when the whole screen is drawn: re4_3ds_minimap_shot_due, ui_bottom_3ds.cpp)
        re4_log("3ds: the minimap: the picture of r%03x to hud.fb", (unsigned) s.room);
    }
    // the cost, into the log every 300 drawings
    const double ms = (double) (svcGetSystemTick() - from) / CPU_TICKS_PER_MSEC;
    gDrawMs += ms;
    gDrawMax = std::max(gDrawMax, ms);
    if (++gDraws % 300 == 0) {
        re4_log("3ds: the minimap: %u drawings, %.2f ms each, at most %.2f ms (the panel's thread)", gDraws,
                gDrawMs / 300.0, gDrawMax);
        gDrawMs = gDrawMax = 0.0;
    }
    return 1;
}
