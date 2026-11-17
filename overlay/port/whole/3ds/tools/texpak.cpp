// The pack of PICA textures for the 3DS (stage 5.7a): the GX textures the game handed over (runtime/re4_tex_dump.h:
// RE4_TEXDUMP of a tour on Linux, RE4_3DS_TEXMISS of the console) made as gx_3ds.cpp makes them (pica_texture.cpp),
// into textures.pak (pica_texture.h), which the console reads instead of making them itself.
//
//   texpak [--mask N] [--no-mipmaps] OUT.pak DUMP...
//
// Each DUMP is one run (the address filter). A DUMP may hold alias records (RTXA, PuebloPrep --textures): a key that shares
// the texels of another record's key (the same image hashed with another count of mip levels) - its entry is the
// other's, with its own key.
// --mask: the PICA formats (gx_3ds.cpp gTexMask, its default 63); the console uses the pack only with the same.
// Built on the host: clang++ pica_texture.cpp gx_texture.cpp texpak.cpp.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "../../runtime/re4_tex_dump.h"
#include "../pica_texture.h"

namespace {

struct Stats {
    unsigned count;
    std::size_t bytes;
};

// more keys than this of one address and size in one run: a picture the game writes (a movie's: 295 a minute at the
// start of r100), not for the pack
const unsigned kDynamic = 8;

// (R5) an alias: `key` reads the texture of `to` (32 bytes, as a record's head)
const std::uint32_t kAliasMagic = 0x41585452u;  // "RTXA"
struct Alias {
    std::uint32_t magic, keyLo, keyHi, toLo, toHi, zero[3];
};
static_assert(sizeof(Alias) == 32, "RTXA");

} // namespace

int main(int argc, char** argv)
{
    re4pica::Options options = {63, true};
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!std::strcmp(argv[i], "--mask") && i + 1 < argc) {
            options.mask = (unsigned) std::strtoul(argv[++i], nullptr, 0);
        } else if (!std::strcmp(argv[i], "--no-mipmaps")) {
            options.mipmaps = false;
        } else {
            std::fprintf(stderr, "texpak: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (argc - i < 2) {
        std::fprintf(stderr, "usage: texpak [--mask N] [--no-mipmaps] OUT.pak DUMP...\n");
        return 2;
    }
    const char* outPath = argv[i++];
    FILE* out = std::fopen(outPath, "wb");
    if (!out) {
        std::perror(outPath);
        return 1;
    }
    re4pica::PakHead head = {};
    std::fwrite(&head, sizeof(head), 1, out);
    std::size_t pos = sizeof(head);
    std::map<std::uint64_t, re4pica::PakEntry> index;
    std::map<std::uint64_t, std::uint32_t> dataBytesOf;
    Stats byPack[re4pica::kPacks] = {}, byFormat[16] = {};
    unsigned records = 0, again = 0, mismatched = 0, undecoded = 0, dynamic = 0;
    std::vector<std::uint8_t> rgba, texels;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> aliases;  // (R5) key, to
    for (; i < argc; i++) {
        FILE* in = std::fopen(argv[i], "rb");
        if (!in) {
            std::perror(argv[i]);
            return 1;
        }
        std::vector<std::uint8_t> file;
        std::uint8_t chunk[1 << 16];
        for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), in)) > 0;) {
            file.insert(file.end(), chunk, chunk + n);
        }
        std::fclose(in);
        // the records; the keys of each address and size (in one run)
        std::vector<std::size_t> at;
        std::map<std::tuple<std::uint32_t, unsigned, unsigned, unsigned>, std::set<std::uint64_t>> keysAt;
        for (std::size_t o = 0; o + sizeof(Re4TexDumpHead) <= file.size();) {
            Re4TexDumpHead d;
            std::memcpy(&d, &file[o], sizeof(d));
            if (d.magic == kAliasMagic) {  // (R5)
                Alias a;
                std::memcpy(&a, &file[o], sizeof(a));
                aliases.emplace_back((std::uint64_t) a.keyHi << 32 | a.keyLo, (std::uint64_t) a.toHi << 32 | a.toLo);
                o += sizeof(a);
                continue;
            }
            if (d.magic != RE4_TEX_DUMP_MAGIC) {
                std::fprintf(stderr, "texpak: %s: not a record at %zu\n", argv[i], o);
                return 1;
            }
            const std::size_t n = d.dataBytes + (std::size_t) d.entries * 2;
            if (o + sizeof(d) + n > file.size()) {
                std::fprintf(stderr, "texpak: %s: cut short\n", argv[i]);
                break;
            }
            at.push_back(o);
            keysAt[std::make_tuple(d.address, (unsigned) d.format, (unsigned) d.width, (unsigned) d.height)].insert(
                (std::uint64_t) d.keyHi << 32 | d.keyLo);
            o += sizeof(d) + ((n + 3) & ~(std::size_t) 3);
        }
        for (std::size_t o : at) {
            Re4TexDumpHead d;
            std::memcpy(&d, &file[o], sizeof(d));
            const std::uint8_t* data = &file[o + sizeof(d)];
            const std::uint8_t* palette = data + d.dataBytes;
            records++;
            const std::uint64_t key = (std::uint64_t) d.keyHi << 32 | d.keyLo;
            if (keysAt[std::make_tuple(d.address, (unsigned) d.format, (unsigned) d.width, (unsigned) d.height)].size() >
                kDynamic) {
                dynamic++;
                continue;
            }
            if (index.count(key)) {
                again++;
                mismatched += dataBytesOf[key] != d.dataBytes;
                continue;
            }
            if (!re4pica::decode(d.format, d.width, d.height, d.tlutFormat, data, d.dataBytes, palette, d.entries,
                                 rgba)) {
                undecoded++;
                continue;
            }
            const re4pica::Shape shape = re4pica::choose(d.format, d.width, d.height, rgba, options);
            const unsigned last = shape.mipmaps ? re4pica::maxLevel(shape.width, shape.height) : 0;
            const std::size_t bytes = re4pica::totalBytes(shape.pack, shape.width, shape.height, last);
            texels.assign(bytes, 0);
            re4pica::build(rgba.data(), d.width, d.height, shape.pack, shape.width, shape.height, last, texels.data());
            re4pica::PakEntry e = {};
            e.key = key;
            e.offset = (std::uint32_t) pos;
            e.pack = (std::uint8_t) shape.pack;
            e.widthLog2 = (std::uint8_t) __builtin_ctz(shape.width);
            e.heightLog2 = (std::uint8_t) __builtin_ctz(shape.height);
            e.mipmaps = shape.mipmaps;
            const std::size_t padded = (bytes + re4pica::kPakAlign - 1) & ~(std::size_t) (re4pica::kPakAlign - 1);
            texels.resize(padded, 0);
            if (std::fwrite(texels.data(), 1, padded, out) != padded) {
                std::perror(outPath);
                return 1;
            }
            pos += padded;
            if (pos >> 32) {
                std::fprintf(stderr, "texpak: the pack is over 4 GB\n");
                return 1;
            }
            index[key] = e;
            dataBytesOf[key] = d.dataBytes;
            byPack[shape.pack].count++;
            byPack[shape.pack].bytes += bytes;
            byFormat[d.format & 15].count++;
            byFormat[d.format & 15].bytes += bytes;
        }
    }
    unsigned aliased = 0, orphans = 0;  // (R5) the aliases: the entry of their target
    for (const auto& a : aliases) {
        const auto to = index.find(a.second);
        if (to == index.end()) {
            orphans++;
        } else if (!index.count(a.first)) {
            re4pica::PakEntry e = to->second;
            e.key = a.first;
            index[a.first] = e;
            aliased++;
        }
    }
    if (!aliases.empty()) {
        std::printf("texpak: %zu aliases: %u keys sharing a texture, %u without their texture\n", aliases.size(),
                    aliased, orphans);
    }
    head.magic = re4pica::kPakMagic;
    head.version = re4pica::kPakVersion;
    head.count = (std::uint32_t) index.size();
    head.indexOffset = (std::uint32_t) pos;
    head.mask = options.mask;
    head.mipmaps = options.mipmaps;
    for (const auto& e : index) {  // std::map: in key order
        std::fwrite(&e.second, sizeof(e.second), 1, out);
    }
    std::fseek(out, 0, SEEK_SET);
    std::fwrite(&head, sizeof(head), 1, out);
    if (std::fclose(out) != 0) {
        std::perror(outPath);
        return 1;
    }
    std::printf("texpak: %u records, %u textures (%u again, %u of them of another size; %u not decoded; %u of addresses "
                "with over %u keys left out), %.1f MB, index %.1f KB; mask %u, mipmaps %d\n",
                records, (unsigned) index.size(), again, mismatched, undecoded, dynamic, kDynamic, pos / 1048576.0,
                index.size() * sizeof(re4pica::PakEntry) / 1024.0, options.mask, options.mipmaps);
    for (unsigned p = 0; p < re4pica::kPacks; p++) {
        if (byPack[p].count) {
            std::printf("  %-7s %6u textures %8.1f MB\n", re4pica::kPackName[p], byPack[p].count,
                        byPack[p].bytes / 1048576.0);
        }
    }
    static const char* const kFormat[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "7",
                                            "C4", "C8", "C14X2", "11", "12", "13", "CMPR", "15"};
    for (unsigned f = 0; f < 16; f++) {
        if (byFormat[f].count) {
            std::printf("  GX %-6s %6u textures %8.1f MB\n", kFormat[f], byFormat[f].count, byFormat[f].bytes / 1048576.0);
        }
    }
    return 0;
}
