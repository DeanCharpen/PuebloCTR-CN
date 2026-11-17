/* The port (stage 3.6, patch 0160): the PS vector functions without a square
 * root, inline. Each is the SDK's C_* body (src/lib/vec.c) statement for
 * statement, so the same results; the call (PSVECAdd -> runtime/mtx_ps.c ->
 * C_VECAdd) cost two branches and the caller's floating-point registers around
 * it, at ~3000 call sites of the game. Included at the end of include/vec.h and
 * include/dolphin/mtx.h; runtime/mtx_ps.c defines RE4_SDK_PS_DEFINITIONS (it
 * defines the PS* functions themselves). */
#ifndef RE4_PSVEC_INLINE_H
#define RE4_PSVEC_INLINE_H
#ifndef RE4_SDK_PS_DEFINITIONS

static inline void re4_PSVECAdd(const Vec* a, const Vec* b, Vec* ab) {
    ab->x = a->x + b->x;
    ab->y = a->y + b->y;
    ab->z = a->z + b->z;
}
static inline void re4_PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b) {
    a_b->x = a->x - b->x;
    a_b->y = a->y - b->y;
    a_b->z = a->z - b->z;
}
static inline void re4_PSVECScale(const Vec* src, Vec* dst, f32 scale) {
    dst->x = (src->x * scale);
    dst->y = (src->y * scale);
    dst->z = (src->z * scale);
}
static inline f32 re4_PSVECSquareMag(const Vec* v) {
    return v->z * v->z + ((v->x * v->x) + (v->y * v->y));
}
static inline f32 re4_PSVECDotProduct(const Vec* a, const Vec* b) {
    return (a->z * b->z) + ((a->x * b->x) + (a->y * b->y));
}
static inline void re4_PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) {
    f32 x = (a->y * b->z) - (a->z * b->y);
    f32 y = (a->z * b->x) - (a->x * b->z);
    f32 z = (a->x * b->y) - (a->y * b->x);
    axb->x = x;
    axb->y = y;
    axb->z = z;
}
static inline f32 re4_PSVECSquareDistance(const Vec* a, const Vec* b) {
    f32 x = a->x - b->x;
    f32 y = a->y - b->y;
    f32 z = a->z - b->z;
    return (z * z) + ((x * x) + (y * y));
}

#define PSVECAdd            re4_PSVECAdd
#define PSVECSubtract       re4_PSVECSubtract
#define PSVECScale          re4_PSVECScale
#define PSVECSquareMag      re4_PSVECSquareMag
#define PSVECDotProduct     re4_PSVECDotProduct
#define PSVECCrossProduct   re4_PSVECCrossProduct
#define PSVECSquareDistance re4_PSVECSquareDistance

#endif
#endif
