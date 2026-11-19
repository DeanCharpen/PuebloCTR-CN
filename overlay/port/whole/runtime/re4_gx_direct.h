/* The 3DS renderer (port/whole/3ds/gx_3ds.cpp, stage 3.3a of the 3DS port) takes the draws as the
 * GX decoder (runtime/gx_decode.cpp) meets them: no chunks (re4_gx_frame.h), whose vertices of a
 * frame (~9 MB in r100) do not fit the 3DS's heap. The decoder converts each draw's vertices
 * into memory the renderer gives (the layout of re4_gx_frame.h) and hands the draw over with the
 * register state it keeps. All on the decoder's thread. 3DS only. */
#ifndef RE4_GX_DIRECT_H
#define RE4_GX_DIRECT_H

#include "re4_gx_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Re4GxDirect {
    /* room for the vertices (bytes) and indices of one draw; NULL: the draw is left out */
    void* (*stage)(uint32_t vertexBytes, uint32_t indices, uint16_t** indexOut);
    /* the draw (offsets 0: the vertices and indices given), with the decoder's BP registers, XF
     * memory (0x000-0x7FF) and XF registers (0x1000-0x1057) */
    void (*draw)(const Re4GxDraw* d, const void* vertices, const uint16_t* indices, const uint32_t* bp,
                 const uint32_t* xfMem, const uint32_t* xfReg);
    /* XF changed at `address` (memory or 0x1000 + register), `count` words: `words` (host order; the renderer keeps
     * its own copy of the state, 2026-11-19) */
    void (*xf)(uint32_t address, uint32_t count, const uint32_t* words);
    /* the owner tag of the draws that follow (re4_gx_frame.h RE4_GX_OP_OWNER) */
    void (*owner)(const uint32_t owner[4]);
    /* a display copy: the frame is complete */
    void (*frame)(uint32_t xfb, uint32_t width, uint32_t height);
    /* texture map t->map changed for the draws that follow: its texels at `data` (t->dataBytes, all
     * levels, GX tiled, big-endian; NULL: an EFB copy or not in memory), for the CI formats the
     * palette (`tlutEntries` big-endian entries as TMEM holds them) */
    void (*texture)(const Re4GxTexture* t, const uint8_t* data, const uint8_t* tlut, uint32_t tlutEntries);
    /* BP register reg now holds value (written with a new value; the TEV colour registers
     * 0xE0-0xE7 at every write, the registers and konstants sharing their addresses) */
    void (*bp)(uint32_t reg, uint32_t value);
    /* lasting room for the display list cache (stage 3.3b): vertexBytes and indices that stay
     * until drop(vertices), which frees them once the GPU is past them; NULL: none. A draw from
     * there carries RE4_GX_DIRECT_KEPT in its primitive. keep NULL: no cache. */
    void* (*keep)(uint32_t vertexBytes, uint32_t indices, uint16_t** indexOut);
    void (*drop)(void* vertices);
    /* a draw whose vertices come in two parts (the display list cache's dynamic lists, stage 3.8): `vertices`
     * the position and the packed normal (12 or 16 bytes a vertex), `rest` the rest of each vertex (the
     * draw's stride less those), `box` the positions' box (min x y z, max x y z); NULL: none */
    void (*drawSplit)(const Re4GxDraw* d, const void* vertices, const void* rest, const float* box,
                      const uint16_t* indices, const uint32_t* bp, const uint32_t* xfMem, const uint32_t* xfReg);
    /* the room of keep written (stage 5.7b): what the draws and drop name it by from now on, the indices at the
     * same offset in it (the renderer may move it: to VRAM, its room freed once copied; the decoder reads it no
     * more); NULL: the room itself */
    void* (*kept)(void* room);
    /* (stage 41) linear memory of the decoder's that draws of the picture now (or the one before) read: given back
     * through re4_gx_head_back once the GPU is past them; NULL: none */
    void (*later)(void* p);
} Re4GxDirect;

#define RE4_GX_DIRECT_KEPT (1u << 24)

void re4_gx_set_direct(const Re4GxDirect* direct);
/* The display list cache's line for the log window (counts since the last line) */
void re4_gx_kept_report(void);
/* (stage 41) The heads of the split dynamic lists by the skinning jobs (gx_decode.cpp). re4_gx_head_on: 0 off, 1 on,
 * 2 each head checked (gx_thread_3ds.cpp, RE4_3DS_HEADJOB). re4_gx_head_job: a model's positions (posN of 6 bytes at
 * pos, from src) and normals (nrmN at nrm; nrmKind 1: 3 bytes each) are skinned, by `side` (0 the GX thread, 1 the
 * game), in the jobs' generation gen. re4_gx_head_announce: on the GX thread, the draws from here read the arrays
 * of that generation. re4_gx_head_back: the renderer, a piece of `later` the GPU is past. */
extern int re4_gx_head_on;
void re4_gx_head_job(int side, unsigned gen, const void* src, const void* pos, unsigned posN, const void* nrm,
                     unsigned nrmN, int nrmKind);
void re4_gx_head_announce(unsigned gen);
void re4_gx_head_back(void* p);

#ifdef __cplusplus
}
#endif

#endif
