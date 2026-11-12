/*
 * host_sim.c - runs the whole plugin (source/main.c) on a Linux PC with a fake game.
 *
 * The PS4 kernel calls are replaced by POSIX ones (below), then main.c is included so the test can call its
 * internal functions. The "game" is the code in main(): it submits libusb-style transfers exactly like the real
 * game would (commands on endpoint 0x01, reads on 0x81) and checks what comes back.
 *
 * Build and run with test/run_host_sim.sh. Pass "serve" as the 1st argument to keep the web server up afterwards.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "orbis/libkernel.h"

/* ---- kernel calls on a PC ---- */
int sceKernelUsleep(unsigned us) { return usleep(us); }
int sceKernelOpen(const char *path, int flags, int mode)
{
    int f = (flags & 0x1) ? O_WRONLY : O_RDONLY; /* FreeBSD flag values -> Linux */
    if (flags & 0x200) f |= O_CREAT;
    if (flags & 0x400) f |= O_TRUNC;
    if (flags & 0x20000) f |= O_DIRECTORY;
    return open(path, f, mode);
}
long sceKernelRead(int fd, void *buf, size_t n) { return read(fd, buf, n); }
long sceKernelWrite(int fd, const void *buf, size_t n) { return write(fd, buf, n); }
int sceKernelClose(int fd) { return close(fd); }
int sceKernelMkdir(const char *path, int mode) { return mkdir(path, mode); }
int sceKernelUnlink(const char *path) { return unlink(path); }
int sceKernelVirtualQuery(const void *a, int f, void *info, size_t n) { (void)a; (void)f; (void)info; (void)n; return -1; }
int sceKernelMprotect(const void *a, size_t n, int prot) { (void)a; (void)n; (void)prot; return -1; }
int sceKernelDlsym(int32_t h, const char *name, void **addr) { (void)h; (void)name; *addr = NULL; return -1; }
/* one fake module covering all addresses, so every callback counts as "game code" */
int sceKernelGetModuleList(OrbisKernelModule *h, size_t max, size_t *n) { (void)max; h[0] = 1; *n = 1; return 0; }
int sceKernelGetModuleInfo(OrbisKernelModule h, OrbisKernelModuleInfo *info)
{
    (void)h;
    strcpy(info->name, "fake_game");
    info->segmentCount = 1;
    info->segmentInfo[0].address = (void *)0x10000;
    info->segmentInfo[0].size = 0x7ffe00000000ULL;
    info->segmentInfo[0].prot = 5;
    return 0;
}
int scePthreadAttrInit(OrbisPthreadAttr *a) { return pthread_attr_init(a); }
int scePthreadAttrSetstacksize(OrbisPthreadAttr *a, size_t n) { return pthread_attr_setstacksize(a, n); }
int scePthreadAttrSetdetachstate(OrbisPthreadAttr *a, int s) { return pthread_attr_setdetachstate(a, s ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE); }
int scePthreadAttrDestroy(OrbisPthreadAttr *a) { return pthread_attr_destroy(a); }
int scePthreadCreate(OrbisPthread *t, const OrbisPthreadAttr *a, void *(*fn)(void *), void *arg, const char *name)
{
    (void)name;
    return pthread_create(t, a, fn, arg);
}
/* the PS4 returns directory entries in the FreeBSD layout: fileno u32, reclen u16, type u8, namlen u8, name */
int sceKernelGetdents(int fd, char *buf, int n)
{
    static DIR *dirs[64];
    DIR *d;
    struct dirent *e;
    int off = 0;
    if (fd < 0 || fd >= 64) return -1;
    if (!dirs[fd]) dirs[fd] = fdopendir(dup(fd));
    d = dirs[fd];
    if (!d) return -1;
    while ((e = readdir(d))) {
        int nl = (int)strlen(e->d_name), rl = (8 + nl + 1 + 7) & ~7;
        if (off + rl > n) { seekdir(d, telldir(d)); break; }
        *(uint32_t *)(buf + off) = (uint32_t)e->d_ino;
        *(uint16_t *)(buf + off + 4) = (uint16_t)rl;
        buf[off + 6] = e->d_type == DT_DIR ? 4 : e->d_type == DT_REG ? 8 : 0;
        buf[off + 7] = (char)nl;
        memcpy(buf + off + 8, e->d_name, nl + 1);
        off += rl;
    }
    if (off == 0) { closedir(d); dirs[fd] = NULL; }
    return off;
}

#include "main.c" /* the copy with the host paths, see run_host_sim.sh */

/* ---- the fake game ---- */
static int fails, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fails++;                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                           \
        }                                                                         \
    } while (0)

typedef struct { uint8_t raw[128]; } Xfer;
static Xfer xfer_pool[16] __attribute__((aligned(16)));
static uint8_t xfer_buf[16][32];
static volatile int cb_done[16];
static int nxfer;
static uint8_t SEQ;

static uint64_t on_done(uint64_t t)
{
    int i;
    for (i = 0; i < 16; i++)
        if ((uint64_t)&xfer_pool[i] == t) cb_done[i] = 1;
    return 0;
}

/* submits a transfer the way the game's sceUsbdSubmitTransfer does (libusb_transfer layout) */
static int submit(uint8_t ep, const uint8_t *data)
{
    int i = nxfer++ % 16;
    uint8_t *t = xfer_pool[i].raw;
    memset(t, 0, 128);
    memset(xfer_buf[i], 0, 32);
    if (data) memcpy(xfer_buf[i], data, 32);
    *(uint64_t *)t = FAKE_HANDLE;
    t[9] = ep;
    t[10] = 3; /* interrupt */
    *(int32_t *)(t + 20) = 32;
    *(uint64_t *)(t + 32) = (uint64_t)on_done;
    *(uint64_t *)(t + 48) = (uint64_t)xfer_buf[i];
    cb_done[i] = 0;
    handle_submit((uint64_t)t);
    return i;
}
static int wait_done(int i, int ms)
{
    while (!cb_done[i] && ms-- > 0) usleep(1000);
    return cb_done[i];
}
static void mkcmd(uint8_t *c, uint8_t cmd, const uint8_t *pl, int n)
{
    memset(c, 0, 32);
    c[0] = 0x55; c[1] = (uint8_t)(n + 2); c[2] = cmd; c[3] = ++SEQ;
    if (n) memcpy(c + 4, pl, n);
    c[4 + n] = tp_checksum(c, 4 + n);
}
/* command out on endpoint 0x01, reply in on 0x81 */
static void command(uint8_t cmd, const uint8_t *pl, int n, uint8_t *reply)
{
    uint8_t c[32];
    int o, i;
    mkcmd(c, cmd, pl, n);
    o = submit(0x01, c);
    i = submit(0x81, NULL);
    CHECK(wait_done(o, 2000));
    CHECK(wait_done(i, 2000));
    if (reply) memcpy(reply, xfer_buf[i], 32);
}
static int next_event(uint8_t *ev)
{
    int i = submit(0x81, NULL), ok = wait_done(i, 2000);
    memcpy(ev, xfer_buf[i], 32);
    return ok;
}
static void put_be(uint8_t *d, uint32_t v) { d[0] = v >> 24; d[1] = v >> 16; d[2] = v >> 8; d[3] = v; }

int main(int argc, char **argv)
{
    uint8_t r[32], d[8], pl[16], enc[8], fig[TP_SIZE], veh[TP_SIZE], back[TP_SIZE];
    uint8_t uid[7] = {0x04, 0xA1, 0x9E, 0xD2, 0xE3, 0x40, 0x80};
    char path[600];
    int i, n;

    /* two tag dumps in the figures folder: a character (in the top folder) and a vehicle (in a sub-folder) */
    sceKernelMkdir(DIR_MAIN, 0777);
    sceKernelMkdir(DIR_FIGS, 0777);
    sceKernelMkdir(DIR_FIGS "/Vehicles", 0777);
    tp_make_character(fig, uid, 1);
    memset(veh, 0, sizeof(veh));
    veh[0] = 4; veh[1] = 7; veh[2] = 8; veh[4] = 9; veh[5] = 10; veh[6] = 11; veh[7] = 0x80;
    veh[144] = 0x63; veh[145] = 0x04;
    write_file(DIR_FIGS "/Batman.bin", fig, TP_SIZE);
    write_file(DIR_FIGS "/Vehicles/Batmobile.bin", veh, TP_SIZE);
    write_file(DIR_FIGS "/Vehicles/broken.bin", veh, 100); /* wrong size: must be refused */

    plugin_load(0, NULL);
    for (i = 0; i < 300 && g_lib_n < 3; i++) usleep(10000);
    CHECK(g_lib_n == 3);
    CHECK(strcmp(lib_rel(0), "Batman.bin") == 0);
    CHECK(strcmp(lib_rel(1), "Vehicles/Batmobile.bin") == 0);
    CHECK(place_figure(2, 2) == -2); /* 100-byte file */

    /* a figure put on the pad before the game wakes it: nothing is sent yet */
    CHECK(place_figure(3, 0) == 3);
    usleep(50000);
    CHECK(portal_pending(&g_portal) == 0);

    /* wake: the reply, then the figure on the pad is announced */
    command(0xB0, (const uint8_t *)"(c) LEGO 2014", 13, r);
    CHECK(r[0] == 0x55 && r[1] == 0x0E && memcmp(r + 3, "(c) LEGO 2014", 13) == 0 && r[16] == 0x46);
    CHECK(next_event(r));
    CHECK(r[0] == 0x56 && r[1] == 0x0B && r[2] == 2 && r[4] == 4 && r[5] == 0);
    CHECK(r[6] == 0x04 && r[7] == 0xA1 && r[8] == 0x9E && r[9] == 0xD2 && r[10] == 0xE3 && r[11] == 0x40 && r[12] == 0x80);
    CHECK(g_game_seen);

    /* seed + challenge */
    memset(d, 0, 8);
    d[0] = 0x78; d[1] = 0x56; d[2] = 0x34; d[3] = 0x12;
    put_be(d + 4, 0xCAFEBABEu);
    tp_tea_encrypt(d, NULL, enc);
    command(0xB1, enc, 8, r);
    tp_tea_decrypt(r + 3, NULL, d);
    CHECK(r[0] == 0x55 && r[1] == 0x09 && d[0] == 0xCA && d[3] == 0xBE);
    memset(d, 0, 8);
    put_be(d, 0x01020304u);
    tp_tea_encrypt(d, NULL, enc);
    command(0xB3, enc, 8, r);
    tp_tea_decrypt(r + 3, NULL, d);
    CHECK(r[0] == 0x55 && r[1] == 0x09 && d[4] == 1 && d[7] == 4);

    /* read the id pages of the figure on index 4, and ask for its model */
    pl[0] = 4; pl[1] = 36;
    command(0xD2, pl, 2, r);
    CHECK(r[0] == 0x55 && r[1] == 0x12 && memcmp(r + 4, fig + 144, 16) == 0);
    memset(d, 0, 8);
    d[0] = 4;
    put_be(d + 4, 0x11223344u);
    tp_tea_encrypt(d, NULL, enc);
    command(0xD4, enc, 8, r);
    tp_tea_decrypt(r + 4, NULL, d);
    CHECK(r[1] == 0x0A && d[0] == 1 && d[4] == 0x11 && d[7] == 0x44);

    /* LEDs */
    pl[0] = 2; pl[1] = 255; pl[2] = 128; pl[3] = 0;
    command(0xC0, pl, 4, r);
    CHECK(r[0] == 0x55 && r[1] == 0x01 && g_portal.led[1][0] == 255 && g_portal.led[1][1] == 128);

    /* the game writes progress: page 8 of the figure; a second later it is saved in saves/ */
    pl[0] = 4; pl[1] = 8; pl[2] = 0xDE; pl[3] = 0xAD; pl[4] = 0xBE; pl[5] = 0xEF;
    command(0xD3, pl, 6, r);
    CHECK(r[0] == 0x55 && r[1] == 0x02);
    for (i = 0; i < 400; i++) {
        usleep(10000);
        if (read_file(g_slot_save[3], back, TP_SIZE) == TP_SIZE) break;
    }
    CHECK(read_file(g_slot_save[3], back, TP_SIZE) == TP_SIZE);
    CHECK(back[32] == 0xDE && back[35] == 0xEF && memcmp(back, fig, 8) == 0);
    {
        char orig[TP_SIZE];
        CHECK(read_file(DIR_FIGS "/Batman.bin", orig, TP_SIZE) == TP_SIZE && memcmp(orig, fig, TP_SIZE) == 0); /* original untouched */
    }

    /* take it off: removal event; put it back (automatic slot): same slot, progress restored from the save */
    remove_figure(3);
    CHECK(next_event(r));
    CHECK(r[0] == 0x56 && r[2] == 2 && r[4] == 4 && r[5] == 1);
    CHECK(place_figure(-1, 0) == 3);
    CHECK(next_event(r) && r[0] == 0x56 && r[4] == 4 && r[5] == 0);
    pl[0] = 4; pl[1] = 8;
    command(0xD2, pl, 2, r);
    CHECK(r[4] == 0xDE && r[5] == 0xAD && r[6] == 0xBE && r[7] == 0xEF);

    /* the vehicle on the centre pad */
    CHECK(place_figure(0, 1) == 0);
    CHECK(next_event(r) && r[2] == 1 && r[4] == 1 && r[5] == 0 && r[6] == 4 && r[7] == 7);
    CHECK(g_portal.slot[0].id == 0x0463);
    memset(d, 0, 8);
    d[0] = 1;
    tp_tea_encrypt(d, NULL, enc);
    command(0xD4, enc, 8, r);
    tp_tea_decrypt(r + 4, NULL, d);
    CHECK(d[0] == 0x63 && d[1] == 0x04);
    CHECK(place_figure(0, 0) == -1); /* occupied */

    /* when the game completes transfers from its own HandleEvents call (worker steps aside) */
    {
        uint8_t c[32];
        int o, in2;
        handle_events_hook();
        pl[0] = 3;
        mkcmd(c, 0xC1, pl, 1);
        o = submit(0x01, c);
        in2 = submit(0x81, NULL);
        for (i = 0; i < 50 && !(cb_done[o] && cb_done[in2]); i++)
            handle_events_hook();
        CHECK(cb_done[o] && cb_done[in2]);
        CHECK(xfer_buf[in2][0] == 0x55 && xfer_buf[in2][1] == 0x01);
        remove_figure(0); /* removal event comes through the same way */
        in2 = submit(0x81, NULL);
        for (i = 0; i < 50 && !cb_done[in2]; i++)
            handle_events_hook();
        CHECK(cb_done[in2] && xfer_buf[in2][0] == 0x56 && xfer_buf[in2][5] == 1);
    }

    /* synchronous calls: SET_REPORT with a command and a read; interrupt OUT with a command and a read */
    {
        uint8_t c[32], out[32];
        int32_t got = 0;
        CHECK(g_ninq == 0);
        mkcmd(c, 0xC1, (const uint8_t *)"\x01", 1);
        CHECK(sync_control(0x21, 0x09, 0x0200, (uint64_t)c, 32) == 32);
        CHECK(sync_interrupt(0x81, (uint64_t)out, 32, (uint64_t)&got, 100) == 0 && got == 32 && out[0] == 0x55 && out[1] == 0x01);
        mkcmd(c, 0xC1, (const uint8_t *)"\x01", 1);
        CHECK(sync_interrupt(0x01, (uint64_t)c, 32, (uint64_t)&got, 100) == 0 && got == 32);
        CHECK(sync_interrupt(0x81, (uint64_t)out, 32, (uint64_t)&got, 100) == 0 && out[0] == 0x55);
        CHECK(sync_interrupt(0x81, (uint64_t)out, 32, (uint64_t)&got, 30) == (uint64_t)(int64_t)-7); /* silent pad: timeout */
    }

    /* descriptors the game asks for */
    {
        uint8_t buf[64];
        n = control_in(0x80, 0x06, 0x0100, buf, 64);
        CHECK(n == 18 && buf[8] == 0x6F && buf[9] == 0x0E && buf[10] == 0x41 && buf[11] == 0x02);
        n = control_in(0x80, 0x06, 0x0200, buf, 64);
        CHECK(n == 41 && buf[29] == 0x81 && buf[31] == 0x20 && buf[36] == 0x01 && buf[38] == 0x20); /* EP IN 0x81, EP OUT 0x01, 32 bytes */
        n = control_in(0x80, 0x06, 0x0302, buf, 64);
        CHECK(n == 2 + 2 * 17 && buf[2] == 'L');
    }

    snprintf(path, sizeof(path), "%s", g_slot_save[3]);
    printf("%d checks, %d failed\n", checks, fails);
    if (argc > 1 && strcmp(argv[1], "serve") == 0) {
        printf("serving on port %d for 40 s\n", WEB_PORT);
        fflush(stdout);
        place_figure(3, 0);
        sleep(40);
    }
    return fails ? 1 : 0;
}
