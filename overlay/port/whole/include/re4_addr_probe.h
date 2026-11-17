/* Stage 2.1 probe (-DRE4_ADDR_PROBE, never in a normal build): which values reach the game's
 * "address, not a file offset" tests (RE4_IS_ADDR, formats relocated()). Each test site reports
 * once per kind of value: MEM1, a small value (below 16 MB: a file offset; again at each new
 * maximum past twice the last one reported), anything else (with the /proc/self/maps line holding
 * it; the first four distinct values per site). */
#ifndef RE4_ADDR_PROBE_H
#define RE4_ADDR_PROBE_H
#include <stdio.h>
#include <string.h>

struct re4_addr_probe_site {
    unsigned char seen[3];
    unsigned char others;
    unsigned int last[4];
    unsigned int max;
};

static inline int re4_addr_probe(struct re4_addr_probe_site* s, unsigned int v, int res, const char* file, int line)
{
    int kind = v - (unsigned int) RE4_MEM1_BASE <= 0x02FFFFFFu ? 0 : v < 0x01000000u ? 1 : 2;
    if (kind == 2) {
        int i;
        for (i = 0; i < s->others; i++) {
            if (s->last[i] == v) {
                return res;
            }
        }
        if (s->others == 4) {
            return res;
        }
        s->last[s->others++] = v;
    } else if (kind == 1) {
        if (s->seen[1] && v <= 2 * s->max) {
            return res;
        }
        s->max = v;
    } else if (s->seen[kind]) {
        return res;
    }
    s->seen[kind] = 1;
    {
        static const char* names[3] = {"mem1", "small", "other"};
        char where[200] = "";
        if (kind == 2) {
            FILE* f = fopen("/proc/self/maps", "r");
            char ln[256];
            while (f && fgets(ln, sizeof(ln), f)) {
                unsigned long a, b;
                if (sscanf(ln, "%lx-%lx", &a, &b) == 2 && v >= a && v < b) {
                    ln[strcspn(ln, "\n")] = 0;
                    snprintf(where, sizeof(where), " in [%s]", ln);
                    break;
                }
            }
            if (f) {
                fclose(f);
            }
        }
        fprintf(stderr, "addrprobe: %s:%d %s %08x -> %s%s\n", file, line, names[kind], v,
                res ? "addr" : "offset", where);
    }
    return res;
}

#define RE4_ADDR_PROBE_AT(v, res)                                                              \
    ({                                                                                         \
        static struct re4_addr_probe_site re4_site_;                                           \
        re4_addr_probe(&re4_site_, (unsigned int) (unsigned long) (v), (res), __FILE__, __LINE__); \
    })
#endif
