/*
 * portal.c - LEGO Dimensions Toy Pad protocol engine. See portal.h.
 * The caller is responsible for locking (every function here assumes it owns the Portal).
 * Follows RPCS3's Dimensions.cpp.
 */
#include "portal.h"
#include <string.h>

static const uint8_t COMMAND_KEY[16] = {0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
                                        0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};
static const uint8_t CHAR_CONSTANT[17] = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8, 0xD8,
                                          0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};
static const uint8_t PAD_OF_SLOT[TP_SLOTS] = {1, 2, 2, 2, 3, 3, 3};

/* ------------------------------------------------------------------ small helpers */

static uint32_t le32(const uint8_t *d) { return d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24); }
static uint32_t be32(const uint8_t *d) { return d[3] | (d[2] << 8) | (d[1] << 16) | ((uint32_t)d[0] << 24); }
static void put_le32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16);
    d[3] = (uint8_t)(v >> 24);
}
static void put_be32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)(v >> 24);
    d[1] = (uint8_t)(v >> 16);
    d[2] = (uint8_t)(v >> 8);
    d[3] = (uint8_t)v;
}
static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static uint32_t rotr32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

uint8_t tp_checksum(const uint8_t *d, int n)
{
    int sum = 0, i;
    for (i = 0; i < n; i++)
        sum += d[i];
    return (uint8_t)(sum & 0xFF);
}

/* ------------------------------------------------------------------ TEA (32 rounds, 128-bit key as 4 LE words) */

void tp_tea_encrypt(const uint8_t in[8], const uint8_t *key, uint8_t out[8])
{
    uint32_t v0 = le32(in), v1 = le32(in + 4), sum = 0;
    uint32_t k0, k1, k2, k3;
    int i;
    if (!key)
        key = COMMAND_KEY;
    k0 = le32(key);
    k1 = le32(key + 4);
    k2 = le32(key + 8);
    k3 = le32(key + 12);
    for (i = 0; i < 32; i++) {
        sum += 0x9E3779B9u;
        v0 += ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        v1 += ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
    }
    put_le32(out, v0);
    put_le32(out + 4, v1);
}

void tp_tea_decrypt(const uint8_t in[8], const uint8_t *key, uint8_t out[8])
{
    uint32_t v0 = le32(in), v1 = le32(in + 4), sum = 0xC6EF3720u;
    uint32_t k0, k1, k2, k3;
    int i;
    if (!key)
        key = COMMAND_KEY;
    k0 = le32(key);
    k1 = le32(key + 4);
    k2 = le32(key + 8);
    k3 = le32(key + 12);
    for (i = 0; i < 32; i++) {
        v1 -= ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
        v0 -= ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        sum -= 0x9E3779B9u;
    }
    put_le32(out, v0);
    put_le32(out + 4, v1);
}

/* ------------------------------------------------------------------ the game's random generator (seed / challenge) */

static void rng_init(Portal *p, uint32_t seed)
{
    int i;
    p->rng[0] = 0xF1EA5EEDu;
    p->rng[1] = seed;
    p->rng[2] = seed;
    p->rng[3] = seed;
    for (i = 0; i < 42; i++) {
        uint32_t *r = p->rng;
        uint32_t e = r[0] - rotl32(r[1], 21);
        r[0] = r[1] ^ rotl32(r[2], 19);
        r[1] = r[2] + rotl32(r[3], 6);
        r[2] = r[3] + e;
        r[3] = e + r[0];
    }
}

static uint32_t rng_next(Portal *p)
{
    uint32_t *r = p->rng;
    uint32_t e = r[0] - rotl32(r[1], 21);
    r[0] = r[1] ^ rotl32(r[2], 19);
    r[1] = r[2] + rotl32(r[3], 6);
    r[2] = r[3] + e;
    r[3] = e + r[0];
    return r[3];
}

/* ------------------------------------------------------------------ figure key / id */

static uint32_t randomize(const uint8_t *key, int count)
{
    uint32_t s = 0;
    int i;
    for (i = 0; i < count; i++)
        s = le32(key + 4 * i) + rotr32(s, 25) + rotr32(s, 10) - s;
    return s;
}

static uint32_t scramble(const uint8_t uid[7], int count)
{
    uint8_t t[24];
    memcpy(t, uid, 7);
    memcpy(t + 7, CHAR_CONSTANT, 17);
    t[count * 4 - 1] = 0xAA;
    return randomize(t, count);
}

/* the tag's UID is in bytes 0,1,2,4,5,6,7 of the dump (byte 3 is a check byte) */
static void tag_uid(const uint8_t *d, uint8_t uid[7])
{
    uid[0] = d[0];
    uid[1] = d[1];
    uid[2] = d[2];
    uid[3] = d[4];
    uid[4] = d[5];
    uid[5] = d[6];
    uid[6] = d[7];
}

static void figure_key(const uint8_t *data, uint8_t key[16])
{
    uint8_t uid[7];
    int k;
    tag_uid(data, uid);
    for (k = 0; k < 4; k++)
        put_le32(key + 4 * k, scramble(uid, 3 + k));
}

uint32_t tp_figure_id(const uint8_t *data)
{
    uint8_t key[16], dec[8];
    uint32_t num;
    figure_key(data, key);
    tp_tea_decrypt(data + 36 * 4, key, dec);
    num = le32(dec);
    if (num < 1000) /* characters: model number encrypted in pages 36 and 37 */
        return num;
    return le32(data + 36 * 4); /* vehicles and gadgets: model number in page 36 as is */
}

/* builds a character tag: uid in bytes 0,1,2,4..7, id encrypted in pages 36/37 (used by tests) */
void tp_make_character(uint8_t *data, const uint8_t uid[7], uint32_t id)
{
    uint8_t key[16], plain[8], enc[8];
    memset(data, 0, TP_SIZE);
    data[0] = uid[0];
    data[1] = uid[1];
    data[2] = uid[2];
    data[4] = uid[3];
    data[5] = uid[4];
    data[6] = uid[5];
    data[7] = uid[6];
    figure_key(data, key);
    put_le32(plain, id);
    put_le32(plain + 4, id);
    tp_tea_encrypt(plain, key, enc);
    memcpy(data + 36 * 4, enc, 8);
}

/* ------------------------------------------------------------------ report queue */

static void push(Portal *p, const uint8_t *r)
{
    if (p->qn >= TP_QMAX) { /* the game is not reading: drop the oldest */
        p->qhead = (uint8_t)((p->qhead + 1) % TP_QMAX);
        p->qn--;
    }
    memcpy(p->q[(p->qhead + p->qn) % TP_QMAX], r, 32);
    p->qn++;
}

int portal_pending(const Portal *p) { return p->qn; }

int portal_next_report(Portal *p, uint8_t *out)
{
    if (!p->qn)
        return 0;
    memcpy(out, p->q[p->qhead], 32);
    p->qhead = (uint8_t)((p->qhead + 1) % TP_QMAX);
    p->qn--;
    return 1;
}

int portal_pad_of_slot(int slot) { return (slot >= 0 && slot < TP_SLOTS) ? PAD_OF_SLOT[slot] : 0; }

/* tag event: dir 0 = placed, 1 = removed */
static void push_event(Portal *p, int slot, int dir)
{
    uint8_t r[32];
    const uint8_t *d = p->slot[slot].data;
    memset(r, 0, sizeof(r));
    r[0] = 0x56;
    r[1] = 0x0B;
    r[2] = PAD_OF_SLOT[slot];
    r[3] = 0x00;
    r[4] = (uint8_t)(slot + 1);
    r[5] = (uint8_t)dir;
    r[6] = d[0];
    r[7] = d[1];
    r[8] = d[2];
    r[9] = d[4];
    r[10] = d[5];
    r[11] = d[6];
    r[12] = d[7];
    r[13] = tp_checksum(r, 13);
    push(p, r);
}

void portal_init(Portal *p) { memset(p, 0, sizeof(*p)); }

int portal_present(const Portal *p, int slot) { return slot >= 0 && slot < TP_SLOTS && p->slot[slot].present; }

/* ------------------------------------------------------------------ commands */

static void set_led(Portal *p, int pad, const uint8_t *rgb)
{
    int i, a = 0, b = 2;
    if (pad < 0 || pad > 3)
        return;
    if (pad > 0)
        a = b = pad - 1;
    for (i = a; i <= b; i++) {
        p->led[i][0] = rgb[0];
        p->led[i][1] = rgb[1];
        p->led[i][2] = rgb[2];
    }
}

/* a reply with <n> payload bytes after the sequence byte: 55 <n+1> <seq> payload... checksum */
static void reply_blank(Portal *p, uint8_t seq)
{
    uint8_t r[32];
    memset(r, 0, sizeof(r));
    r[0] = 0x55;
    r[1] = 0x01;
    r[2] = seq;
    r[3] = tp_checksum(r, 3);
    push(p, r);
}

static void reply_encrypted(Portal *p, uint8_t seq, const uint8_t plain[8])
{
    uint8_t r[32];
    memset(r, 0, sizeof(r));
    r[0] = 0x55;
    r[1] = 0x09;
    r[2] = seq;
    tp_tea_encrypt(plain, NULL, r + 3);
    r[11] = tp_checksum(r, 11);
    push(p, r);
}

static void wake(Portal *p, uint8_t seq)
{
    /* the pad's answer to the wake command: the text "(c) LEGO 2014" */
    static const uint8_t TXT[13] = {0x28, 0x63, 0x29, 0x20, 0x4C, 0x45, 0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34};
    uint8_t r[32];
    int i;
    memset(r, 0, sizeof(r));
    r[0] = 0x55;
    r[1] = 0x0E;
    r[2] = seq;
    memcpy(r + 3, TXT, sizeof(TXT));
    r[16] = tp_checksum(r, 16);
    p->qn = 0; /* a fresh start: whatever was waiting is stale */
    p->qhead = 0;
    p->woken = 1;
    push(p, r);
    for (i = 0; i < TP_SLOTS; i++) /* figures already standing on the pad are announced after the wake reply */
        if (p->slot[i].present)
            push_event(p, i, 0);
}

void portal_command(Portal *p, const uint8_t *buf, int len)
{
    uint8_t b[32], r[32], cmd, seq;
    uint8_t plain[8], dec[8];
    int idx, page;
    if (len < 4)
        return;
    memset(b, 0, sizeof(b));
    memcpy(b, buf, len > 32 ? 32 : len);
    if (b[0] != 0x55)
        return;
    cmd = b[2];
    seq = b[3];
    p->cmds++;
    memset(r, 0, sizeof(r));

    switch (cmd) {
    case 0xB0: /* wake */
        wake(p, seq);
        break;
    case 0xB1: { /* seed: starts the random generator, answers with the confirmation value */
        uint32_t seed, conf;
        tp_tea_decrypt(b + 4, NULL, dec);
        seed = le32(dec);
        conf = be32(dec + 4);
        rng_init(p, seed);
        memset(plain, 0, sizeof(plain));
        put_be32(plain, conf);
        reply_encrypted(p, seq, plain);
        break;
    }
    case 0xB3: { /* challenge: next random number + the confirmation value */
        uint32_t conf, next;
        tp_tea_decrypt(b + 4, NULL, dec);
        conf = be32(dec);
        next = rng_next(p);
        put_le32(plain, next);
        put_be32(plain + 4, conf);
        reply_encrypted(p, seq, plain);
        break;
    }
    case 0xC0: /* colour: pad, R, G, B */
        set_led(p, b[4], b + 5);
        reply_blank(p, seq);
        break;
    case 0xC1: /* get colour */
        reply_blank(p, seq);
        break;
    case 0xC2: /* fade one pad: pad, tickTime, tickCount, R, G, B */
        set_led(p, b[4], b + 7);
        reply_blank(p, seq);
        break;
    case 0xC3: /* flash one pad: pad, tickOn, tickOff, tickCount, R, G, B */
        set_led(p, b[4], b + 8);
        reply_blank(p, seq);
        break;
    case 0xC4: /* fade random */
        reply_blank(p, seq);
        break;
    case 0xC6: /* fade all: per pad (centre, left, right) enable, tickTime, tickCount, R, G, B */
        for (idx = 0; idx < 3; idx++)
            if (b[4 + 6 * idx])
                set_led(p, idx + 1, b + 4 + 6 * idx + 3);
        reply_blank(p, seq);
        break;
    case 0xC7: /* flash all: per pad enable, tickOn, tickOff, tickCount, R, G, B */
        for (idx = 0; idx < 3; idx++)
            if (b[4 + 7 * idx])
                set_led(p, idx + 1, b + 4 + 7 * idx + 4);
        reply_blank(p, seq);
        break;
    case 0xC8: /* colour all: per pad enable, R, G, B */
        for (idx = 0; idx < 3; idx++)
            if (b[4 + 4 * idx])
                set_led(p, idx + 1, b + 4 + 4 * idx + 1);
        reply_blank(p, seq);
        break;
    case 0xD2: /* read 4 pages (16 bytes) of the figure at index b[4] (1-based), starting at page b[5] */
        idx = b[4] - 1;
        page = b[5];
        r[0] = 0x55;
        r[1] = 0x12;
        r[2] = seq;
        r[3] = 0x00;
        if (idx >= 0 && idx < TP_SLOTS && p->slot[idx].present && 4 * page < TP_SIZE - 16)
            memcpy(r + 4, p->slot[idx].data + 4 * page, 16);
        r[20] = tp_checksum(r, 20);
        push(p, r);
        break;
    case 0xD3: /* write 4 bytes to page b[5] of the figure at index b[4] */
        idx = b[4] - 1;
        page = b[5];
        r[0] = 0x55;
        r[1] = 0x02;
        r[2] = seq;
        r[3] = 0x00;
        if (idx >= 0 && idx < TP_SLOTS && p->slot[idx].present && page < TP_SIZE / 4) {
            TpSlot *s = &p->slot[idx];
            if (page == 36) /* the id is written to page 36 */
                s->id = le32(b + 6);
            memcpy(s->data + 4 * page, b + 6, 4);
            s->dirty = 1;
            s->dirty_tick = p->tick;
        }
        r[4] = tp_checksum(r, 4);
        push(p, r);
        break;
    case 0xD4: { /* model: id of the figure at index dec[0] (1-based), plus the confirmation value */
        uint32_t conf;
        tp_tea_decrypt(b + 4, NULL, dec);
        idx = dec[0] - 1;
        conf = be32(dec + 4);
        memset(plain, 0, sizeof(plain));
        if (idx >= 0 && idx < TP_SLOTS && p->slot[idx].present)
            put_le32(plain, p->slot[idx].id);
        put_be32(plain + 4, conf);
        r[0] = 0x55;
        r[1] = 0x0A;
        r[2] = seq;
        r[3] = 0x00;
        tp_tea_encrypt(plain, NULL, r + 4);
        r[12] = tp_checksum(r, 12);
        push(p, r);
        break;
    }
    default:
        /* tag list (D0), password (E1), reader on/off (E5)...: RPCS3 does not implement them either and the
         * game works with an empty report as the answer */
        push(p, r);
        break;
    }
}

/* ------------------------------------------------------------------ figures */

static uint32_t rd32(const uint8_t *d) { return le32(d); }

int portal_place(Portal *p, int slot, const uint8_t *data)
{
    uint32_t uid = rd32(data);
    TpSlot *s;
    int i;
    if (slot >= TP_SLOTS)
        return -1;
    if (slot >= 0) {
        if (p->slot[slot].present)
            return -1;
    } else {
        for (i = 0; i < TP_SLOTS && slot < 0; i++) /* the spot this figure had before */
            if (!p->slot[i].present && p->slot[i].last_uid == uid && uid)
                slot = i;
        for (i = 0; i < TP_SLOTS && slot < 0; i++)
            if (!p->slot[i].present)
                slot = i;
        if (slot < 0)
            return -1;
    }
    s = &p->slot[slot];
    memcpy(s->data, data, TP_SIZE);
    s->present = 1;
    s->id = tp_figure_id(data);
    s->last_uid = uid;
    s->dirty = 0;
    if (p->woken)
        push_event(p, slot, 0);
    return slot;
}

int portal_remove(Portal *p, int slot)
{
    if (slot < 0 || slot >= TP_SLOTS || !p->slot[slot].present)
        return 0;
    if (p->woken)
        push_event(p, slot, 1); /* uses the uid still stored in the slot */
    p->slot[slot].present = 0;
    p->slot[slot].id = 0;
    return 1;
}
