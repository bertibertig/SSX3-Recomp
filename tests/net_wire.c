/*
 * tests/net_wire.c -- the LAN packet writer/reader (src/net/net_wire.h).
 *
 * Checks the little-endian byte order, a full round trip of every field type,
 * string truncation on both sides, and that running past the end of a buffer
 * sets `bad` and reads zeros instead of out-of-bounds bytes.
 * Run through ctest (tests/CMakeLists.txt).
 */
#include <stdio.h>
#include <string.h>
#include "net_wire.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void byte_order(void)
{
    uint8_t buf[16];
    net_writer w;
    float one = 1.0f;

    nw_init(&w, buf, sizeof buf);
    nw_u16(&w, 0x1234);
    nw_u32(&w, 0xA1B2C3D4u);
    nw_f32(&w, one);
    CHECK(!w.bad && w.len == 10);
    CHECK(buf[0] == 0x34 && buf[1] == 0x12);
    CHECK(buf[2] == 0xD4 && buf[3] == 0xC3 && buf[4] == 0xB2 && buf[5] == 0xA1);
    CHECK(buf[6] == 0x00 && buf[7] == 0x00 && buf[8] == 0x80 && buf[9] == 0x3F);   /* 1.0f = 0x3F800000 */
}

static void round_trip(void)
{
    uint8_t buf[64];
    char s[16];
    net_writer w;
    net_reader r;

    nw_init(&w, buf, sizeof buf);
    nw_u8(&w, 0xAB);
    nw_u16(&w, 0xBEEF);
    nw_u32(&w, 0xDEADBEEFu);
    nw_u64(&w, 0x0123456789ABCDEFull);
    nw_f32(&w, -2.5f);
    nw_str(&w, "Zoe", 15);
    nw_str(&w, NULL, 15);
    CHECK(!w.bad);

    nr_init(&r, buf, w.len);
    CHECK(nr_u8(&r) == 0xAB);
    CHECK(nr_u16(&r) == 0xBEEF);
    CHECK(nr_u32(&r) == 0xDEADBEEFu);
    CHECK(nr_u64(&r) == 0x0123456789ABCDEFull);
    CHECK(nr_f32(&r) == -2.5f);
    nr_str(&r, s, sizeof s);
    CHECK(!strcmp(s, "Zoe"));
    nr_str(&r, s, sizeof s);
    CHECK(s[0] == 0);
    CHECK(!r.bad && r.pos == w.len);
}

static void strings_are_cut(void)
{
    uint8_t buf[300];
    char big[300], s[4];
    net_writer w;
    net_reader r;

    /* Writer: cut at `max`, and never more than the u8 length allows. */
    nw_init(&w, buf, sizeof buf);
    nw_str(&w, "Mackenzie", 3);
    CHECK(w.len == 4 && buf[0] == 3 && !memcmp(buf + 1, "Mac", 3));

    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    nw_init(&w, buf, sizeof buf);
    nw_str(&w, big, sizeof big);
    CHECK(!w.bad && buf[0] == 255 && w.len == 256);

    /* Reader: a string longer than the output keeps the stream in step. */
    nw_init(&w, buf, sizeof buf);
    nw_str(&w, "Griff", 15);
    nw_u8(&w, 0x5A);
    nr_init(&r, buf, w.len);
    nr_str(&r, s, sizeof s);
    CHECK(!strcmp(s, "Gri"));
    CHECK(nr_u8(&r) == 0x5A && !r.bad);
}

static void overruns(void)
{
    uint8_t buf[3] = { 0x11, 0x22, 0x33 };
    char s[8];
    net_writer w;
    net_reader r;

    /* Writer: a field that does not fit sets bad and writes nothing past cap. */
    uint8_t out[4] = { 0, 0, 0, 0xEE };
    nw_init(&w, out, 3);
    nw_u32(&w, 0xFFFFFFFFu);
    CHECK(w.bad && w.len <= 3 && out[3] == 0xEE);

    /* Reader: a truncated u32 reads as zeros past the end and flags the packet. */
    nr_init(&r, buf, sizeof buf);
    CHECK(nr_u16(&r) == 0x2211 && !r.bad);
    CHECK(nr_u32(&r) == 0x33 && r.bad);

    /* A string whose length byte promises more than there is. */
    buf[0] = 200;
    nr_init(&r, buf, sizeof buf);
    nr_str(&r, s, sizeof s);
    CHECK(r.bad && strlen(s) < sizeof s);

    /* An empty packet. */
    nr_init(&r, buf, 0);
    CHECK(nr_u8(&r) == 0 && r.bad);
}

int main(void)
{
    byte_order();
    round_trip();
    strings_are_cut();
    overruns();
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
