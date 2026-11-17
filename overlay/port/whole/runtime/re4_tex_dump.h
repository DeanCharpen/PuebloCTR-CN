/* GX textures as the game hands them over, one record each, for the pack of PICA textures made ahead of time
 * (stage 5.7a, port/whole/3ds/tools/texpak.cpp): level 0 of the texels (GX tiles, big-endian) and the palette as
 * TMEM holds it, by the decoder's content key (gx_decode.cpp bindMap). Written by the decoder (RE4_TEXDUMP=DIR,
 * Linux: DIR/tex-<pid>.dump, each key once a run) and by the 3DS renderer for the textures not in the pack
 * (gx_3ds.cpp, RE4_3DS_TEXMISS). */
#ifndef RE4_TEX_DUMP_H
#define RE4_TEX_DUMP_H

#include <stdint.h>
#include <stdio.h>

#define RE4_TEX_DUMP_MAGIC 0x32585452u /* "RTX2" */

typedef struct Re4TexDumpHead {
    uint32_t magic;
    uint32_t keyLo, keyHi;
    uint32_t address;                           /* where the game keeps it (the same address and size with many
                                                   keys: a picture the game writes, a movie's, not for the pack) */
    uint16_t format, width, height, tlutFormat; /* GX format, size; the palette's format (BP TX_SETTLUT >> 10 & 3) */
    uint32_t dataBytes;                         /* the texels that follow */
    uint32_t entries;                           /* then the palette: entries * 2 bytes; then up to 4 bytes */
} Re4TexDumpHead;

static inline int re4_tex_dump_write(FILE* f, const Re4TexDumpHead* head, const uint8_t* data,
                                     const uint8_t* palette)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    const uint32_t tail = (head->dataBytes + head->entries * 2) & 3;
    return fwrite(head, sizeof(*head), 1, f) == 1 && fwrite(data, 1, head->dataBytes, f) == head->dataBytes &&
           fwrite(palette, 2, head->entries, f) == head->entries &&
           (!tail || fwrite(zero, 1, 4 - tail, f) == 4 - tail);
}

#endif
