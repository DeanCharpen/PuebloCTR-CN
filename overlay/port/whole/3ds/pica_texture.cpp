// GX textures as the PICA's (stage 5.7a): the conversion of gx_3ds.cpp (stages 3.3b, 3.8, 5.4, 5.6), here so that
// the console and the host tool (tools/texpak.cpp) make the same texels. See pica_texture.h.
#include "pica_texture.h"

#include <cstring>

#include "gx_texture.h"

namespace re4pica {

typedef std::uint8_t u8;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
using std::size_t;

const unsigned kPackBits[kPacks] = {16, 16, 8, 16, 4, 8, 8};
const char* const kPackName[kPacks] = {"RGB565", "RGBA4", "LA4", "LA8", "ETC1", "ETC1A4", "L8"};

namespace {

// ETC1 (OES_compressed_ETC1_RGB8_texture), quickly: per layout of the two halves (2x4, 4x2) the base of each
// half is its average (5 bits and a difference, else 4 bits each); a texel's modifier m and the table are
// chosen by the error without clamping, sum (b + m - p)^2 = sum (b - p)^2 + 2 m D + 3 m^2 with D = sum (b - p),
// which clamping only lowers. `px` texels by x * 4 + y; with `weigh` the transparent ones do not count.
const int kEtcTable[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
// no divisions (calls on the ARM11): (s + w / 2) / w = ((2 s + w) * kEtcInv[w]) >> 17 for s <= 255 w, and
// x / 255 = (x * 257 + 257) >> 16 for x < 8160 (both checked for every value)
const u32 kEtcInv[9] = {0, 65536, 32768, 21845, 16384, 13107, 10923, 9362, 8192};

inline int div255(int x)
{
    return (x * 257 + 257) >> 16;
}

u64 etc1Block(const u8 (*px)[4], bool weigh)
{
    u64 best = 0;
    u32 bestErr = 0xFFFFFFFF;
    for (int flip = 0; flip < 2; flip++) {
        int at[2][8], n[2] = {0, 0}, avg[2][3];
        for (int i = 0; i < 16; i++) {
            const int h = flip ? (i & 3) >= 2 : i >= 8;
            at[h][n[h]++] = i;
        }
        for (int h = 0; h < 2; h++) {
            int sum[3] = {0, 0, 0}, w = 0;
            for (int k = 0; k < 8; k++) {
                const u8* p = px[at[h][k]];
                if (!weigh || p[3] >= 128) {
                    sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], w++;
                }
            }
            for (int c = 0; c < 3; c++) {
                avg[h][c] = w ? (int) ((u32) (2 * sum[c] + w) * kEtcInv[w] >> 17) : 0;
            }
        }
        int q[2][3], base[2][3];
        bool diff = true;
        for (int h = 0; h < 2; h++) {
            for (int c = 0; c < 3; c++) {
                q[h][c] = div255(avg[h][c] * 31 + 127);
            }
        }
        for (int c = 0; c < 3; c++) {
            const int d = q[1][c] - q[0][c];
            diff = diff && d >= -4 && d <= 3;
        }
        u64 v = (u64) diff << 33 | (u64) flip << 32;
        for (int c = 0; c < 3; c++) {
            if (diff) {
                base[0][c] = q[0][c] << 3 | q[0][c] >> 2;
                base[1][c] = q[1][c] << 3 | q[1][c] >> 2;
                v |= (u64) q[0][c] << (59 - 8 * c) | (u64) ((q[1][c] - q[0][c]) & 7) << (56 - 8 * c);
            } else {
                const int a = div255(avg[0][c] * 15 + 127), b = div255(avg[1][c] * 15 + 127);
                base[0][c] = a * 17;
                base[1][c] = b * 17;
                v |= (u64) a << (60 - 8 * c) | (u64) b << (56 - 8 * c);
            }
        }
        u32 err = 0;
        for (int h = 0; h < 2; h++) {
            int dsum[8], sq[8];
            for (int k = 0; k < 8; k++) {
                const u8* p = px[at[h][k]];
                const int r = base[h][0] - p[0], g = base[h][1] - p[1], b = base[h][2] - p[2];
                dsum[k] = r + g + b;
                sq[k] = r * r + g * g + b * b;
            }
            u32 tableErr = 0xFFFFFFFF, table = 0, bits = 0;
            for (u32 t = 0; t < 8; t++) {
                const int ta = kEtcTable[t][0], tb = kEtcTable[t][1];
                u32 e = 0, b = 0;
                for (int k = 0; k < 8; k++) {
                    const u8* p = px[at[h][k]];
                    // the least of 2 m D + 3 m^2: m against D's sign, the large one when 2 |D| > 3 (a + b)
                    const int d = dsum[k] < 0 ? -dsum[k] : dsum[k];
                    const bool large = 2 * d > 3 * (ta + tb);
                    const int m = large ? tb : ta;
                    const int low = 3 * m * m - 2 * m * d;
                    const int pick = (large ? 1 : 0) | (dsum[k] > 0 ? 2 : 0);
                    if (!weigh || p[3] >= 128) {
                        e += (u32) (sq[k] + low);  // the whole error, the halves' layouts compared by it
                    }
                    b |= (u32) pick << (2 * k);
                }
                if (e >= tableErr) {
                    break;  // the error goes up from here (all 8 tried: 0.1 dB better on r100, 1.8 times slower)
                }
                tableErr = e;
                table = t;
                bits = b;
            }
            err += tableErr;
            v |= (u64) table << (37 - 3 * h);
            for (int k = 0; k < 8; k++) {
                const u32 s = bits >> (2 * k) & 3, i = (u32) at[h][k];
                v |= (u64) (s >> 1) << (16 + i) | (u64) (s & 1) << i;
            }
        }
        if (err < bestErr) {
            bestErr = err;
            best = v;
        }
    }
    return best;
}

// The mean square error (of R, G, B) of level 0 packed by packEtc1, over the texels it shows (ETC1A4: alpha >= 128)
double etc1Error(const u8* level, u32 lw, u32 lh, const u8* out, bool alpha)
{
    const u64* o = (const u64*) out;
    double sum = 0;
    u32 n = 0;
    for (u32 ty = 0; ty < lh; ty += 8) {
        for (u32 tx = 0; tx < lw; tx += 8) {
            for (u32 by = 0; by < 8; by += 4) {
                for (u32 bx = 0; bx < 8; bx += 4) {
                    o += alpha;
                    const u64 v = *o++;
                    const int diff = v >> 33 & 1, flip = v >> 32 & 1;
                    int base[2][3];
                    for (int c = 0; c < 3; c++) {
                        if (diff) {
                            const int a = v >> (59 - 8 * c) & 31;
                            int d = v >> (56 - 8 * c) & 7;
                            d -= d >= 4 ? 8 : 0;
                            base[0][c] = a << 3 | a >> 2;
                            base[1][c] = (a + d) << 3 | (a + d) >> 2;
                        } else {
                            base[0][c] = (v >> (60 - 8 * c) & 15) * 17;
                            base[1][c] = (v >> (56 - 8 * c) & 15) * 17;
                        }
                    }
                    const int table[2] = {(int) (v >> 37 & 7), (int) (v >> 34 & 7)};
                    for (u32 x = 0; x < 4; x++) {
                        for (u32 y = 0; y < 4; y++) {
                            const u32 i = x * 4 + y;
                            const int h = flip ? y >= 2 : x >= 2;
                            const int s = (int) ((v >> (16 + i) & 1) << 1 | (v >> i & 1));
                            const int m = (s & 1 ? kEtcTable[table[h]][1] : kEtcTable[table[h]][0]) * (s & 2 ? -1 : 1);
                            const u8* p = &level[((size_t) (lh - 1 - (ty + by + y)) * lw + tx + bx + x) * 4];
                            if (alpha && p[3] < 128) {
                                continue;
                            }
                            for (int c = 0; c < 3; c++) {
                                int q = base[h][c] + m;
                                q = q < 0 ? 0 : q > 255 ? 255 : q;
                                sum += (double) (q - p[c]) * (q - p[c]);
                            }
                            n++;
                        }
                    }
                }
            }
        }
    }
    return n ? sum / (3.0 * n) : 0.0;
}

// One level (RGBA8, GX rows: the top first) as ETC1 / ETC1A4 of the PICA: the rows from the bottom, 8x8 tiles
// of four 4x4 blocks (left top, right top, left bottom, right bottom), each block a native u64 (ETC1A4: its
// alpha first, 4 bits a texel by x * 4 + y)
void packEtc1(const u8* level, u32 lw, u32 lh, u8* out, bool alpha)
{
    u64* o = (u64*) out;
    u8 px[16][4];
    for (u32 ty = 0; ty < lh; ty += 8) {
        for (u32 tx = 0; tx < lw; tx += 8) {
            for (u32 by = 0; by < 8; by += 4) {
                for (u32 bx = 0; bx < 8; bx += 4) {
                    for (u32 x = 0; x < 4; x++) {
                        for (u32 y = 0; y < 4; y++) {
                            memcpy(px[x * 4 + y], &level[((size_t) (lh - 1 - (ty + by + y)) * lw + tx + bx + x) * 4], 4);
                        }
                    }
                    if (alpha) {
                        u64 a = 0;
                        for (int i = 0; i < 16; i++) {
                            a |= (u64) (px[i][3] >> 4) << (4 * i);
                        }
                        *o++ = a;
                    }
                    *o++ = etc1Block(px, alpha);
                }
            }
        }
    }
}

u32 pow2(u32 v)
{
    u32 p = 8;
    while (p < v && p < kTexMax) {
        p <<= 1;
    }
    return p;
}

// Morton order inside an 8x8 tile of the PICA
u32 morton(u32 x, u32 y)
{
    u32 i = 0;
    for (u32 b = 0; b < 3; b++) {
        i |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    }
    return i;
}

// Grey to within 8 in each texel (CMPR's ends are RGB565: in grey ones the green comes out apart)
bool isGrey(const std::vector<std::uint8_t>& rgba)
{
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const int r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
        if (r - g > 8 || g - r > 8 || b - g > 8 || g - b > 8) {
            return false;
        }
    }
    return true;
}

// The GX texels to the PICA's size along one axis (stage 5.4; before, the nearest texel: columns dropped or doubled):
// `lines` lines of `n` RGBA8 texels `step` bytes apart (lines `skip` apart) to `m` texels (`outStep`, `outSkip`).
// Up: linear between the two nearest texel centres (clamped at the ends); down: the mean of the texels each covers.
void resampleLine(const u8* in, u32 n, size_t step, u32 lines, size_t skip, u8* out, u32 m, size_t outStep,
                  size_t outSkip)
{
    for (u32 l = 0; l < lines; l++) {
        const u8* src = in + l * skip;
        u8* dst = out + l * outSkip;
        for (u32 i = 0; i < m; i++) {
            u8* o = dst + i * outStep;
            if (m == n) {
                memcpy(o, src + i * step, 4);
            } else if (m > n) {
                // the centre of output texel i in input texels, 8 bits of fraction
                const int at = (int) (((2 * i + 1) * n * 256) / (2 * m)) - 128;
                const int a = at < 0 ? 0 : at >> 8, f = at < 0 ? 0 : at & 255;
                const int b = a + 1 < (int) n ? a + 1 : a;
                const u8* p = src + a * step;
                const u8* q = src + b * step;
                for (int k = 0; k < 4; k++) {
                    o[k] = (u8) ((p[k] * (256 - f) + q[k] * f + 128) >> 8);
                }
            } else {
                const u32 a = i * n / m, b = (i + 1) * n / m > a ? (i + 1) * n / m : a + 1;
                u32 sum[4] = {};
                for (u32 j = a; j < b; j++) {
                    const u8* p = src + j * step;
                    for (int k = 0; k < 4; k++) {
                        sum[k] += p[k];
                    }
                }
                const u32 c = b - a;
                for (int k = 0; k < 4; k++) {
                    o[k] = (u8) ((sum[k] + c / 2) / c);
                }
            }
        }
    }
}

} // namespace

bool decode(unsigned format, unsigned width, unsigned height, unsigned tlutFormat, const std::uint8_t* data,
            std::size_t bytes, const std::uint8_t* tlut, unsigned entries, std::vector<std::uint8_t>& rgba)
{
    const size_t level0 = re4gx::GxTextureLevelBytes(format, width, height);
    return re4gx::GxDecodeTexture(data, level0 < bytes ? level0 : bytes, format, width, height, tlut, entries,
                                  tlutFormat, rgba);
}

Shape choose(unsigned format, unsigned w, unsigned h, const std::vector<std::uint8_t>& rgba, const Options& options)
{
    u32 W = pow2(w), H = pow2(h);
    // the large ones of 1024 texels a side are the pictures of the screens and the text (C4, C8, I8 ...); the CMPR ones
    // (the models' and rooms', the menus' backdrops) stay at 256 (with all at 1024 linear memory ran out: 172 textures
    // a log window not made, the prompts of the action button gone, r100-3ds-view2.log)
    const u32 side = format == re4gx::kTexCMPR ? 256 : kTexMax;
    W = W < side ? W : side;
    H = H < side ? H : side;
    while (W * H > kTexArea) {
        (W >= H ? W : H) >>= 1;
    }
    // without mipmaps beyond 256 (a picture of the screen: drawn near its size; so too when made smaller for room)
    const bool flat = W > 256 || H > 256;
    bool alpha = false;
    for (size_t i = 3; i < rgba.size() && !alpha; i += 4) {
        alpha = rgba[i] != 255;
    }
    Pack pack = alpha ? kRgba4 : kRgb565;
    bool grey = false;
    const u32 mask = options.mask;
    switch (format) {
    case re4gx::kTexI4:
    case re4gx::kTexIA4:
        pack = mask & 1 ? kLa4 : pack;
        break;
    case re4gx::kTexI8:
    case re4gx::kTexIA8:
        pack = mask & 2 ? kLa8 : pack;
        break;
    case re4gx::kTexCMPR:
        // the opaque ones too: with ETC1 Leon's hair and, on the move, textures around flickered on the 3DS
        // (the data right; why not found, 01.11.2026)
        pack = mask & (alpha ? 8 : 4) ? kEtc1a4 : pack;
        pack = pack == kEtc1a4 && !alpha && (mask & 64) ? kEtc1 : pack;  // bit 64: plain ETC1 again (a test)
        if (!alpha && (mask & 16) && isGrey(rgba)) {
            pack = kL8;  // (ETC1's errors on the gloss of Leon's hair flickered as it moved)
            grey = true;
        }
        break;
    default:
        // grey ones (the font, C4 with RGB5A3 greys of 4 bits and their alpha of 3: common_p.fnt 1024x192,
        // stage1_p.fnt 1024x1024) at a half or less: from the 4-bit formats LA4, else LA8; opaque L8
        if ((mask & 32) && isGrey(rgba)) {
            pack = !alpha ? kL8 : format == re4gx::kTexC4 ? kLa4 : kLa8;
        }
        break;
    }
    return {pack, W, H, options.mipmaps && !flat, grey};
}

unsigned maxLevel(unsigned width, unsigned height)
{
    return (31 - __builtin_clz(width < height ? width : height)) - 3;
}

std::size_t levelBytes(Pack pack, unsigned width, unsigned height)
{
    return (size_t) width * height * kPackBits[pack] / 8;
}

std::size_t totalBytes(Pack pack, unsigned width, unsigned height, unsigned last)
{
    size_t bytes = 0;
    for (u32 l = 0; l <= last; l++) {
        bytes += levelBytes(pack, width >> l, height >> l);
    }
    return bytes;
}

// level 0 at the PICA's size (RGBA8, GX rows in order), then each level the 2x2 average of the one before (without
// them the trees of r100 shimmered: one texel of many per pixel)
std::vector<u8> gLevel, gAcross;

void reserve(unsigned h, unsigned width, unsigned height)
{
    gLevel.resize((size_t) width * height * 4);
    gAcross.resize((size_t) width * h * 4);
}

void build(const std::uint8_t* rgba, unsigned w, unsigned h, Pack pack, unsigned W, unsigned H, unsigned last,
           std::uint8_t* out, double* etcError)
{
    reserve(h, W, H);
    std::vector<u8>& level = gLevel;
    resampleLine(rgba, w, 4, h, (size_t) w * 4, gAcross.data(), W, 4, (size_t) W * 4);
    resampleLine(gAcross.data(), h, (size_t) W * 4, W, 4, level.data(), H, (size_t) W * 4, 4);
    u32 lw = W, lh = H;
    for (u32 l = 0; l <= last; l++) {
        if (l > 0) {
            const u32 nw = lw / 2, nh = lh / 2;
            for (u32 y = 0; y < nh; y++) {
                for (u32 x = 0; x < nw; x++) {
                    const u8* a = &level[((size_t) (2 * y) * lw + 2 * x) * 4];
                    const u8* b = a + (size_t) lw * 4;
                    u8* o = &level[((size_t) y * nw + x) * 4];  // behind what it reads
                    for (int k = 0; k < 4; k++) {
                        o[k] = (u8) ((a[k] + a[k + 4] + b[k] + b[k + 4] + 2) >> 2);
                    }
                }
            }
            lw = nw;
            lh = nh;
        }
        // the PICA's rows go up from t = 0 at the first row in memory: GX row 0 (t = 0, the top)
        // last (the first try had GX row y at PICA row y: the letters of the warning screen upside down)
        if (pack == kEtc1 || pack == kEtc1a4) {
            packEtc1(level.data(), lw, lh, out, pack == kEtc1a4);
            if (etcError && l == 0) {
                *etcError = etc1Error(level.data(), lw, lh, out, pack == kEtc1a4);
            }
        } else {
            for (u32 y = 0; y < lh; y++) {
                const u8* row = &level[(size_t) (lh - 1 - y) * lw * 4];
                for (u32 x = 0; x < lw; x++) {
                    const u8* c = row + (size_t) x * 4;
                    const u32 i = ((y >> 3) * (lw >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7);
                    switch (pack) {
                    case kRgba4:
                        ((std::uint16_t*) out)[i] =
                            (std::uint16_t) ((c[0] >> 4) << 12 | (c[1] >> 4) << 8 | (c[2] >> 4) << 4 | c[3] >> 4);
                        break;
                    case kLa4:
                        out[i] = (u8) ((c[0] & 0xF0) | c[3] >> 4);
                        break;
                    case kLa8:
                        ((std::uint16_t*) out)[i] = (std::uint16_t) (c[0] << 8 | c[3]);
                        break;
                    case kL8:
                        out[i] = (u8) ((c[0] + 2 * c[1] + c[2] + 2) >> 2);
                        break;
                    default:
                        ((std::uint16_t*) out)[i] = (std::uint16_t) ((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3);
                        break;
                    }
                }
            }
        }
        out += levelBytes(pack, lw, lh);
    }
}

} // namespace re4pica
