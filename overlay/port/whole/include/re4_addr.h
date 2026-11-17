/* "An address, not a file offset" (RE4_IS_ADDR in the game, relocated() in runtime/formats).
 * The game relocates its files in place and tells a relocated pointer from an offset by bit 31
 * (patch 0151). The offsets are small (310 KB at most over every room); the addresses are MEM1,
 * the heaps and the windows above RE4_MEM1_BASE, and the game's own static data (DmyZeroTpl,
 * scroll.cpp, passes calcTplAddr). Everything at or above the base counts, not only MEM1: some
 * files carry the fill 0xCDCDCDCD where a table would go (motion data), and the GameCube leaves
 * those alone as addresses too. Below the base only the static data counts; it lies there on the
 * 3DS (the program image from 0x00100000), re4_static_range_init (runtime/mem1.cpp) sets it. */
#ifndef RE4_ADDR_H
#define RE4_ADDR_H
#ifndef RE4_MEM1_BASE
#define RE4_MEM1_BASE 0x80000000u
#endif
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned int re4_static_lo, re4_static_size;
#ifdef __cplusplus
}
#endif
static inline int re4_is_addr(unsigned int v)
{
    return v >= (unsigned int) RE4_MEM1_BASE || v - re4_static_lo < re4_static_size;
}
#endif
