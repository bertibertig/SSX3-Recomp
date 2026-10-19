/*
 * LAN multiplayer – little-endian byte writer/reader for packets (docs/lan-multiplayer.md, phase 1).
 *
 * Every field goes on the wire explicitly in little-endian order, so hosts of any byte order and compiler
 * agree; no structs are sent as raw memory. A reader that runs past its end sets `bad` and returns zeros, so
 * a truncated or hostile packet is detected once, after all fields are read.
 */
#ifndef SSX3_NET_WIRE_H
#define SSX3_NET_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *p;
    size_t   len, cap;
    int      bad;
} net_writer;

typedef struct {
    const uint8_t *p;
    size_t         len, pos;
    int            bad;
} net_reader;

static inline void nw_init(net_writer *w, void *buf, size_t cap)
{
    w->p = (uint8_t *)buf; w->len = 0; w->cap = cap; w->bad = 0;
}

static inline void nw_u8(net_writer *w, uint32_t v)
{
    if (w->len + 1 > w->cap) { w->bad = 1; return; }
    w->p[w->len++] = (uint8_t)v;
}

static inline void nw_u16(net_writer *w, uint32_t v) { nw_u8(w, v); nw_u8(w, v >> 8); }
static inline void nw_u32(net_writer *w, uint32_t v) { nw_u16(w, v); nw_u16(w, v >> 16); }
static inline void nw_u64(net_writer *w, uint64_t v) { nw_u32(w, (uint32_t)v); nw_u32(w, (uint32_t)(v >> 32)); }

static inline void nw_f32(net_writer *w, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    nw_u32(w, v);
}

/* A string: u8 length, then the bytes (no terminator), cut at `max`. */
static inline void nw_str(net_writer *w, const char *s, size_t max)
{
    size_t n = s ? strlen(s) : 0, i;
    if (n > max) n = max;
    if (n > 255) n = 255;
    nw_u8(w, (uint32_t)n);
    for (i = 0; i < n; i++)
        nw_u8(w, (uint8_t)s[i]);
}

static inline void nr_init(net_reader *r, const void *buf, size_t len)
{
    r->p = (const uint8_t *)buf; r->len = len; r->pos = 0; r->bad = 0;
}

static inline uint32_t nr_u8(net_reader *r)
{
    if (r->pos + 1 > r->len) { r->bad = 1; return 0; }
    return r->p[r->pos++];
}

static inline uint32_t nr_u16(net_reader *r) { uint32_t v = nr_u8(r); return v | nr_u8(r) << 8; }
static inline uint32_t nr_u32(net_reader *r) { uint32_t v = nr_u16(r); return v | nr_u16(r) << 16; }
static inline uint64_t nr_u64(net_reader *r) { uint64_t v = nr_u32(r); return v | (uint64_t)nr_u32(r) << 32; }

static inline float nr_f32(net_reader *r)
{
    uint32_t v = nr_u32(r);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Reads a string into out[cap] (always terminated); bytes past cap - 1 are dropped. */
static inline void nr_str(net_reader *r, char *out, size_t cap)
{
    uint32_t n = nr_u8(r), i;
    size_t o = 0;
    for (i = 0; i < n; i++) {
        uint32_t c = nr_u8(r);
        if (o + 1 < cap)
            out[o++] = (char)c;
    }
    if (cap)
        out[o] = 0;
}

#endif
