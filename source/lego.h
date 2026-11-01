/*
 * lego.h - LEGO Dimensions Toy Pad protocol engine (no PS4 code, testable on a PC).
 * Port of the Toy Pad emulation in RPCS3 (rpcs3/Emu/Io/Dimensions.cpp).
 * Commands arrive on interrupt OUT 0x01 as 32-byte packets (cmd in byte 2, sequence in byte 3, payload from byte 4).
 * Replies and figure events leave on interrupt IN 0x81.
 */
#ifndef LEGO_H
#define LEGO_H
#include <stdint.h>

#define LEGO_SLOTS 7      /* figure positions on the Toy Pad (game index 1..7) */
#define LEGO_SIZE 180     /* 45 pages x 4 bytes (NTAG213 dump) */
#define LEGO_PAGES 45
#define LEGO_PAD_ID 0     /* pad number reported to the game in events (assumption, see notes) */
#define LEGO_EVQ 16

typedef struct {
    uint8_t present;
    uint8_t dirty;            /* the game wrote to this figure since the last save */
    uint32_t id;              /* figure id (0 when the spot is empty) */
    uint8_t data[LEGO_SIZE];  /* last known contents; kept after removal so it can be saved */
} LegoFigure;

typedef struct {
    LegoFigure fig[LEGO_SLOTS];
    uint32_t ra, rb, rc, rd;      /* rng state */
    uint8_t ev[LEGO_EVQ][32];     /* figure added/removed events waiting for the game */
    int nev;
} LegoPad;

void lego_init(LegoPad *p);

/* one 32-byte command from the game (interrupt OUT 0x01). Writes the reply to reply[32]. Always returns 1,
 * the reply must be queued for the next IN transfer. */
int lego_command(LegoPad *p, const uint8_t *pkt, uint8_t *reply);

/* next figure event for the game (interrupt IN 0x81). Returns 1 and fills out[32] when there was one. */
int lego_pop_event(LegoPad *p, uint8_t *out);

/* figure put on slot 0..6 from a 180-byte dump. 0 = ok, -1 = slot busy, -2 = bad slot */
int lego_place(LegoPad *p, int slot, const uint8_t *data);

/* figure lifted from slot. Returns 1 if one was there. Data stays in the slot struct, so save before or after. */
int lego_remove(LegoPad *p, int slot);

/* figure id from page 36 (encrypted for characters, plain for vehicles/gadgets) */
uint32_t lego_figure_id(const uint8_t *data);

#endif
