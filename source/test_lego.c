/*
 * test_lego.c - PC self-test for lego.c (no PS4, no game needed).
 * Build: gcc -Wall -O1 -o test_lego test_lego.c && ./test_lego
 * It includes lego.c directly so the static helpers can be checked too.
 */
#include <stdio.h>
#include <stdlib.h>
#include "lego.c"

static int fails = 0;
#define CHECK(c, msg)                                   \
    do {                                                \
        if (c) printf("OK    %s\n", msg);               \
        else { printf("FAIL  %s\n", msg); fails++; }    \
    } while (0)

/* synthetic figure: uid bytes as on a real tag, the rest random */
static void make_fig(uint8_t *d)
{
    int i;
    for (i = 0; i < LEGO_SIZE; i++)
        d[i] = (uint8_t)(rand() * 31 + i);
    d[0] = 0x04; d[1] = 0x3A; d[2] = 0x7F; d[3] = 0x5A;
    d[4] = 0x12; d[5] = 0x34; d[6] = 0x56; d[7] = 0x80;
}

/* character id as the game writes it: encrypted with the figure key, into pages 36-37 */
static void set_char_id(uint8_t *d, uint32_t id)
{
    uint8_t key[16], in[8], enc[8];
    figure_key(d, key);
    wr_le(in, id);
    wr_le(in + 4, id);
    tea_enc(in, key, enc);
    memcpy(d + 144, enc, 4);
    memcpy(d + 148, enc + 4, 4);
}

int main(void)
{
    uint8_t fig[LEGO_SIZE], pkt[32], r[32], in[8], enc[8], dec[8], ev[32], nw[4] = {1, 2, 3, 4};
    LegoPad p;
    uint32_t seed = 0x12345678, conf = 0xCAFEBABE;

    srand(1);

    /* 1. figure id: character (encrypted) */
    make_fig(fig);
    set_char_id(fig, 42);
    CHECK(lego_figure_id(fig) == 42, "character id 42 decrypts from page 36");

    /* 2. figure id: vehicle (plain, large number) */
    make_fig(fig);
    wr_le(fig + 144, 5000);
    {
        uint8_t key[16], d2[8];
        uint32_t n;
        figure_key(fig, key);
        tea_dec(fig + 144, key, d2);
        n = rd_le(d2);
        printf("INFO  vehicle: decrypted value of plain page = %u (must be >= 1000 to use plain id)\n", n);
        CHECK(lego_figure_id(fig) == (n < 1000 ? n : 5000), "vehicle id read as plain when decrypt >= 1000");
    }

    /* 3. seed handshake: 0xB1 payload = [seed LE | conf BE] encrypted with the command key */
    lego_init(&p);
    wr_le(in, seed);
    wr_be(in + 4, conf);
    tea_enc(in, NULL, enc);
    memset(pkt, 0, 32);
    pkt[2] = 0xB1; pkt[3] = 7;
    memcpy(pkt + 4, enc, 8);
    lego_command(&p, pkt, r);
    CHECK(r[0] == 0x55 && r[1] == 0x09 && r[2] == 7, "seed reply header");
    tea_dec(r + 3, NULL, dec);
    CHECK(rd_be(dec) == conf, "seed reply echoes conf");
    CHECK(r[11] == checksum(r, 11), "seed reply checksum");

    /* 4. challenge: 0xB3 payload = [conf BE | 0] ; reply = [next LE | conf BE] */
    memset(in, 0, 8);
    wr_be(in, conf);
    tea_enc(in, NULL, enc);
    memset(pkt, 0, 32);
    pkt[2] = 0xB3; pkt[3] = 8;
    memcpy(pkt + 4, enc, 8);
    lego_command(&p, pkt, r);
    tea_dec(r + 3, NULL, dec);
    CHECK(rd_be(dec + 4) == conf, "challenge reply echoes conf");
    CHECK(r[2] == 8 && r[11] == checksum(r, 11), "challenge reply header and checksum");

    /* 5. place a figure: added event, then read and write its pages */
    make_fig(fig);
    set_char_id(fig, 42);
    CHECK(lego_place(&p, 0, fig) == 0, "place figure on slot 1");
    CHECK(lego_place(&p, 0, fig) == -1, "second place on same slot is refused");
    CHECK(lego_pop_event(&p, ev) && ev[0] == 0x56 && ev[1] == 0x0B && ev[4] == 1 && ev[5] == 0,
          "added event: index 1, direction added");
    CHECK(ev[13] == checksum(ev, 13), "event checksum");

    memset(pkt, 0, 32);
    pkt[2] = 0xD2; pkt[3] = 9; pkt[4] = 1; pkt[5] = 3;
    lego_command(&p, pkt, r);
    CHECK(memcmp(r + 4, fig + 12, 16) == 0, "read pages 3-6 returns the dump");

    memset(pkt, 0, 32);
    pkt[2] = 0xD3; pkt[3] = 10; pkt[4] = 1; pkt[5] = 5;
    memcpy(pkt + 6, nw, 4);
    lego_command(&p, pkt, r);
    CHECK(memcmp(p.fig[0].data + 20, nw, 4) == 0 && p.fig[0].dirty, "write page 5 stored and marked dirty");

    /* 6. model id: payload = [index | conf BE] encrypted; reply = [id LE | conf BE] */
    memset(in, 0, 8);
    in[0] = 1;
    wr_be(in + 4, 0x11223344);
    tea_enc(in, NULL, enc);
    memset(pkt, 0, 32);
    pkt[2] = 0xD4; pkt[3] = 11;
    memcpy(pkt + 4, enc, 8);
    lego_command(&p, pkt, r);
    tea_dec(r + 4, NULL, dec);
    CHECK(rd_le(dec) == 42 && rd_be(dec + 4) == 0x11223344, "model id returns 42 with conf");

    /* 7. remove: removed event, then the spot is free again */
    CHECK(lego_remove(&p, 0) == 1, "remove figure from slot 1");
    CHECK(lego_pop_event(&p, ev) && ev[4] == 1 && ev[5] == 1, "removed event: index 1, direction removed");
    CHECK(lego_remove(&p, 0) == 0, "removing an empty spot does nothing");
    CHECK(lego_place(&p, 6, fig) == 0 && lego_place(&p, 7, fig) == -2, "slot 7 is out of range");

    printf("\n%s (%d failure%s)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
