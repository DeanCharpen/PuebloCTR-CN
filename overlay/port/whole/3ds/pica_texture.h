// GX textures as the PICA's (stage 5.7a, docs 09-stage3-gx.md of the port research): what gx_3ds.cpp makes of each
// texture, without citro3d, so that a tool on the host (tools/texpak.cpp) makes the same ahead of time for a pack on
// the SD card (textures.pak). Level 0 decoded (gx_texture.cpp), the PICA format and size chosen, scaled to it, the
// mipmaps, the PICA's tiles (ETC1 / ETC1A4 blocks); all levels one after the other, level 0 first, as
// C3D_TexGetImagePtr lays them out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace re4pica {

enum Pack : unsigned { kRgb565, kRgba4, kLa4, kLa8, kEtc1, kEtc1a4, kL8, kPacks };
extern const unsigned kPackBits[kPacks];
extern const char* const kPackName[kPacks];

// larger ones are scaled down: a side to kTexMax, then the longer side halved to kTexArea texels (stage 5.4: the
// game's font, C4 1024 x 192, at 256 lost three columns of four and its letters)
const unsigned kTexMax = 1024, kTexArea = 512 * 512;

// What a texture is made as: `mask` the PICA's own formats (gx_3ds.cpp gTexMask; 0: RGB565 / RGBA4 only) and
// whether there are mipmaps
struct Options {
    unsigned mask;
    bool mipmaps;
};

struct Shape {
    Pack pack;
    unsigned width, height;  // the PICA's, powers of two
    bool mipmaps;            // a chain of levels down to 8 texels (not beyond 256 texels a side)
    bool grey;               // a CMPR one made as L8 (for the log)
};

// Level 0 of a GX texture (`bytes` of data, all its levels or fewer) into w x h RGBA8 texels, the top row first
bool decode(unsigned format, unsigned width, unsigned height, unsigned tlutFormat, const std::uint8_t* data,
            std::size_t bytes, const std::uint8_t* tlut, unsigned entries, std::vector<std::uint8_t>& rgba);
// The PICA format and the full size for decoded texels
Shape choose(unsigned format, unsigned width, unsigned height, const std::vector<std::uint8_t>& rgba,
             const Options& options);
// The levels after the first (C3D_TexCalcMaxLevel: the smallest 8 texels a side)
unsigned maxLevel(unsigned width, unsigned height);
// The bytes of one level and of levels 0..last
std::size_t levelBytes(Pack pack, unsigned width, unsigned height);
std::size_t totalBytes(Pack pack, unsigned width, unsigned height, unsigned last);
// The room build needs for a texture h texels high at width x height, taken now (out of memory: std::bad_alloc)
void reserve(unsigned h, unsigned width, unsigned height);
// The texels (w x h RGBA8) at width x height in `pack`, levels 0..last, into `out` (totalBytes of them); with
// `etcError`, an ETC1 one's mean square error of level 0 (RE4_3DS_ETC1LOG)
void build(const std::uint8_t* rgba, unsigned w, unsigned h, Pack pack, unsigned width, unsigned height,
           unsigned last, std::uint8_t* out, double* etcError = nullptr);

// textures.pak: the head, the textures (all levels as build() makes them, each at kPakAlign), the index by key in order.
// The version goes up when the conversion above changes (a pack of another version is not used).
const std::uint32_t kPakMagic = 0x4B503452u;  // "R4PK"
const std::uint32_t kPakVersion = 1, kPakAlign = 16;

struct PakHead {
    std::uint32_t magic, version;
    std::uint32_t count, indexOffset;  // entries, where the index begins
    std::uint32_t mask, mipmaps;       // the Options the textures were made with
    std::uint32_t reserved[2];
};

struct PakEntry {
    std::uint64_t key;  // Re4GxTexture keyHi << 32 | keyLo
    std::uint32_t offset;
    std::uint8_t pack, widthLog2, heightLog2, mipmaps;
};
static_assert(sizeof(PakHead) == 32 && sizeof(PakEntry) == 16, "textures.pak layout");

} // namespace re4pica
