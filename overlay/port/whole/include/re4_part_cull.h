/* The culling of models and their parts outside the view before the game sets them up
 * (runtime/part_cull.cpp, patch 0162 in game/trans.cpp commonModelTrans; stage 3.6 of the 3DS port). */
#ifndef RE4_PART_CULL_H
#define RE4_PART_CULL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 0: the game need not ask (RE4_PARTCULL=0); set by the first call. */
extern int re4_part_cull_on;

/* A model info drawn from its file's own positions: `parts` its first part (cPolyHeader, then the primitive
 * stream; `count` parts in a row), vertex indices big-endian, `vertexBytes` a vertex (position, normal,
 * [colour,] texture coordinate, 16-bit indices), the positions s16 x/y/z/pad in host order (`positions`,
 * `vertices` of them, `shift` fraction bits), through the position matrix `mv` and the projection `proj`
 * (GXGetProjectionv). 0: draw every part; 1: wholly outside the view, leave it out; 2: ask re4_part_out for
 * each part. */
int re4_info_cull(const void* parts, unsigned count, const short* positions, unsigned vertices, unsigned shift,
                  unsigned vertexBytes, const float mv[3][4], const float proj[7]);
/* After re4_info_cull gave 2: 1 when part `i` of that info is wholly outside the view. */
int re4_part_out(unsigned i);

#ifdef __cplusplus
}
#endif

/* game/trans.cpp commonModelTrans (patch 0162): an info of `d` drawn through `mv` */
#if defined(__cplusplus) && defined(MODEL_H)
extern "C" void GXGetProjectionv(float* ptr);
static inline int re4InfoCulled(cModelData* d, float mv[3][4])
{
    float proj[7];

    if (!re4_part_cull_on) {
        return 0;
    }
    GXGetProjectionv(proj);
    return re4_info_cull(d->pParts, d->displist_num, (const short*) d->vtxOrig, d->nVtx, d->shift,
                         (d->flags & 0x80000000) ? 8 : 6, mv, proj);
}
#endif

#endif
