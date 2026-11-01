/*
 * lego.c - LEGO Dimensions Toy Pad protocol engine. See lego.h.
 * Ported from RPCS3 Emu/Io/Dimensions.cpp. The caller handles locking (every function assumes it owns the pad).
 */
#include "lego.h"
#include <string.h>

static const uint8_t COMMAND_KEY[16] = {0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
                                        0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};
static const uint8_t CHAR_CONSTANT[17] = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C,
                                          0xA8, 0xD8, 0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};

/* ------------------------------------------------------------ byte helpers */

static uint32_t rd_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static uint8_t checksum(const uint8_t *d, int n)
{
    int s = 0, i;
    for (i = 0; i < n; i++)
        s += d[i];
    return (uint8_t)s;
}

/* ------------------------------------------------------------ TEA-style cipher (8 bytes, 128-bit key) */

static void tea_dec(const uint8_t *in, const uint8_t *key, uint8_t *out)
{
    const uint8_t *k = key ? key : COMMAND_KEY;
    uint32_t v0 = rd_le(in), v1 = rd_le(in + 4);
    uint32_t k0 = rd_le(k), k1 = rd_le(k + 4), k2 = rd_le(k + 8), k3 = rd_le(k + 12);
    uint32_t sum = 0xC6EF3720, i;
    for (i = 0; i < 32; i++) {
        v1 -= ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
        v0 -= ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        sum -= 0x9E3779B9;
    }
    wr_le(out, v0);
    wr_le(out + 4, v1);
}

static void tea_enc(const uint8_t *in, const uint8_t *key, uint8_t *out)
{
    const uint8_t *k = key ? key : COMMAND_KEY;
    uint32_t v0 = rd_le(in), v1 = rd_le(in + 4);
    uint32_t k0 = rd_le(k), k1 = rd_le(k + 4), k2 = rd_le(k + 8), k3 = rd_le(k + 12);
    uint32_t sum = 0, i;
    for (i = 0; i < 32; i++) {
        sum += 0x9E3779B9;
        v0 += ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        v1 += ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
    }
    wr_le(out, v0);
    wr_le(out + 4, v1);
}

/* ------------------------------------------------------------ rng used by the seed / challenge handshake */

static uint32_t rng_next(LegoPad *p)
{
    uint32_t e = p->ra - rotl(p->rb, 21);
    p->ra = p->rb ^ rotl(p->rc, 19);
    p->rb = p->rc + rotl(p->rd, 6);
    p->rc = p->rd + e;
    p->rd = e + p->ra;
    return p->rd;
}

static void rng_seed(LegoPad *p, uint32_t seed)
{
    int i;
    p->ra = 0xF1EA5EED;
    p->rb = seed;
    p->rc = seed;
    p->rd = seed;
    for (i = 0; i < 42; i++)
        rng_next(p);
}

/* ------------------------------------------------------------ replies */

static void blank_reply(uint8_t *r, uint8_t type, uint8_t seq)
{
    memset(r, 0, 32);
    r[0] = 0x55;
    r[1] = type;
    r[2] = seq;
    r[3] = checksum(r, 3);
}

static void enc_reply(uint8_t *r, uint8_t seq, const uint8_t *enc8)
{
    memset(r, 0, 32);
    r[0] = 0x55;
    r[1] = 0x09;
    r[2] = seq;
    memcpy(r + 3, enc8, 8);
    r[11] = checksum(r, 11);
}

/* ------------------------------------------------------------ figure key and id (page 36) */

static uint32_t randomize(const uint8_t *k, int count)
{
    uint32_t s = 0;
    int i;
    for (i = 0; i < count; i++) {
        uint32_t v4 = rotr(s, 25), v5 = rotr(s, 10), b = rd_le(k + i * 4);
        s = b + v4 + v5 - s;
    }
    return s;
}

static uint32_t scramble(const uint8_t *uid, int count)
{
    uint8_t k[24], t[4];
    memcpy(k, uid, 7);
    memcpy(k + 7, CHAR_CONSTANT, 17);
    k[count * 4 - 1] = 0xAA;
    wr_le(t, randomize(k, count));
    return rd_be(t);
}

static void figure_key(const uint8_t *d, uint8_t key[16])
{
    uint8_t uid[7] = {d[0], d[1], d[2], d[4], d[5], d[6], d[7]};
    wr_be(key, scramble(uid, 3));
    wr_be(key + 4, scramble(uid, 4));
    wr_be(key + 8, scramble(uid, 5));
    wr_be(key + 12, scramble(uid, 6));
}

uint32_t lego_figure_id(const uint8_t *d)
{
    uint8_t key[16], dec[8];
    uint32_t n;
    figure_key(d, key);
    tea_dec(d + 36 * 4, key, dec);
    n = rd_le(dec);
    /* characters: encrypted id. vehicles/gadgets: plain little-endian id */
    if (n < 1000)
        return n;
    return rd_le(d + 36 * 4);
}

/* ------------------------------------------------------------ game commands */

static void read_block(LegoPad *p, uint8_t index, uint8_t page, uint8_t seq, uint8_t *r)
{
    memset(r, 0, 32);
    r[0] = 0x55;
    r[1] = 0x12;
    r[2] = seq;
    r[3] = 0;
    if ((uint8_t)(index - 1) < LEGO_SLOTS) { /* game index starts at 1 */
        LegoFigure *f = &p->fig[index - 1];
        if (f->present && 4 * (int)page < LEGO_SIZE - 16)
            memcpy(r + 4, f->data + 4 * page, 16);
    }
    r[20] = checksum(r, 20);
}

static void write_block(LegoPad *p, uint8_t index, uint8_t page, const uint8_t *d, uint8_t seq, uint8_t *r)
{
    memset(r, 0, 32);
    r[0] = 0x55;
    r[1] = 0x02;
    r[2] = seq;
    r[3] = 0;
    if ((uint8_t)(index - 1) < LEGO_SLOTS) {
        LegoFigure *f = &p->fig[index - 1];
        if (f->present && page < LEGO_PAGES) {
            if (page == 36)
                f->id = rd_le(d);
            memcpy(f->data + 4 * page, d, 4);
            f->dirty = 1; /* the caller saves it */
        }
    }
    r[4] = checksum(r, 4);
}

static void get_model(LegoPad *p, const uint8_t *pl, uint8_t seq, uint8_t *r)
{
    uint8_t v[8], in[8], enc[8];
    memset(in, 0, sizeof(in));
    tea_dec(pl, NULL, v);
    if ((uint8_t)(v[0] - 1) < LEGO_SLOTS)
        wr_le(in, p->fig[v[0] - 1].id);
    wr_be(in + 4, rd_be(v + 4));
    tea_enc(in, NULL, enc);
    memset(r, 0, 32);
    r[0] = 0x55;
    r[1] = 0x0A;
    r[2] = seq;
    r[3] = 0;
    memcpy(r + 4, enc, 8);
    r[12] = checksum(r, 12);
}

int lego_command(LegoPad *p, const uint8_t *pkt, uint8_t *r)
{
    static const uint8_t WAKE[17] = {0x55, 0x0E, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4C, 0x45,
                                     0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34, 0x46};
    uint8_t cmd = pkt[2], seq = pkt[3];
    const uint8_t *pl = pkt + 4;
    uint8_t v[8], in[8], enc[8];

    memset(r, 0, 32);
    switch (cmd) {
    case 0xB0: /* wake */
        memcpy(r, WAKE, sizeof(WAKE));
        break;
    case 0xB1: /* seed: start the rng, confirm with the decrypted conf */
        tea_dec(pl, NULL, v);
        rng_seed(p, rd_le(v));
        memset(in, 0, sizeof(in));
        wr_be(in, rd_be(v + 4));
        tea_enc(in, NULL, enc);
        enc_reply(r, seq, enc);
        break;
    case 0xB3: /* challenge: next rng value + conf */
        tea_dec(pl, NULL, v);
        wr_le(in, rng_next(p));
        wr_be(in + 4, rd_be(v));
        tea_enc(in, NULL, enc);
        enc_reply(r, seq, enc);
        break;
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC6: case 0xC7: case 0xC8: /* LED colour / fade / flash: just acknowledge */
        blank_reply(r, 0x01, seq);
        break;
    case 0xD2: /* read 4 pages */
        read_block(p, pl[0], pl[1], seq, r);
        break;
    case 0xD3: /* write 1 page */
        write_block(p, pl[0], pl[1], pl + 2, seq, r);
        break;
    case 0xD4: /* model id */
        get_model(p, pl, seq, r);
        break;
    default:   /* unknown: the game gets an empty reply, as RPCS3 does */
        break;
    }
    return 1;
}

/* ------------------------------------------------------------ figure events and slots */

static void push_event(LegoPad *p, const uint8_t *ev)
{
    if (p->nev < LEGO_EVQ)
        memcpy(p->ev[p->nev++], ev, 32);
}

static void uid_event(uint8_t *ev, const uint8_t *d, int slot, int removed)
{
    memset(ev, 0, 32);
    ev[0] = 0x56;
    ev[1] = 0x0B;
    ev[2] = LEGO_PAD_ID;
    ev[3] = 0;
    ev[4] = (uint8_t)(slot + 1);
    ev[5] = removed ? 1 : 0;
    ev[6] = d[0];
    ev[7] = d[1];
    ev[8] = d[2];
    ev[9] = d[4];
    ev[10] = d[5];
    ev[11] = d[6];
    ev[12] = d[7];
    ev[13] = checksum(ev, 13);
}

void lego_init(LegoPad *p)
{
    memset(p, 0, sizeof(*p));
}

int lego_pop_event(LegoPad *p, uint8_t *out)
{
    if (p->nev <= 0)
        return 0;
    memcpy(out, p->ev[0], 32);
    memmove(p->ev[0], p->ev[1], 32 * (size_t)(p->nev - 1));
    p->nev--;
    return 1;
}

int lego_place(LegoPad *p, int slot, const uint8_t *data)
{
    LegoFigure *f;
    uint8_t ev[32];
    if (slot < 0 || slot >= LEGO_SLOTS)
        return -2;
    f = &p->fig[slot];
    if (f->present)
        return -1;
    memcpy(f->data, data, LEGO_SIZE);
    f->id = lego_figure_id(data);
    f->present = 1;
    f->dirty = 0;
    uid_event(ev, data, slot, 0);
    push_event(p, ev);
    return 0;
}

int lego_remove(LegoPad *p, int slot)
{
    LegoFigure *f;
    uint8_t ev[32];
    if (slot < 0 || slot >= LEGO_SLOTS)
        return 0;
    f = &p->fig[slot];
    if (!f->present)
        return 0;
    uid_event(ev, f->data, slot, 1);
    push_event(p, ev);
    f->present = 0;
    f->id = 0;
    return 1;
}
