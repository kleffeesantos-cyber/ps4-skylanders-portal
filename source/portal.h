/*
 * portal.h - LEGO Dimensions Toy Pad protocol engine (no PS4 code in here, so it can be tested on a PC).
 *
 * Protocol reference: RPCS3 (rpcs3/Emu/Io/Dimensions.cpp). The file keeps the name "portal" so the rest of the
 * plugin and the Makefile do not change.
 *
 * Packets are 32 bytes. The game sends a command on interrupt endpoint 0x01:
 *     55 <len> <cmd> <seq> <payload...> <checksum>
 * and the pad answers on interrupt endpoint 0x81 with
 *     55 <len> <seq> <payload...> <checksum>
 * Tag events (a figure put on / taken off) are sent unasked, starting with 0x56:
 *     56 0b <pad> 00 <index> <dir> <uid x7> <checksum>      dir 0 = placed, 1 = removed
 * The checksum is the sum of all the bytes before it, modulo 256, and sits at byte (2 + len).
 *
 * The pad has 7 positions: slot 0 = centre pad, slots 1-3 = left pad, slots 4-6 = right pad.
 * A figure is an NTAG213 tag dump: 45 pages x 4 bytes = 180 bytes.
 */
#ifndef PORTAL_H
#define PORTAL_H
#include <stdint.h>

#define TP_SLOTS 7
#define TP_SIZE 180 /* 0x2D pages x 4 bytes */
#define TP_QMAX 64  /* reports waiting for the game */

typedef struct {
    uint8_t present;
    uint8_t dirty;
    uint32_t dirty_tick;
    uint32_t last_uid; /* first 4 bytes of the last figure that stood here */
    uint32_t id;       /* figure / vehicle id (what the 'model' command returns) */
    uint8_t data[TP_SIZE];
} TpSlot;

typedef struct {
    TpSlot slot[TP_SLOTS];
    uint8_t q[TP_QMAX][32]; /* reports for the interrupt IN endpoint, oldest first */
    uint8_t qhead, qn;
    uint32_t rng[4];        /* random generator seeded by the game ('seed' command) */
    uint8_t led[3][3];      /* colour the game asked for: [0] centre, [1] left, [2] right (shown in the web UI) */
    uint8_t woken;          /* the game sent the wake command */
    uint32_t tick;
    uint32_t cmds;          /* number of commands seen (0 = game has not talked to us yet) */
} Portal;

void portal_init(Portal *p);

/* game -> pad: one command (len 1..32). Replies are queued; fetch them with portal_next_report(). */
void portal_command(Portal *p, const uint8_t *buf, int len);

/* number of reports waiting, and pop the oldest one (returns 1 and fills out[32], or 0 if none) */
int portal_pending(const Portal *p);
int portal_next_report(Portal *p, uint8_t *out);

/* figure management. place: slot -1 = first free (prefers the slot this figure used last).
 * Returns the slot used or -1. remove: returns 1 if a figure was there. */
int portal_place(Portal *p, int slot, const uint8_t *data);
int portal_remove(Portal *p, int slot);
int portal_present(const Portal *p, int slot);

/* the pad (1 = centre, 2 = left, 3 = right) a slot belongs to */
int portal_pad_of_slot(int slot);

/* figure id from the tag dump (characters: decrypted from pages 36/37, vehicles and gadgets: page 36 as is) */
uint32_t tp_figure_id(const uint8_t *data);

/* helpers that are also useful for tests */
uint8_t tp_checksum(const uint8_t *d, int n);
void tp_tea_encrypt(const uint8_t in[8], const uint8_t *key16, uint8_t out[8]); /* key NULL = the pad's command key */
void tp_tea_decrypt(const uint8_t in[8], const uint8_t *key16, uint8_t out[8]);
void tp_make_character(uint8_t *data /* TP_SIZE bytes */, const uint8_t uid[7], uint32_t id);

#endif
