// The display list cache's triangles as strips made anew and put in order for the PICA's post-transform vertex cache
// (2026-11-18; gx_decode.cpp keptBuild, RE4_3DS_VCACHE). The game's lists are short strips, single triangles and
// quads (2.2-3 indices a triangle once joined into one strip), in an order made for no cache: the console shaded
// 1.3-1.5 times the distinct vertices (a simulation over Linux frame dumps; the cache holds 24-31 entries
// by a probe on the console, r109: an index ~50-77 ns, a shaded vertex ~90-130 ns).
//
// restrip(): the triangles (their winding kept, none without area) grown into strips greedily, the strips cut to
// `cut` triangles, then drawn in the order that shades the fewest vertices now (a FIFO of `cache` entries, a hit
// does not refresh; empty at the draw's begin) and joined into one strip as stripAppend does: every part begins at
// an even place (`starts`: the pieces of plan 5b are cut there). Only for draws whose picture does not depend on the
// triangles' order (the caller's check). No other code of the port is read: built on its own by a host check.
#pragma once

#include <stdint.h>
#include <string.h>

#include <vector>

namespace re4restrip {

// Vertices a draw of these indices shades
inline uint32_t shaded(const uint16_t* ix, uint32_t n, uint32_t vertices, uint32_t cache)
{
    std::vector<int32_t> ins(vertices, -0x40000000);
    int32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t& t = ins[ix[i]];
        if (count - t >= (int32_t) cache) {
            t = count++;
        }
    }
    return (uint32_t) count;
}

struct Part {
    uint32_t from, count;  // in the pool of indices
    uint32_t odd;          // its first triangle is turned (grown by an odd count in front, or cut at an odd place)
};

// tri: n indices, 3 a triangle (a != b != c); vertices: all indices below it. out: one strip; starts: where each
// part begins (even places). False: nothing made (no triangles).
inline bool restrip(const uint16_t* tri, uint32_t n, uint32_t vertices, uint32_t cache, uint32_t cut,
                    std::vector<uint16_t>& out, std::vector<uint32_t>* starts)
{
    const uint32_t nt = n / 3;
    if (nt == 0 || cache == 0 || cut == 0) {
        return false;
    }
    // the triangles by their directed edges (a -> b, b -> c, c -> a): open addressing, alike keys one after another
    struct Edge {
        uint32_t key, tri;
    };
    const uint32_t kNone = 0xFFFFFFFFu;
    uint32_t bits = 4;
    while ((1u << bits) < nt * 4) {  // (3 nt edges: at most 3/4 full)
        bits++;
    }
    const uint32_t mask = (1u << bits) - 1;
    std::vector<Edge> edges((size_t) 1 << bits, Edge{0, kNone});
    const auto slotOf = [bits](uint32_t key) { return key * 0x9E3779B1u >> (32 - bits); };
    for (uint32_t t = 0; t < nt; t++) {
        const uint16_t* p = tri + 3 * t;
        for (int e = 0; e < 3; e++) {
            const uint32_t key = (uint32_t) p[e] << 16 | p[e == 2 ? 0 : e + 1];
            uint32_t s = slotOf(key);
            while (edges[s].tri != kNone) {
                s = (s + 1) & mask;
            }
            edges[s] = Edge{key, t};
        }
    }
    std::vector<uint8_t> done(nt, 0);
    // a triangle not drawn yet with the edge u -> v, its third vertex
    const auto next = [&](uint32_t u, uint32_t v, uint32_t& third) -> uint32_t {
        const uint32_t key = u << 16 | v;
        for (uint32_t s = slotOf(key); edges[s].tri != kNone; s = (s + 1) & mask) {
            if (edges[s].key == key && !done[edges[s].tri]) {
                const uint16_t* p = tri + 3 * edges[s].tri;
                third = p[0] == u && p[1] == v ? p[2] : p[1] == u && p[2] == v ? p[0] : p[1];
                return edges[s].tri;
            }
        }
        return kNone;
    };
    // a strip from triangle t0 turned `rot` times, grown at its end, then in front; the triangles taken in `used`
    std::vector<uint16_t> back, front;
    std::vector<uint32_t> used;
    const auto grow = [&](uint32_t t0, int rot) {
        const uint16_t* p = tri + 3 * t0;
        back.clear();
        front.clear();
        used.clear();
        back.push_back(p[rot]);
        back.push_back(p[(rot + 1) % 3]);
        back.push_back(p[(rot + 2) % 3]);
        done[t0] = 1;
        used.push_back(t0);
        for (uint32_t k = 1;; k++) {  // triangle k: (s[k], s[k + 1], x), turned when k is odd
            const uint32_t a = back[back.size() - 2], b = back[back.size() - 1];
            uint32_t x = 0;
            const uint32_t t = (k & 1) ? next(b, a, x) : next(a, b, x);
            if (t == kNone) {
                break;
            }
            back.push_back((uint16_t) x);
            done[t] = 1;
            used.push_back(t);
        }
        uint32_t a = back[0], b = back[1];
        for (uint32_t k = 0;; k++) {  // the triangle before: (x, a, b), turned when its place is odd
            uint32_t x = 0;
            const uint32_t t = (k & 1) ? next(a, b, x) : next(b, a, x);
            if (t == kNone) {
                break;
            }
            front.push_back((uint16_t) x);
            done[t] = 1;
            used.push_back(t);
            b = a;
            a = x;
        }
    };
    std::vector<uint16_t> pool;
    pool.reserve((size_t) nt * 2);
    std::vector<Part> parts;
    for (uint32_t t0 = 0; t0 < nt; t0++) {
        if (done[t0]) {
            continue;
        }
        int best = 0;
        size_t most = 0;
        for (int rot = 0; rot < 3; rot++) {
            grow(t0, rot);
            if (used.size() > most) {
                most = used.size();
                best = rot;
            }
            for (uint32_t t : used) {
                done[t] = 0;
            }
        }
        grow(t0, best);
        const uint32_t from = (uint32_t) pool.size(), odd = (uint32_t) front.size() & 1;
        pool.insert(pool.end(), front.rbegin(), front.rend());
        pool.insert(pool.end(), back.begin(), back.end());
        const uint32_t tris = (uint32_t) pool.size() - from - 2;
        for (uint32_t k = 0; k < tris; k += cut) {  // (the cuts share two indices)
            const uint32_t end = k + cut < tris ? k + cut : tris;
            parts.push_back(Part{from + k, end - k + 2, (odd + k) & 1});
        }
    }
    std::vector<Edge>().swap(edges);
    std::vector<uint8_t>().swap(done);
    // the parts of each vertex
    const uint32_t np = (uint32_t) parts.size();
    std::vector<uint32_t> first(vertices + 1, 0);
    for (const Part& p : parts) {
        for (uint32_t i = 0; i < p.count; i++) {
            first[pool[p.from + i] + 1]++;
        }
    }
    for (uint32_t v = 0; v < vertices; v++) {
        first[v + 1] += first[v];
    }
    std::vector<uint32_t> of(first[vertices]);
    {
        std::vector<uint32_t> at(first.begin(), first.end() - 1);
        for (uint32_t i = 0; i < np; i++) {
            for (uint32_t j = 0; j < parts[i].count; j++) {
                of[at[pool[parts[i].from + j]]++] = i;
            }
        }
    }
    // the order: the next part is the one that shades the fewest vertices a triangle now, of those with a vertex in
    // the cache (the first of equals); none - the first not drawn yet
    std::vector<int32_t> ins(vertices, -0x40000000);
    int32_t count = 0;
    std::vector<uint16_t> recent(cache, 0);  // the vertices as they went in (a ring)
    uint32_t recentAt = 0, recentHeld = 0;
    std::vector<uint32_t> seen(np, kNone);
    std::vector<uint8_t> drawn(np, 0);
    std::vector<uint32_t> undoV;
    std::vector<int32_t> undoT;
    uint32_t firstLeft = 0;
    out.clear();
    out.reserve(pool.size() + (size_t) np * 3);
    if (starts) {
        starts->clear();
    }
    for (uint32_t step = 0; step < np; step++) {
        uint32_t best = kNone, bestMisses = 0, bestTris = 1;
        for (uint32_t r = 0; r < recentHeld; r++) {
            const uint32_t v = recent[r];
            for (uint32_t a = first[v]; a < first[v + 1]; a++) {
                const uint32_t i = of[a];
                if (drawn[i] || seen[i] == step) {
                    continue;
                }
                seen[i] = step;
                const Part& p = parts[i];
                undoV.clear();
                undoT.clear();
                int32_t c = count;
                for (uint32_t j = 0; j < p.count; j++) {
                    const uint32_t x = pool[p.from + j];
                    if (c - ins[x] >= (int32_t) cache) {
                        undoV.push_back(x);
                        undoT.push_back(ins[x]);
                        ins[x] = c++;
                    }
                }
                for (size_t u = undoV.size(); u-- > 0;) {
                    ins[undoV[u]] = undoT[u];
                }
                const uint32_t misses = (uint32_t) (c - count), tris = p.count - 2;
                // misses / tris < bestMisses / bestTris, the first of equals
                const uint64_t l = (uint64_t) misses * bestTris, rr = (uint64_t) bestMisses * tris;
                if (best == kNone || l < rr || (l == rr && i < best)) {
                    best = i;
                    bestMisses = misses;
                    bestTris = tris;
                }
            }
        }
        if (best == kNone) {
            while (drawn[firstLeft]) {
                firstLeft++;
            }
            best = firstLeft;
        }
        drawn[best] = 1;
        const Part& p = parts[best];
        for (uint32_t j = 0; j < p.count; j++) {
            const uint32_t x = pool[p.from + j];
            if (count - ins[x] >= (int32_t) cache) {
                ins[x] = count++;
                recent[recentAt] = (uint16_t) x;
                recentAt = recentAt + 1 == cache ? 0 : recentAt + 1;
                recentHeld += recentHeld < cache;
            }
        }
        // joined: the last index and this part's first; once more where its first triangle would turn the wrong way
        const uint16_t p0 = pool[p.from];
        if (!out.empty()) {
            out.push_back(out.back());
            out.push_back(p0);
        }
        if ((out.size() & 1) != p.odd) {
            out.push_back(p0);
        }
        if (starts) {
            starts->push_back((uint32_t) out.size() - p.odd);  // (odd: the index before is its first too)
        }
        out.insert(out.end(), pool.begin() + p.from, pool.begin() + p.from + p.count);
    }
    return true;
}

}  // namespace re4restrip
