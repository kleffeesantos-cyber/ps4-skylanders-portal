/* PC test of the Toy Pad protocol engine. Build: gcc -Wall -Wextra -fsanitize=address,undefined -o test_portal test/test_portal.c source/portal.c */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../source/portal.h"

static int fails, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fails++;                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                           \
        }                                                                         \
    } while (0)

static Portal P;

static void cmd(const uint8_t *c, int n) { portal_command(&P, c, n); }

/* builds a 32-byte command: 55 len cmd seq payload checksum */
static void mk(uint8_t *out, uint8_t c, uint8_t seq, const uint8_t *payload, int n)
{
    memset(out, 0, 32);
    out[0] = 0x55;
    out[1] = (uint8_t)(n + 2);
    out[2] = c;
    out[3] = seq;
    if (n)
        memcpy(out + 4, payload, n);
    out[4 + n] = tp_checksum(out, 4 + n);
}

static int next(uint8_t *r) { return portal_next_report(&P, r); }

static int valid_reply(const uint8_t *r)
{
    return r[0] == 0x55 && r[2 + r[1]] == tp_checksum(r, 2 + r[1]);
}

int main(void)
{
    uint8_t c[32], r[32], zero8[8] = {0}, out[8], dec[8], pl[8];
    uint8_t fig[TP_SIZE], uid[7] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x80};
    int i;

    /* checksum: examples from public protocol notes */
    {
        const uint8_t led[9] = {0x55, 0x06, 0xC0, 0x05, 0x00, 0xFF, 0xFF, 0x00, 0x1E};
        CHECK(tp_checksum(led, 8) == 0x1E);
        const uint8_t off[9] = {0x55, 0x06, 0xC0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x1D};
        CHECK(tp_checksum(off, 8) == 0x1D);
    }

    /* TEA known answer: zero key, zero block -> 41ea3a0a 94baa940 (the classic TEA vector) */
    {
        uint8_t zkey[16] = {0};
        tp_tea_encrypt(zero8, zkey, out);
        CHECK(out[0] == 0x0A && out[1] == 0x3A && out[2] == 0xEA && out[3] == 0x41);
        CHECK(out[4] == 0x40 && out[5] == 0xA9 && out[6] == 0xBA && out[7] == 0x94);
        tp_tea_decrypt(out, zkey, dec);
        CHECK(memcmp(dec, zero8, 8) == 0);
    }
    /* TEA round trip with the pad's command key */
    {
        uint8_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        tp_tea_encrypt(in, NULL, out);
        CHECK(memcmp(in, out, 8) != 0);
        tp_tea_decrypt(out, NULL, dec);
        CHECK(memcmp(in, dec, 8) == 0);
    }

    portal_init(&P);

    /* a figure placed before the game woke the pad: no event yet */
    tp_make_character(fig, uid, 7);
    CHECK(tp_figure_id(fig) == 7);
    CHECK(portal_place(&P, 3, fig) == 3);
    CHECK(portal_pending(&P) == 0);
    CHECK(portal_present(&P, 3));
    CHECK(portal_pad_of_slot(0) == 1 && portal_pad_of_slot(3) == 2 && portal_pad_of_slot(4) == 3);

    /* wake: reply first, then the announcement of the figure already there */
    {
        static const uint8_t txt[13] = "(c) LEGO 2014";
        mk(c, 0xB0, 1, txt, 13);
        cmd(c, 32);
    }
    CHECK(portal_pending(&P) == 2);
    CHECK(next(r));
    CHECK(r[0] == 0x55 && r[1] == 0x0E && r[2] == 1 && memcmp(r + 3, "(c) LEGO 2014", 13) == 0 && r[16] == 0x46);
    CHECK(next(r));
    CHECK(r[0] == 0x56 && r[1] == 0x0B && r[2] == 2 && r[4] == 4 && r[5] == 0);
    CHECK(r[6] == 0x04 && r[7] == 0x11 && r[8] == 0x22 && r[9] == 0x33 && r[10] == 0x44 && r[11] == 0x55 && r[12] == 0x80);
    CHECK(r[13] == tp_checksum(r, 13));
    CHECK(!next(r));

    /* seed: the game encrypts (seed LE, confirmation BE) with the command key */
    {
        uint32_t seed = 0x12345678u, conf = 0xCAFEBABEu;
        uint8_t plain[8], enc[8];
        plain[0] = (uint8_t)seed; plain[1] = (uint8_t)(seed >> 8); plain[2] = (uint8_t)(seed >> 16); plain[3] = (uint8_t)(seed >> 24);
        plain[4] = (uint8_t)(conf >> 24); plain[5] = (uint8_t)(conf >> 16); plain[6] = (uint8_t)(conf >> 8); plain[7] = (uint8_t)conf;
        tp_tea_encrypt(plain, NULL, enc);
        mk(c, 0xB1, 2, enc, 8);
        cmd(c, 32);
        CHECK(next(r));
        CHECK(valid_reply(r) && r[1] == 0x09 && r[2] == 2);
        tp_tea_decrypt(r + 3, NULL, dec);
        CHECK(dec[0] == 0xCA && dec[1] == 0xFE && dec[2] == 0xBA && dec[3] == 0xBE); /* confirmation, big endian */
        CHECK(dec[4] == 0 && dec[5] == 0 && dec[6] == 0 && dec[7] == 0);

        /* challenge: reply holds the next random number (LE) and the confirmation (BE) */
        {
            uint8_t enc2[8], first[4];
            Portal q;
            uint32_t conf2 = 0x01020304u;
            plain[0] = (uint8_t)(conf2 >> 24); plain[1] = (uint8_t)(conf2 >> 16); plain[2] = (uint8_t)(conf2 >> 8); plain[3] = (uint8_t)conf2;
            plain[4] = plain[5] = plain[6] = plain[7] = 0;
            tp_tea_encrypt(plain, NULL, enc2);
            mk(c, 0xB3, 3, enc2, 8);
            cmd(c, 32);
            CHECK(next(r));
            CHECK(valid_reply(r) && r[1] == 0x09 && r[2] == 3);
            tp_tea_decrypt(r + 3, NULL, dec);
            CHECK(dec[4] == 0x01 && dec[5] == 0x02 && dec[6] == 0x03 && dec[7] == 0x04);
            memcpy(first, dec, 4);
            /* the sequence is deterministic: a second pad seeded the same gives the same numbers, and the next differs */
            portal_init(&q);
            mk(c, 0xB1, 2, enc, 8);
            portal_command(&q, c, 32);
            portal_next_report(&q, r);
            mk(c, 0xB3, 3, enc2, 8);
            portal_command(&q, c, 32);
            portal_next_report(&q, r);
            tp_tea_decrypt(r + 3, NULL, dec);
            CHECK(memcmp(first, dec, 4) == 0);
            portal_command(&q, c, 32);
            portal_next_report(&q, r);
            tp_tea_decrypt(r + 3, NULL, dec);
            CHECK(memcmp(first, dec, 4) != 0);
        }
    }

    /* LED commands are acknowledged and remembered */
    {
        uint8_t pa[4] = {2, 10, 20, 30}; /* left pad */
        mk(c, 0xC0, 4, pa, 4);
        cmd(c, 32);
        CHECK(next(r) && r[0] == 0x55 && r[1] == 0x01 && r[2] == 4 && r[3] == tp_checksum(r, 3));
        CHECK(P.led[1][0] == 10 && P.led[1][1] == 20 && P.led[1][2] == 30 && P.led[0][0] == 0);
        {
            uint8_t all[12] = {1, 1, 2, 3, 0, 9, 9, 9, 1, 7, 8, 9}; /* centre on, left off, right on */
            mk(c, 0xC8, 5, all, 12);
            cmd(c, 32);
            CHECK(next(r));
            CHECK(P.led[0][0] == 1 && P.led[0][2] == 3 && P.led[1][0] == 10 && P.led[2][1] == 8);
        }
    }

    /* read 4 pages from the figure (index 4 = slot 3), page 36 holds the encrypted id */
    {
        uint8_t pa[2] = {4, 36};
        mk(c, 0xD2, 6, pa, 2);
        cmd(c, 32);
        CHECK(next(r));
        CHECK(valid_reply(r) && r[1] == 0x12 && r[2] == 6 && r[3] == 0);
        CHECK(memcmp(r + 4, fig + 36 * 4, 16) == 0);
        pa[1] = 0;
        mk(c, 0xD2, 7, pa, 2);
        cmd(c, 32);
        CHECK(next(r) && memcmp(r + 4, fig, 16) == 0);
        pa[0] = 5; /* nothing there: zeros */
        mk(c, 0xD2, 8, pa, 2);
        cmd(c, 32);
        CHECK(next(r) && valid_reply(r));
        for (i = 4; i < 20; i++)
            CHECK(r[i] == 0);
    }

    /* write: page 8 gets 4 bytes, the slot is marked dirty; writing page 36 changes the id */
    {
        uint8_t pa[6] = {4, 8, 0xDE, 0xAD, 0xBE, 0xEF};
        P.tick = 77;
        mk(c, 0xD3, 9, pa, 6);
        cmd(c, 32);
        CHECK(next(r));
        CHECK(valid_reply(r) && r[1] == 0x02 && r[2] == 9 && r[3] == 0);
        CHECK(memcmp(P.slot[3].data + 32, pa + 2, 4) == 0);
        CHECK(P.slot[3].dirty == 1 && P.slot[3].dirty_tick == 77);
        pa[1] = 36;
        pa[2] = 0x63; pa[3] = 0x04; pa[4] = 0; pa[5] = 0;
        mk(c, 0xD3, 10, pa, 6);
        cmd(c, 32);
        CHECK(next(r));
        CHECK(P.slot[3].id == 0x0463);
        pa[1] = 45; /* beyond the tag: ignored */
        mk(c, 0xD3, 11, pa, 6);
        cmd(c, 32);
        CHECK(next(r) && valid_reply(r));
    }

    /* model: id of the figure at index 4 (confirmation echoed back) */
    {
        uint8_t plain[8] = {4, 0, 0, 0, 0xA1, 0xB2, 0xC3, 0xD4}, enc[8];
        tp_tea_encrypt(plain, NULL, enc);
        mk(c, 0xD4, 12, enc, 8);
        cmd(c, 32);
        CHECK(next(r));
        CHECK(valid_reply(r) && r[1] == 0x0A && r[2] == 12 && r[3] == 0);
        tp_tea_decrypt(r + 4, NULL, dec);
        CHECK(dec[0] == 0x63 && dec[1] == 0x04 && dec[2] == 0 && dec[3] == 0);
        CHECK(dec[4] == 0xA1 && dec[5] == 0xB2 && dec[6] == 0xC3 && dec[7] == 0xD4);
        (void)pl;
    }

    /* unimplemented commands get an empty report, like RPCS3 */
    mk(c, 0xD0, 13, NULL, 0);
    cmd(c, 32);
    CHECK(next(r) && r[0] == 0);

    /* remove / place events */
    CHECK(portal_remove(&P, 3) == 1);
    CHECK(!portal_present(&P, 3));
    CHECK(next(r));
    CHECK(r[0] == 0x56 && r[2] == 2 && r[4] == 4 && r[5] == 1 && r[6] == 0x04 && r[12] == 0x80 && r[13] == tp_checksum(r, 13));
    CHECK(portal_remove(&P, 3) == 0);
    CHECK(portal_place(&P, 0, fig) == 0);
    CHECK(next(r) && r[2] == 1 && r[4] == 1 && r[5] == 0);
    CHECK(portal_place(&P, 0, fig) == -1); /* occupied */
    CHECK(portal_place(&P, 7, fig) == -1); /* no such slot */
    CHECK(portal_remove(&P, 0) == 1);
    next(r);
    /* auto slot: the figure goes back where it was */
    CHECK(portal_place(&P, -1, fig) == 0);
    CHECK(portal_remove(&P, 0) == 1);
    {
        uint8_t other[TP_SIZE];
        uint8_t uid2[7] = {0x04, 0x99, 0x88, 0x77, 0x66, 0x55, 0x80};
        tp_make_character(other, uid2, 9);
        CHECK(portal_place(&P, -1, other) == 0); /* a new figure takes the first free slot */
        CHECK(portal_place(&P, -1, fig) == 3);   /* it goes back to the slot it used before (slot 3) */
        CHECK(portal_remove(&P, 0) == 1);
        CHECK(portal_remove(&P, 3) == 1);
        CHECK(portal_place(&P, 0, other) == 0);  /* put back for the wake test below */
    }

    /* a second wake re-announces the figures standing on the pad and drops stale reports */
    {
        static const uint8_t txt[13] = "(c) LEGO 2014";
        mk(c, 0xB0, 1, txt, 13);
        cmd(c, 32);
        CHECK(portal_pending(&P) == 2);
        CHECK(next(r) && r[0] == 0x55);
        CHECK(next(r) && r[0] == 0x56 && r[5] == 0);
    }

    /* the queue never overflows: oldest reports are dropped */
    mk(c, 0xD0, 14, NULL, 0);
    for (i = 0; i < 200; i++)
        portal_command(&P, c, 32);
    CHECK(portal_pending(&P) == TP_QMAX);
    while (next(r)) {}
    CHECK(portal_pending(&P) == 0);

    /* short or foreign packets are ignored */
    portal_command(&P, c, 2);
    c[0] = 'A';
    portal_command(&P, c, 32);
    CHECK(portal_pending(&P) == 0);

    /* vehicles: model number is the raw page 36 */
    {
        uint8_t veh[TP_SIZE];
        memset(veh, 0, sizeof(veh));
        veh[0] = 4; veh[1] = 1; veh[2] = 2; veh[4] = 3; veh[5] = 4; veh[6] = 5; veh[7] = 0x80;
        veh[144] = 0x63; veh[145] = 0x04;
        CHECK(tp_figure_id(veh) == 0x0463);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
