/*
 * lego_toypad - GoldHEN plugin: virtual LEGO Dimensions Toy Pad for the PS4.
 *
 * Makes the game believe a real Toy Pad (USB VID 0E6F / PID 0241) is plugged in. Figures are your own
 * 180-byte .bin dumps, chosen from a phone/PC browser at http://<ps4-ip>:8080
 *
 * The USB part (swapping the game's libSceUsbd pointers and answering for one fake device) is the same
 * mechanism as the Skylanders plugin. The Toy Pad protocol follows RPCS3 (Emu/Io/Dimensions.cpp) - see lego.c.
 *
 * Files (all under /data/GoldHEN/lego/):
 *   figures/   your .bin dumps (sub-folders are fine). Never modified.
 *   saves/     progress the game wrote to each figure. Delete one to reset that figure.
 *   state.txt  which figures were on the pad (restored at the next launch)
 *   log.txt    what the plugin saw - send this when reporting a problem
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <orbis/libkernel.h>

#include "lego.h"
#include "web.h" /* phone web page: regenerate with tools/gen_web.py from index.html */

#define PLUGIN_VERSION_STR "0.2.0"

/* BSD sockets exported by libkernel */
extern int socket(int, int, int);
extern int bind(int, const void *, unsigned);
extern int listen(int, int);
extern int accept(int, void *, unsigned *);
extern int recv(int, void *, size_t, int);
extern int send(int, const void *, size_t, int);
extern int setsockopt(int, int, int, const void *, unsigned);
extern int close(int);

#define attr_public __attribute__((visibility("default")))
#define IN_DATA __attribute__((section(".data"))) /* keep big buffers out of .bss */

attr_public const char *g_pluginName = "lego_toypad";
attr_public const char *g_pluginDesc = "Virtual LEGO Dimensions Toy Pad (phone web UI on port 8080)";
attr_public const char *g_pluginAuth = "Nox";
attr_public uint32_t g_pluginVersion = 0x00000200; /* 0.2.0 */

#define DIR_MAIN "/data/GoldHEN/lego"
#define DIR_FIGS DIR_MAIN "/figures"
#define DIR_UPLOAD DIR_FIGS "/Uploaded"
#define DIR_SAVES DIR_MAIN "/saves"
#define WEB_PORT 8080
#define IN_TIMEOUT_TICKS 250 /* ~0.5 s (worker tick is 2 ms) */

typedef uint64_t (*raw8_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*cb_fn)(uint64_t);

/* ------------------------------------------------------------------ small helpers */

static volatile int g_lock;
static void lock(void) { while (__sync_lock_test_and_set(&g_lock, 1)) sceKernelUsleep(100); }
static void unlock(void) { __sync_lock_release(&g_lock); }

static volatile uint32_t g_tick; /* worker ticks, ~2 ms each */

static int is_ptr(uint64_t p) { return p > 0x10000 && p < 0x0000800000000000ULL && (p & 7) == 0; }

static int contains_ci(const char *s, const char *needle)
{
    size_t i, j, n = strlen(needle);
    for (i = 0; s[i]; i++) {
        for (j = 0; j < n && s[i + j]; j++) {
            char a = s[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != b) break;
        }
        if (j == n) return 1;
    }
    return 0;
}

static int read_file(const char *path, void *buf, int max)
{
    int fd = sceKernelOpen(path, 0, 0), n;
    if (fd < 0)
        return -1;
    n = (int)sceKernelRead(fd, buf, max);
    sceKernelClose(fd);
    return n;
}

static int write_file(const char *path, const void *buf, int n)
{
    int fd = sceKernelOpen(path, 0x601, 0666); /* O_WRONLY | O_CREAT | O_TRUNC */
    int w;
    if (fd < 0)
        return -1;
    w = (int)sceKernelWrite(fd, buf, n);
    sceKernelClose(fd);
    return w;
}

/* ------------------------------------------------------------------ log (memory ring + log.txt) */

static char g_log[8192] IN_DATA;
static int g_loglen IN_DATA;
static int g_logfd = -1;

static void logf_(const char *fmt, ...)
{
    char line[256];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    lock();
    if (g_loglen + n >= (int)sizeof(g_log)) { /* drop the oldest half */
        int keep = g_loglen / 2;
        memmove(g_log, g_log + g_loglen - keep, keep);
        g_loglen = keep;
    }
    memcpy(g_log + g_loglen, line, n);
    g_loglen += n;
    unlock();
    if (g_logfd >= 0)
        sceKernelWrite(g_logfd, line, n);
}

/* ------------------------------------------------------------------ figure library (scanned from figures/) */

#define LIB_MAX 4096
#define POOL_SIZE (512 * 1024)
static char *g_pool;          /* relative paths, 0-terminated, back to back */
static int g_pool_used;
static int *g_lib_off;        /* offset of each entry in g_pool */
static int g_lib_n;

static const char *lib_rel(int i) { return (i >= 0 && i < g_lib_n) ? g_pool + g_lib_off[i] : ""; }

typedef struct { uint32_t fileno; uint16_t reclen; uint8_t type; uint8_t namlen; char name[256]; } Dirent;

static int ends_bin(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && s[n - 4] == '.' && (s[n - 3] | 32) == 'b' && (s[n - 2] | 32) == 'i' && (s[n - 1] | 32) == 'n';
}

static void scan_dir(const char *rel, int depth)
{
    char path[512], buf[4096];
    int fd, n;
    snprintf(path, sizeof(path), DIR_FIGS "%s%s", rel[0] ? "/" : "", rel);
    fd = sceKernelOpen(path, 0x20000, 0); /* O_RDONLY | O_DIRECTORY */
    if (fd < 0)
        return;
    while ((n = sceKernelGetdents(fd, buf, sizeof(buf))) > 0) {
        int off = 0;
        while (off < n) {
            Dirent *d = (Dirent *)(buf + off);
            char sub[512];
            if (d->reclen == 0) break;
            off += d->reclen;
            if (d->fileno == 0 || d->name[0] == '.')
                continue;
            snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", d->name);
            if (d->type == 4 && depth < 4) {
                scan_dir(sub, depth + 1);
            } else if (d->type == 8 && ends_bin(d->name) && g_lib_n < LIB_MAX) {
                int len = (int)strlen(sub) + 1;
                if (g_pool_used + len > POOL_SIZE) continue;
                memcpy(g_pool + g_pool_used, sub, len);
                g_lib_off[g_lib_n++] = g_pool_used;
                g_pool_used += len;
            }
        }
    }
    sceKernelClose(fd);
}

static int cmp_entry(const void *a, const void *b)
{
    const char *x = g_pool + *(const int *)a, *y = g_pool + *(const int *)b;
    for (;; x++, y++) {
        char c = *x, d = *y;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (d >= 'A' && d <= 'Z') d += 32;
        if (c != d || !c) return (unsigned char)c - (unsigned char)d;
    }
}

/* defined with their values below; declared here because rescan needs them */
static int g_slot_lib[LEGO_SLOTS];
static LegoPad g_pad;
static char g_slot_save[LEGO_SLOTS][96];
static volatile int g_game_seen;

static void rescan(void)
{
    char keep[LEGO_SLOTS][512];
    int s, i;
    if (!g_pool || !g_lib_off)
        return;
    lock();
    for (s = 0; s < LEGO_SLOTS; s++) /* remember what is on the pad, by path */
        snprintf(keep[s], sizeof(keep[s]), "%s", g_slot_lib[s] >= 0 ? lib_rel(g_slot_lib[s]) : "");
    g_lib_n = 0;
    g_pool_used = 0;
    unlock();
    scan_dir("", 0);
    qsort(g_lib_off, g_lib_n, sizeof(int), cmp_entry);
    /* the pad keeps its figures; only the library indexes change */
    lock();
    for (s = 0; s < LEGO_SLOTS; s++) {
        int found = -1;
        if (keep[s][0])
            for (i = 0; i < g_lib_n; i++)
                if (strcmp(lib_rel(i), keep[s]) == 0) { found = i; break; }
        g_slot_lib[s] = found;
    }
    unlock();
    logf_("library: %d figure files", g_lib_n);
}

/* ------------------------------------------------------------------ the pad + what is on it */

/* (g_pad, g_slot_lib, g_slot_save, g_game_seen are declared above rescan) */
static void slot_lib_init(void)
{
    int s;
    for (s = 0; s < LEGO_SLOTS; s++)
        g_slot_lib[s] = -1;
}

static void save_path_for(const char *rel, char *out, int max)
{
    uint32_t h = 2166136261u;
    const char *p, *base = rel;
    char name[48];
    int i;
    for (p = rel; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
        if (*p == '/') base = p + 1;
    }
    for (i = 0; base[i] && i < 40; i++) {
        char c = base[i];
        name[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.') ? c : '_';
    }
    name[i] = 0;
    snprintf(out, max, DIR_SAVES "/%08x_%s", h, name);
}

/* same chip? (byte 3 is a check byte and differs per dump, so it is skipped) */
static int same_uid(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 3) == 0 && memcmp(a + 4, b + 4, 4) == 0;
}

static void save_state(void)
{
    char b[LEGO_SLOTS * 520];
    int n = 0, s;
    lock();
    for (s = 0; s < LEGO_SLOTS; s++)
        n += snprintf(b + n, sizeof(b) - n, "%s\n", g_slot_lib[s] >= 0 ? lib_rel(g_slot_lib[s]) : "-");
    unlock();
    write_file(DIR_MAIN "/state.txt", b, n);
}

static void flush_slot(int s)
{
    uint8_t copy[LEGO_SIZE]; /* worker and web threads both save: own buffer each */
    char path[96];
    lock();
    if (!g_pad.fig[s].dirty || !g_slot_save[s][0]) {
        g_pad.fig[s].dirty = 0;
        unlock();
        return;
    }
    memcpy(copy, g_pad.fig[s].data, LEGO_SIZE);
    memcpy(path, g_slot_save[s], sizeof(path));
    g_pad.fig[s].dirty = 0;
    unlock();
    if (write_file(path, copy, LEGO_SIZE) != LEGO_SIZE)
        logf_("save failed: %s", path);
}

static void flush_dirty(void)
{
    int s;
    for (s = 0; s < LEGO_SLOTS; s++)
        if (g_pad.fig[s].dirty)
            flush_slot(s);
}

/* returns 0 ok, -1 bad slot/figure, -2 file problem, -3 slot busy */
static int place_figure(int slot, int id)
{
    static uint8_t tmp[LEGO_SIZE] IN_DATA;
    static uint8_t sv[LEGO_SIZE] IN_DATA;
    char path[600], save[96];
    int n, r, busy;
    if (id < 0 || id >= g_lib_n || slot < 0 || slot >= LEGO_SLOTS)
        return -1;
    lock();
    busy = g_pad.fig[slot].present;
    unlock();
    if (busy)
        return -3;
    snprintf(path, sizeof(path), DIR_FIGS "/%s", lib_rel(id));
    n = read_file(path, tmp, LEGO_SIZE);
    if (n != LEGO_SIZE) {
        logf_("not a 180-byte figure dump (%d bytes): %s", n, lib_rel(id));
        return -2;
    }
    save_path_for(lib_rel(id), save, sizeof(save));
    if (read_file(save, sv, LEGO_SIZE) == LEGO_SIZE && same_uid(sv, tmp))
        memcpy(tmp, sv, LEGO_SIZE); /* continue where the game left it */
    lock();
    r = lego_place(&g_pad, slot, tmp);
    if (r == 0) {
        g_slot_lib[slot] = id;
        memcpy(g_slot_save[slot], save, sizeof(save));
    }
    unlock();
    if (r == 0) {
        logf_("placed slot %d: %s (figure id %u)", slot + 1, lib_rel(id), lego_figure_id(tmp));
        save_state();
        return 0;
    }
    return -3;
}

static int remove_figure(int s)
{
    int was;
    if (s < 0 || s >= LEGO_SLOTS)
        return 0;
    flush_slot(s); /* save progress before the figure leaves */
    lock();
    was = lego_remove(&g_pad, s);
    g_slot_lib[s] = -1;
    g_slot_save[s][0] = 0;
    unlock();
    if (was)
        logf_("removed slot %d", s + 1);
    save_state();
    return was;
}

static void load_state(void)
{
    char b[LEGO_SLOTS * 520 + 1], *p, *e;
    int n = read_file(DIR_MAIN "/state.txt", b, sizeof(b) - 1), s = 0, i;
    if (n <= 0)
        return;
    b[n] = 0;
    for (p = b; *p && s < LEGO_SLOTS; p = e, s++) {
        e = p;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        if (strcmp(p, "-") == 0 || !*p)
            continue;
        for (i = 0; i < g_lib_n; i++)
            if (strcmp(lib_rel(i), p) == 0) {
                place_figure(s, i);
                break;
            }
    }
}

/* ------------------------------------------------------------------ USB descriptors of a real Toy Pad */

static const uint8_t DEV_DESC[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 0x20, 0x6F, 0x0E, 0x41, 0x02, 0x00, 0x02, 1, 2, 3, 1};
static const uint8_t CFG_DESC[41] = {
    9, 2, 41, 0, 1, 1, 0, 0x80, 0xFA,                   /* configuration */
    9, 4, 0, 0, 2, 3, 0, 0, 0,                          /* interface 0: HID, 2 endpoints */
    9, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1D, 0x00,  /* HID descriptor, report descriptor = 29 bytes */
    7, 5, 0x81, 3, 0x20, 0, 1,                          /* EP 0x81 interrupt IN  (replies, events) */
    7, 5, 0x01, 3, 0x20, 0, 1};                         /* EP 0x01 interrupt OUT (commands) */
/* assumption: same 32-byte vendor report as the Skylanders pad (the RPCS3 code does not include it) */
static const uint8_t HID_REPORT[29] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x19, 0x01, 0x29, 0x40,
                                       0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x20, 0x81, 0x00,
                                       0x19, 0x01, 0x29, 0x40, 0x91, 0x00, 0xC0};
static const char *STRS[4] = {NULL, "LEGO", "LEGO Dimensions Toy Pad", "0001"};

static int string_desc(int index, uint8_t *out, int max)
{
    const char *s;
    int n = 0, i;
    if (index == 0) {
        static const uint8_t lang[4] = {4, 3, 0x09, 0x04};
        n = max < 4 ? max : 4;
        memcpy(out, lang, n);
        return n;
    }
    if (index < 1 || index > 3 || max < 2) return 0;
    s = STRS[index];
    out[0] = (uint8_t)(2 + 2 * strlen(s));
    out[1] = 3;
    for (i = 0, n = 2; s[i] && n + 1 < max; i++) {
        out[n++] = (uint8_t)s[i];
        out[n++] = 0;
    }
    return n;
}

/* answers a control IN request (GET_DESCRIPTOR etc.). Returns bytes written. */
static int control_in(uint8_t rt, uint8_t req, uint16_t wValue, uint8_t *out, int max)
{
    const uint8_t *src = NULL;
    int n = 0;
    uint8_t tmp[64];
    if (req == 0x06) { /* GET_DESCRIPTOR */
        uint8_t type = wValue >> 8;
        if (type == 1) { src = DEV_DESC; n = 18; }
        else if (type == 2) { src = CFG_DESC; n = 41; }
        else if (type == 0x21) { src = CFG_DESC + 18; n = 9; }
        else if (type == 0x22) { src = HID_REPORT; n = 29; }
        else if (type == 3) { n = string_desc(wValue & 0xFF, tmp, sizeof(tmp)); src = tmp; }
    } else if (req == 0x08 && rt == 0x80) { /* GET_CONFIGURATION */
        tmp[0] = 1; src = tmp; n = 1;
    } else if (req == 0x00 && (rt & 0x60) == 0) { /* GET_STATUS */
        tmp[0] = 0; tmp[1] = 0; src = tmp; n = 2;
    }
    if (!src) return 0;
    if (n > max) n = max;
    memcpy(out, src, n);
    return n;
}

/* ------------------------------------------------------------------ fake USB device + queues */

enum { R_OTHER = 0, R_LIST, R_FREE, R_DESC, R_OPEN, R_ALLOC, R_FILL, R_SUBMIT, R_REF, R_GETCFG, R_ACFG, R_CTRL,
       R_INTR, R_VIDPID, R_FREECFG, R_GETDEV, R_STRASCII, R_CANCEL, R_FREEXFER, R_MAXPKT, R_GETDESC, R_HANDLE,
       R_KDACTIVE, R_KDDETACH, R_KDATTACH, R_CHECKCONN };

static uint64_t handle_events_hook(void);
typedef struct { const char *name; int role; } FnName;
static const FnName g_names[] = {
    {"sceUsbdInit", R_OTHER},                      {"sceUsbdGetDeviceList", R_LIST},
    {"sceUsbdFreeDeviceList", R_FREE},             {"sceUsbdGetDeviceDescriptor", R_DESC},
    {"sceUsbdOpen", R_OPEN},                       {"sceUsbdOpenDeviceWithVidPid", R_VIDPID},
    {"sceUsbdClaimInterface", R_OTHER},            {"sceUsbdSetConfiguration", R_OTHER},
    {"sceUsbdControlTransfer", R_CTRL},            {"sceUsbdInterruptTransfer", R_INTR},
    {"sceUsbdBulkTransfer", R_INTR},               {"sceUsbdAllocTransfer", R_ALLOC},
    {"sceUsbdFillInterruptTransfer", R_FILL},      {"sceUsbdFillBulkTransfer", R_FILL},
    {"sceUsbdFillControlTransfer", R_FILL},        {"sceUsbdSubmitTransfer", R_SUBMIT},
    {"sceUsbdCancelTransfer", R_CANCEL},           {"sceUsbdFreeTransfer", R_FREEXFER},
    {"sceUsbdRefDevice", R_REF},                   {"sceUsbdGetConfiguration", R_GETCFG},
    {"sceUsbdGetActiveConfigDescriptor", R_ACFG},  {"sceUsbdGetConfigDescriptor", R_ACFG},
    {"sceUsbdFreeConfigDescriptor", R_FREECFG},    {"sceUsbdGetDevice", R_GETDEV},
    {"sceUsbdGetStringDescriptorAscii", R_STRASCII}, {"sceUsbdGetMaxPacketSize", R_MAXPKT},
    {"sceUsbdGetDescriptor", R_GETDESC},           {"sceUsbdHandleEventsTimeout", R_HANDLE},
    /* below: only named so the log can say what the game called */
    {"sceUsbdExit", R_OTHER},                      {"sceUsbdClose", R_OTHER},
    {"sceUsbdUnrefDevice", R_OTHER},               {"sceUsbdReleaseInterface", R_OTHER},
    {"sceUsbdKernelDriverActive", R_KDACTIVE},     {"sceUsbdDetachKernelDriver", R_KDDETACH},
    {"sceUsbdAttachKernelDriver", R_KDATTACH},     {"sceUsbdSetInterfaceAltSetting", R_OTHER},
    {"sceUsbdClearHalt", R_OTHER},                 {"sceUsbdResetDevice", R_OTHER},
    {"sceUsbdGetBusNumber", R_OTHER},              {"sceUsbdGetDeviceAddress", R_OTHER},
    {"sceUsbdGetDeviceSpeed", R_OTHER},            {"sceUsbdGetMaxIsoPacketSize", R_OTHER},
    {"sceUsbdHandleEvents", R_HANDLE},             {"sceUsbdHandleEventsLocked", R_HANDLE},
    {"sceUsbdLockEvents", R_OTHER},                {"sceUsbdUnlockEvents", R_OTHER},
    {"sceUsbdTryLockEvents", R_OTHER},             {"sceUsbdLockEventWaiters", R_OTHER},
    {"sceUsbdUnlockEventWaiters", R_OTHER},        {"sceUsbdWaitForEvent", R_OTHER},
    {"sceUsbdEventHandlerActive", R_OTHER},        {"sceUsbdEventHandlingOk", R_OTHER},
    {"sceUsbdGetStringDescriptor", R_OTHER},       {"sceUsbdCheckConnected", R_CHECKCONN},
    {"sceUsbdGetConfigDescriptorByValue", R_OTHER}, {"sceUsbdFillIsoTransfer", R_OTHER},
    {"sceUsbdControlTransferGetData", R_OTHER},    {"sceUsbdControlTransferGetSetup", R_OTHER},
    {"sceUsbdFillControlSetup", R_OTHER},          {"sceUsbdGetIsoPacketBuffer", R_OTHER},
    {"sceUsbdSetIsoPacketLengths", R_OTHER},
};
#define NFN ((int)(sizeof(g_names) / sizeof(g_names[0])))
static uint64_t g_real[NFN];
static uint64_t g_usbd_lo, g_usbd_hi; /* libSceUsbd address range */
static uint64_t g_text_lo, g_text_hi; /* the game's code range (valid callback targets) */

#define MAXSLOT 40
typedef struct { uint64_t orig; uint64_t slot_addr; int role; uint32_t cnt; int name; } Slot;
static Slot g_slot[MAXSLOT] IN_DATA;
static int g_nslot;
static int g_installed;

static uint8_t g_fake_mem[1024] __attribute__((aligned(16))) IN_DATA;
#define FAKE_DEV ((uint64_t)&g_fake_mem[0])
#define FAKE_HANDLE ((uint64_t)&g_fake_mem[256])
#define FAKE_CFG ((uint64_t)&g_fake_mem[512])
static uint64_t g_fake_list[16];
static uint64_t g_real_list;

#define NTRK 32
static uint64_t g_trk[NTRK];
static int g_ntrk;

/* transfers the worker or HandleEvents must complete */
typedef struct { uint64_t t; int actual; int is_cmd; int cmdlen; uint8_t d[32]; } OutEnt;
typedef struct { uint8_t d[32]; } Resp;
static OutEnt g_oq[16] IN_DATA;     /* OUT transfers (commands) waiting to be completed */
static int g_noq;
static Resp g_resp[32] IN_DATA;     /* replies waiting for the interrupt IN endpoint */
static int g_nresp;
static uint64_t g_inq[8];           /* interrupt IN transfers the game is waiting on */
static uint32_t g_inq_tick[8];      /* when each was submitted */
static int g_ninq;
static volatile int g_opened;
static volatile int g_he_seen;
static volatile uint32_t g_he_tick;
static int g_kdriver = 1;           /* "kernel driver active" until the game detaches it */
static uint64_t g_cancelq[8];
static int g_ncancel;

/* queue helpers: caller holds the lock */

static int pending_report(void)
{
    return g_nresp > 0 || g_pad.nev > 0;
}

/* next report for the interrupt IN endpoint: a figure event first, then a queued reply. Caller holds the lock. */
static int next_report(uint8_t *out)
{
    if (lego_pop_event(&g_pad, out))
        return 1;
    if (g_nresp > 0) {
        memcpy(out, g_resp[0].d, 32);
        memmove(g_resp, g_resp + 1, sizeof(Resp) * (g_nresp - 1));
        g_nresp--;
        return 1;
    }
    return 0;
}

/* runs one command (caller holds the lock) */
static void do_command(const uint8_t *d, int len)
{
    uint8_t pkt[32];
    Resp r;
    if (len <= 0) return;
    memset(pkt, 0, sizeof(pkt));
    memcpy(pkt, d, len < 32 ? len : 32);
    if (lego_command(&g_pad, pkt, r.d) && g_nresp < 32)
        g_resp[g_nresp++] = r;
    g_game_seen = 1;
}

static int tracked(uint64_t t)
{
    int i, r = 0;
    if (!t) return 0;
    lock();
    for (i = 0; i < NTRK; i++)
        if (g_trk[i] == t) r = 1;
    unlock();
    return r;
}

/* libusb_transfer layout: +8 flags, +9 endpoint, +10 type, +16 status, +20 length, +24 actual, +32 callback,
 * +48 buffer. Types: 0 control, 2 bulk, 3 interrupt. */
static uint64_t handle_submit(uint64_t t)
{
    uint8_t ep = *(uint8_t *)(t + 9), type = *(uint8_t *)(t + 10);
    int len = *(int32_t *)(t + 20);
    uint8_t *buf = (uint8_t *)*(uint64_t *)(t + 48);
    OutEnt e;
    static uint32_t nsub;
    memset(&e, 0, sizeof(e));
    e.t = t;
    if ((uint64_t)buf <= 0x10000 || len < 0)
        len = 0;
    if (++nsub <= 20 || nsub == 500)
        logf_("submit #%u type %d ep %02x len %d", nsub, type, ep, len);

    if (type == 0 && len >= 8) { /* control: 8-byte setup packet, then the data */
        uint8_t rt = buf[0], req = buf[1];
        uint16_t wValue = buf[2] | (buf[3] << 8), wLength = buf[6] | (buf[7] << 8);
        int dl = len - 8 < wLength ? len - 8 : wLength;
        if (rt == 0x21 && req == 0x09) { /* SET_REPORT: some hosts send commands this way */
            e.is_cmd = 1;
            e.cmdlen = dl > 32 ? 32 : dl;
            memcpy(e.d, buf + 8, e.cmdlen);
            e.actual = dl;
        } else if (rt & 0x80) {
            e.actual = control_in(rt, req, wValue, buf + 8, dl);
        } else {
            e.actual = dl;
        }
    } else if (ep & 0x80) { /* interrupt IN 0x81: replies and figure events */
        lock();
        g_trk[g_ntrk++ % NTRK] = t;
        if (g_ninq < 8) {
            g_inq_tick[g_ninq] = g_tick;
            g_inq[g_ninq++] = t;
        }
        unlock();
        return 0;
    } else { /* interrupt OUT 0x01: 32-byte commands from the game */
        e.is_cmd = 1;
        e.cmdlen = len < 32 ? len : 32;
        if (len > 0)
            memcpy(e.d, buf, e.cmdlen);
        e.actual = len;
    }
    lock();
    g_trk[g_ntrk++ % NTRK] = t;
    if (g_noq < 16)
        g_oq[g_noq++] = e;
    unlock();
    return 0;
}

/* sceUsbdControlTransfer(handle, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout) */
static uint64_t sync_control(uint64_t b, uint64_t c, uint64_t d, uint64_t f, uint64_t g)
{
    uint8_t rt = (uint8_t)b, req = (uint8_t)c;
    uint16_t wValue = (uint16_t)d, wLength = (uint16_t)g;
    uint8_t *data = (uint8_t *)f;
    if ((uint64_t)data <= 0x10000) wLength = 0;
    if (rt == 0x21 && req == 0x09) {
        int n = wLength > 32 ? 32 : wLength;
        lock();
        do_command(data, n);
        unlock();
        return wLength;
    }
    if (rt & 0x80)
        return (uint64_t)(uint32_t)control_in(rt, req, wValue, data, wLength);
    return wLength;
}

/* sceUsbdInterruptTransfer(handle, endpoint, data, length, int *transferred, timeout_ms) */
static uint64_t sync_interrupt(uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint8_t ep = (uint8_t)b;
    uint8_t *data = (uint8_t *)c;
    int len = (int)d, n = 0;
    uint32_t timeout = (uint32_t)f;
    if ((uint64_t)data <= 0x10000) len = 0;
    if (ep & 0x80) {
        uint8_t rep[32];
        int waited = 0, got = 0;
        for (;;) {
            lock();
            got = pending_report();
            if (got)
                next_report(rep);
            unlock();
            if (got)
                break;
            if (waited >= (timeout ? (int)timeout : 2000)) {
                if (e > 0x10000) *(int32_t *)e = 0;
                return (uint64_t)(int64_t)-7; /* LIBUSB_ERROR_TIMEOUT */
            }
            sceKernelUsleep(1000);
            waited++;
        }
        n = len < 32 ? len : 32;
        if (n > 0) memcpy(data, rep, n);
    } else {
        if (len > 0 && ep == 0x01) {
            lock();
            do_command(data, len < 32 ? len : 32);
            unlock();
        }
        n = len;
    }
    if (e > 0x10000) *(int32_t *)e = n;
    return 0;
}

/* executable ranges of every loaded module (a game may keep its USB code in a library, not the main file) */
#define NEXEC 64
static uint64_t g_exec_lo[NEXEC] IN_DATA, g_exec_hi[NEXEC] IN_DATA;
static int g_nexec;

static void refresh_exec_ranges(void)
{
    OrbisKernelModule h[256];
    size_t n = 0, i;
    int k = 0;
    if (sceKernelGetModuleList(h, 256, &n) != 0)
        return;
    for (i = 0; i < n && i < 256 && k < NEXEC; i++) {
        OrbisKernelModuleInfo info;
        uint32_t sg;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(h[i], &info) != 0)
            continue;
        for (sg = 0; sg < info.segmentCount && sg < 4 && k < NEXEC; sg++)
            if (info.segmentInfo[sg].prot & 4) {
                g_exec_lo[k] = (uint64_t)info.segmentInfo[sg].address;
                g_exec_hi[k] = g_exec_lo[k] + info.segmentInfo[sg].size;
                k++;
            }
    }
    g_nexec = k;
}

static int is_code(uint64_t cb)
{
    int i, pass;
    if (cb >= g_text_lo && cb < g_text_hi)
        return 1;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < g_nexec; i++)
            if (cb >= g_exec_lo[i] && cb < g_exec_hi[i])
                return 1;
        if (pass == 0)
            refresh_exec_ranges();
    }
    return 0;
}

/* completes a fake transfer, then calls the game's own callback */
static void finish_transfer(uint64_t t, int status, int actual)
{
    static int bad_logged;
    uint64_t cb = *(uint64_t *)(t + 32);
    *(int32_t *)(t + 16) = status;
    *(int32_t *)(t + 24) = actual;
    if (cb && is_code(cb))
        ((cb_fn)cb)(t);
    else if (bad_logged++ < 5)
        logf_("callback %llx is not in any loaded code - not called", (unsigned long long)cb);
}

/* is p inside our fake USB objects (device, handle, config)? */
static int in_fake_mem(uint64_t p)
{
    return p >= (uint64_t)g_fake_mem && p < (uint64_t)g_fake_mem + sizeof(g_fake_mem);
}

/* the generic dispatcher: all USB calls of the game go through here */
static uint64_t generic_call(int idx, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                             uint64_t f, uint64_t g, uint64_t h)
{
    Slot *s = &g_slot[idx];
    s->cnt++;
    switch (s->role) {
    case R_LIST: {
        uint64_t r = ((raw8_fn)s->orig)(a, b, c, d, e, f, g, h);
        int cnt = (int)r;
        if (cnt >= 0 && cnt <= 12 && is_ptr(a)) {
            uint64_t real = *(uint64_t *)a;
            int i;
            if (is_ptr(real))
                for (i = 0; i < cnt; i++)
                    g_fake_list[i] = ((uint64_t *)real)[i];
            g_fake_list[cnt] = FAKE_DEV;
            g_fake_list[cnt + 1] = 0;
            g_real_list = real;
            *(uint64_t *)a = (uint64_t)g_fake_list;
            if (s->cnt <= 2) logf_("device list: %d real + virtual pad", cnt);
            return (uint64_t)(uint32_t)(cnt + 1);
        }
        return r;
    }
    case R_FREE:
        if (a == (uint64_t)g_fake_list) {
            uint64_t r = ((raw8_fn)s->orig)(g_real_list, b, c, d, e, f, g, h);
            g_real_list = 0;
            return r;
        }
        break;
    case R_DESC:
        if (a == FAKE_DEV && b > 0x10000) {
            memcpy((void *)b, DEV_DESC, 18);
            return 0;
        }
        break;
    case R_OPEN:
        if (a == FAKE_DEV) {
            if (b > 0x10000)
                *(uint64_t *)b = FAKE_HANDLE;
            logf_("game opened the pad");
            g_opened = 1;
            return 0;
        }
        break;
    case R_VIDPID:
        if ((uint16_t)b == 0x0E6F && (uint16_t)c == 0x0241) {
            logf_("game opened the pad (by VID/PID)");
            g_opened = 1;
            return FAKE_HANDLE;
        }
        break;
    case R_SUBMIT:
        if (is_ptr(a) && *(uint64_t *)a == FAKE_HANDLE)
            return handle_submit(a);
        break;
    case R_CTRL:
        if (a == FAKE_HANDLE)
            return sync_control(b, c, d, f, g);
        break;
    case R_INTR:
        if (a == FAKE_HANDLE)
            return sync_interrupt(b, c, d, e, f);
        break;
    case R_HANDLE: /* no device argument: act only once our pad is open */
        if (g_opened)
            return handle_events_hook();
        break;
    case R_KDACTIVE:
        if (a == FAKE_HANDLE)
            return (uint64_t)g_kdriver;
        break;
    case R_KDDETACH:
        if (a == FAKE_HANDLE) {
            g_kdriver = 0;
            return 0;
        }
        break;
    case R_KDATTACH:
        if (a == FAKE_HANDLE) {
            g_kdriver = 1;
            return 0;
        }
        break;
    case R_CHECKCONN:
        if (a == FAKE_HANDLE)
            return 0;
        break;
    case R_CANCEL:
        if (tracked(a)) {
            lock();
            if (g_ncancel < 8) g_cancelq[g_ncancel++] = a;
            unlock();
            return 0;
        }
        break;
    case R_FREEXFER:
        if (tracked(a)) {
            int i;
            lock();
            for (i = 0; i < NTRK; i++)
                if (g_trk[i] == a) g_trk[i] = 0;
            unlock();
        }
        break; /* the real function frees it */
    case R_REF:
        if (a == FAKE_DEV)
            return a;
        break;
    case R_GETDEV:
        if (a == FAKE_HANDLE)
            return FAKE_DEV;
        break;
    case R_GETCFG:
        if (a == FAKE_HANDLE) {
            if (b > 0x10000)
                *(int32_t *)b = 1;
            return 0;
        }
        break;
    case R_ACFG: /* GetActiveConfigDescriptor(dev, &cfg) / GetConfigDescriptor(dev, index, &cfg) */
        if (a == FAKE_DEV) {
            uint64_t out = (s->orig == g_real[21]) ? c : b;
            if (out > 0x10000)
                *(uint64_t *)out = FAKE_CFG;
            return 0;
        }
        break;
    case R_FREECFG:
        if (a == FAKE_CFG)
            return 0;
        break;
    case R_MAXPKT:
        if (a == FAKE_DEV)
            return 0x20;
        break;
    case R_STRASCII: /* (handle, index, data, length) */
        if (a == FAKE_HANDLE) {
            const char *str = b >= 1 && b <= 3 ? STRS[b] : "";
            int n = (int)strlen(str);
            if (c > 0x10000 && (int)d > 0) {
                if (n >= (int)d) n = (int)d - 1;
                memcpy((void *)c, str, n);
                ((char *)c)[n] = 0;
            }
            return (uint64_t)(uint32_t)n;
        }
        break;
    case R_GETDESC: /* (handle, type, index, data, length) */
        if (a == FAKE_HANDLE)
            return (uint64_t)(uint32_t)control_in(0x80, 0x06, (uint16_t)(((b & 0xFF) << 8) | (c & 0xFF)), (uint8_t *)d, (int)e);
        break;
    case R_ALLOC:
    case R_FILL:
        break;
    default:
        if (a == FAKE_DEV || a == FAKE_HANDLE || b == FAKE_DEV || b == FAKE_HANDLE || in_fake_mem(a) ||
            in_fake_mem(b) || tracked(a) || tracked(b)) {
            if (s->cnt > 3)
                sceKernelUsleep(1000); /* calls that poll in a loop: do not spin */
            return 0;
        }
        break;
    }
    return ((raw8_fn)s->orig)(a, b, c, d, e, f, g, h);
}

#define GW(N)                                                                                      \
    static uint64_t gw##N(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,  \
                          uint64_t g, uint64_t h)                                                  \
    {                                                                                              \
        return generic_call(N, a, b, c, d, e, f, g, h);                                            \
    }
GW(0) GW(1) GW(2) GW(3) GW(4) GW(5) GW(6) GW(7) GW(8) GW(9) GW(10) GW(11) GW(12) GW(13) GW(14)
GW(15) GW(16) GW(17) GW(18) GW(19) GW(20) GW(21) GW(22) GW(23) GW(24) GW(25) GW(26) GW(27) GW(28)
GW(29) GW(30) GW(31) GW(32) GW(33) GW(34) GW(35) GW(36) GW(37) GW(38) GW(39)
static const uint64_t g_gw[MAXSLOT] = {
    (uint64_t)gw0,  (uint64_t)gw1,  (uint64_t)gw2,  (uint64_t)gw3,  (uint64_t)gw4,  (uint64_t)gw5,
    (uint64_t)gw6,  (uint64_t)gw7,  (uint64_t)gw8,  (uint64_t)gw9,  (uint64_t)gw10, (uint64_t)gw11,
    (uint64_t)gw12, (uint64_t)gw13, (uint64_t)gw14, (uint64_t)gw15, (uint64_t)gw16, (uint64_t)gw17,
    (uint64_t)gw18, (uint64_t)gw19, (uint64_t)gw20, (uint64_t)gw21, (uint64_t)gw22, (uint64_t)gw23,
    (uint64_t)gw24, (uint64_t)gw25, (uint64_t)gw26, (uint64_t)gw27, (uint64_t)gw28, (uint64_t)gw29,
    (uint64_t)gw30, (uint64_t)gw31, (uint64_t)gw32, (uint64_t)gw33, (uint64_t)gw34, (uint64_t)gw35,
    (uint64_t)gw36, (uint64_t)gw37, (uint64_t)gw38, (uint64_t)gw39};

/* completes at most one cancelled, one OUT and one IN transfer. Returns how many were completed. */
static int deliver(void)
{
    OutEnt e;
    int have_out = 0, have_in = 0, have_cancel = 0, timed_out = 0, i, done = 0;
    uint64_t tin = 0, tc = 0;
    uint8_t rep[32];

    lock();
    if (g_ncancel > 0) { /* cancelled IN transfers finish with LIBUSB_TRANSFER_CANCELLED */
        tc = g_cancelq[0];
        memmove(g_cancelq, g_cancelq + 1, sizeof(uint64_t) * (g_ncancel - 1));
        g_ncancel--;
        for (i = 0; i < g_ninq; i++)
            if (g_inq[i] == tc) {
                memmove(g_inq + i, g_inq + i + 1, sizeof(uint64_t) * (g_ninq - i - 1));
                memmove(g_inq_tick + i, g_inq_tick + i + 1, sizeof(uint32_t) * (g_ninq - i - 1));
                g_ninq--;
                have_cancel = 1;
                break;
            }
    }
    if (g_noq > 0) {
        e = g_oq[0];
        memmove(g_oq, g_oq + 1, sizeof(OutEnt) * (g_noq - 1));
        g_noq--;
        have_out = 1;
    }
    if (g_ninq > 0) {
        if (pending_report()) {
            tin = g_inq[0];
            memmove(g_inq, g_inq + 1, sizeof(uint64_t) * (g_ninq - 1));
            memmove(g_inq_tick, g_inq_tick + 1, sizeof(uint32_t) * (g_ninq - 1));
            g_ninq--;
            next_report(rep);
            have_in = 1;
        } else if (g_tick - g_inq_tick[0] > IN_TIMEOUT_TICKS) { /* nothing to say for a while: time it out */
            tin = g_inq[0];
            memmove(g_inq, g_inq + 1, sizeof(uint64_t) * (g_ninq - 1));
            memmove(g_inq_tick, g_inq_tick + 1, sizeof(uint32_t) * (g_ninq - 1));
            g_ninq--;
            have_in = 1;
            timed_out = 1;
        }
    }
    unlock();

    if (have_cancel) {
        finish_transfer(tc, 3, 0);
        done++;
    }
    if (have_out) {
        if (e.is_cmd) {
            lock();
            do_command(e.d, e.cmdlen);
            unlock();
        }
        finish_transfer(e.t, 0, e.actual);
        done++;
    }
    if (have_in) {
        uint8_t *buf = (uint8_t *)*(uint64_t *)(tin + 48);
        int len = *(int32_t *)(tin + 20);
        int n = len < 32 ? len : 32;
        if (timed_out) {
            finish_transfer(tin, 2, 0); /* LIBUSB_TRANSFER_TIMED_OUT */
        } else {
            if ((uint64_t)buf > 0x10000 && n > 0)
                memcpy(buf, rep, n);
            finish_transfer(tin, 0, n);
        }
        done++;
    }
    return done;
}

static int he_mode(void) { return g_he_seen && g_tick - g_he_tick < 500; }

/* worker: saves progress, and completes transfers when the game is not doing it through HandleEvents */
static void *worker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sceKernelUsleep(2000);
        g_tick++;
        if (g_tick % 250 == 0)
            flush_dirty(); /* ~0.5 s */
        if (!he_mode())
            deliver();
    }
    return NULL;
}

/* the game's sceUsbdHandleEvents*(): complete our transfers here, on the game's thread */
static uint64_t handle_events_hook(void)
{
    static int logged;
    if (!logged++)
        logf_("game calls HandleEvents: completing pad transfers on the game's thread from now on");
    g_he_tick = g_tick;
    g_he_seen = 1;
    if (deliver())
        return 0;
    sceKernelUsleep(1000); /* nothing ready yet: wait a moment, like a short USB event wait */
    deliver();
    return 0;
}

/* ------------------------------------------------------------------ pointer swapping */

/* our own copy of the kernel's virtual-query result: the field names differ between OpenOrbis versions */
typedef struct {
    uint64_t start_addr, end_addr, offset;
    int32_t prot, mtype;
    uint32_t flags;
    char name[32];
} VQInfo;

static int name_of(uint64_t v)
{
    int k;
    for (k = 0; k < NFN; k++)
        if (g_real[k] && g_real[k] == v)
            return k;
    return -1;
}

static int role_of(uint64_t v)
{
    int k;
    for (k = 0; k < NFN; k++)
        if (g_real[k] && g_real[k] == v)
            return g_names[k].role;
    /* fallback found by the Infinity plugin on FW 11.52, only used if the name lookup failed */
    if (!g_real[2] && v - g_usbd_lo == 0x410) return R_FREE;
    if (!g_real[18] && v - g_usbd_lo == 0x430) return R_REF;
    if (!g_real[19] && v - g_usbd_lo == 0x520) return R_GETCFG;
    if (!g_real[20] && v - g_usbd_lo == 0x6c0) return R_ACFG;
    return R_OTHER;
}

static void install_swaps(void)
{
    OrbisKernelModuleInfo info;
    uint32_t sg;
    int i, nroles[32];

    memset(&info, 0, sizeof(info));
    memset(nroles, 0, sizeof(nroles));
    info.size = sizeof(info);
    if (sceKernelGetModuleInfo(0, &info) != 0) {
        logf_("could not read the game's module info");
        return;
    }
    for (sg = 0; sg < info.segmentCount && sg < 4; sg++)
        if (info.segmentInfo[sg].prot & 4) {
            g_text_lo = (uint64_t)info.segmentInfo[sg].address;
            g_text_hi = g_text_lo + info.segmentInfo[sg].size;
        }
    for (sg = 0; sg < info.segmentCount && sg < 4; sg++) {
        VQInfo vq;
        uint64_t base = (uint64_t)info.segmentInfo[sg].address;
        uint64_t limit = base + info.segmentInfo[sg].size, j;
        volatile uint64_t *p = (volatile uint64_t *)base;
        memset(&vq, 0, sizeof(vq));
        if (sceKernelVirtualQuery((const void *)base, 0, (void *)&vq, sizeof(vq)) != 0)
            continue;
        if (!(vq.prot & 1) || (vq.prot & 4))
            continue; /* readable, non-executable only */
        if ((uint64_t)vq.end_addr > limit)
            limit = (uint64_t)vq.end_addr < limit + 0x4000000ULL ? (uint64_t)vq.end_addr : limit + 0x4000000ULL;
        for (j = 0; j < (limit - base) / 8; j++) {
            uint64_t v = p[j];
            if (v >= g_usbd_lo && v < g_usbd_hi && g_nslot < MAXSLOT) {
                Slot *s = &g_slot[g_nslot++];
                s->orig = v;
                s->slot_addr = base + j * 8;
                s->role = role_of(v);
                s->name = name_of(v);
                nroles[s->role]++;
            }
        }
    }
    logf_("found %d USB function pointers (list %d, open %d, vidpid %d, submit %d, ctrl %d, intr %d)", g_nslot,
          nroles[R_LIST], nroles[R_OPEN], nroles[R_VIDPID], nroles[R_SUBMIT], nroles[R_CTRL], nroles[R_INTR]);
    refresh_exec_ranges();
    if (!g_nslot)
        return;
    for (i = 0; i < g_nslot; i++) {
        Slot *s = &g_slot[i];
        uint64_t page = s->slot_addr & ~0xFFFULL;
        VQInfo vq;
        int writable, r1 = 0;
        memset(&vq, 0, sizeof(vq));
        sceKernelVirtualQuery((const void *)s->slot_addr, 0, (void *)&vq, sizeof(vq));
        writable = (vq.prot & 2) != 0;
        if (!writable)
            r1 = sceKernelMprotect((const void *)page, 0x1000, 3);
        if (r1 == 0)
            *(volatile uint64_t *)s->slot_addr = g_gw[i];
        else
            logf_("could not unprotect page %llx", (unsigned long long)page);
        if (!writable && r1 == 0)
            sceKernelMprotect((const void *)page, 0x1000, vq.prot & 7);
    }
    g_installed = 1;
}

static void check_for_usbd(void)
{
    OrbisKernelModule h[256];
    size_t n = 0, i;
    if (sceKernelGetModuleList(h, 256, &n) != 0)
        return;
    for (i = 0; i < n && i < 256; i++) {
        OrbisKernelModuleInfo info;
        uint32_t s;
        int k, found = 0;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(h[i], &info) != 0 || !contains_ci(info.name, "libsceusbd"))
            continue;
        for (s = 0; s < info.segmentCount && s < 4; s++) {
            uint64_t a = (uint64_t)info.segmentInfo[s].address;
            uint64_t b = a + info.segmentInfo[s].size;
            if (!g_usbd_lo || a < g_usbd_lo) g_usbd_lo = a;
            if (b > g_usbd_hi) g_usbd_hi = b;
        }
        for (k = 0; k < NFN; k++) {
            void *addr = NULL;
            sceKernelDlsym((int32_t)h[i], g_names[k].name, &addr);
            g_real[k] = (uint64_t)addr;
            if (addr) found++;
        }
        logf_("libSceUsbd found, %d/%d functions resolved by name", found, NFN);
        install_swaps();
        return;
    }
}

static void *watch_thread(void *arg)
{
    int tick;
    (void)arg;
    for (tick = 0; tick < 6000 && !g_installed; tick++) { /* up to 10 minutes */
        sceKernelUsleep(100000);
        check_for_usbd();
    }
    if (!g_installed)
        logf_("gave up: the game never loaded libSceUsbd");
    return NULL;
}

/* ------------------------------------------------------------------ web server (phone UI) */

static void http_send(int c, const void *p, size_t n)
{
    const char *q = (const char *)p;
    while (n) {
        int r = send(c, q, n, 0x20000); /* MSG_NOSIGNAL */
        if (r <= 0)
            return;
        q += r;
        n -= (size_t)r;
    }
}

static void http_head(int c, const char *code, const char *type, int len)
{
    char h[256];
    int n;
    if (len >= 0)
        n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, type, len);
    else
        n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, type);
    http_send(c, h, n);
}

/* small buffered writer, so the state (with the whole library list) is sent in pieces */
typedef struct { int c; int n; char b[1024]; } Out;

static void out_flush(Out *o)
{
    if (o->n) {
        http_send(o->c, o->b, o->n);
        o->n = 0;
    }
}

static void out_raw(Out *o, const char *s, int len)
{
    if (o->n + len > (int)sizeof(o->b))
        out_flush(o);
    if (len > (int)sizeof(o->b)) {
        http_send(o->c, s, len);
        return;
    }
    memcpy(o->b + o->n, s, len);
    o->n += len;
}

static void out_s(Out *o, const char *s) { out_raw(o, s, (int)strlen(s)); }

/* writes s as a JSON string (quotes and backslashes escaped) */
static void out_json(Out *o, const char *s)
{
    char tmp[2];
    out_s(o, "\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)ch;
            out_raw(o, tmp, 2);
        } else if (ch < 32) {
            out_raw(o, " ", 1);
        } else {
            out_raw(o, (const char *)&s[0], 1);
        }
    }
    out_s(o, "\"");
}

static void send_state(int c, const char *msg)
{
    char names[LEGO_SLOTS][512];
    int has[LEGO_SLOTS];
    int s, i;
    Out o;

    lock();
    for (s = 0; s < LEGO_SLOTS; s++) {
        has[s] = g_pad.fig[s].present && g_slot_lib[s] >= 0;
        snprintf(names[s], sizeof(names[s]), "%s", has[s] ? lib_rel(g_slot_lib[s]) : "");
    }
    unlock();

    http_head(c, "200 OK", "application/json; charset=utf-8", -1);
    o.c = c;
    o.n = 0;
    out_s(&o, "{\"ver\":\"" PLUGIN_VERSION_STR "\",\"game\":");
    out_s(&o, g_game_seen ? "true" : "false");
    out_s(&o, ",\"usb\":");
    out_s(&o, g_installed ? "true" : "false");
    out_s(&o, ",\"msg\":");
    out_json(&o, msg ? msg : "");
    out_s(&o, ",\"slots\":[");
    for (s = 0; s < LEGO_SLOTS; s++) {
        if (s) out_s(&o, ",");
        if (has[s]) out_json(&o, names[s]);
        else out_s(&o, "null");
    }
    out_s(&o, "],\"figures\":[");
    for (i = 0; i < g_lib_n; i++) {
        if (i) out_s(&o, ",");
        out_json(&o, lib_rel(i));
    }
    out_s(&o, "]}");
    out_flush(&o);
}

static void send_text(int c, const char *code, const char *type, const char *b, int n)
{
    http_head(c, code, type, n);
    http_send(c, b, n);
}

static const char *query_str(const char *q, const char *key, char *out, int max)
{
    size_t kl = strlen(key);
    while (*q) {
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            const char *p = q + kl + 1;
            int n = 0;
            while (*p && *p != '&' && n < max - 1) {
                if (*p == '%' && p[1] && p[2]) {
                    char hx[3] = {p[1], p[2], 0};
                    out[n++] = (char)strtol(hx, NULL, 16);
                    p += 3;
                } else {
                    out[n++] = (*p == '+') ? ' ' : *p;
                    p++;
                }
            }
            out[n] = 0;
            return out;
        }
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
    return NULL;
}

/* reads the request body (Content-Length bytes). body gets max bytes plus a NUL. Returns the body length or -1. */
static int get_body(int c, const char *req, int got, uint8_t *body, int max)
{
    char *hend = strstr(req, "\r\n\r\n"), *cl;
    int clen = 0, header_len, have, r;
    if (!hend)
        return -1;
    header_len = (int)(hend - req) + 4;
    hend[0] = 0; /* so the search below sees only the headers */
    cl = strstr(req, "Content-Length:");
    if (!cl) cl = strstr(req, "content-length:");
    if (cl) clen = atoi(cl + 15);
    hend[0] = '\r';
    if (clen < 0 || clen > max)
        return -1;
    have = got - header_len;
    if (have > clen) have = clen;
    if (have > 0)
        memcpy(body, req + header_len, have);
    while (have < clen) {
        r = recv(c, body + have, clen - have, 0);
        if (r <= 0)
            break;
        have += r;
    }
    body[have] = 0;
    return have == clen ? clen : -1;
}

/* finds "key": and returns a pointer to its value */
static const char *json_val(const char *b, const char *key)
{
    char pat[40];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(b, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static int json_int(const char *b, const char *key, int def)
{
    const char *p = json_val(b, key);
    return p ? atoi(p) : def;
}

static int json_str(const char *b, const char *key, char *out, int max)
{
    const char *p = json_val(b, key);
    int n = 0;
    if (!p || *p != '"')
        return 0;
    p++;
    while (*p && *p != '"' && n < max - 1) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n;
}

/* POST /api/upload?name=Spyro.bin  body = the 180-byte dump */
static void do_upload(int c, const char *query, const char *req, int got)
{
    static uint8_t body[LEGO_SIZE + 16] IN_DATA;
    char name[128], path[256];
    int i, j = 0, clen;
    clen = get_body(c, req, got, body, LEGO_SIZE);
    if (clen != LEGO_SIZE) {
        send_state(c, "O arquivo precisa ter exatamente 180 bytes");
        return;
    }
    if (!query || !query_str(query, "name", name, sizeof(name)))
        name[0] = 0;
    for (i = 0; name[i] && j < 100; i++) { /* keep the name safe: no folders, no odd characters */
        char ch = name[i];
        if (ch == '/' || ch == '\\' || ch == ':' || (unsigned char)ch < 32) continue;
        if (ch == '.' && j == 0) continue;
        name[j++] = ch;
    }
    name[j] = 0;
    if (!j) snprintf(name, sizeof(name), "figure.bin");
    if (!ends_bin(name) && j < 100) strcat(name, ".bin");
    sceKernelMkdir(DIR_UPLOAD, 0777);
    snprintf(path, sizeof(path), DIR_UPLOAD "/%s", name);
    if (write_file(path, body, LEGO_SIZE) != LEGO_SIZE) {
        send_state(c, "Não consegui gravar o arquivo");
        return;
    }
    logf_("uploaded %s", name);
    rescan();
    send_state(c, "Figura enviada");
}

static void http_handle(int c, char *req, int n);

static void http_serve(int c)
{
    char req[4096 + LEGO_SIZE]; /* each connection has its own thread, so its own buffer */
    int n = 0;
    while (n < (int)sizeof(req) - 1) {
        int r = recv(c, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) break;
        n += r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (n <= 0)
        return;
    req[n] = 0;
    http_handle(c, req, n);
}

/* requests are handled one at a time: the pad state is shared */
static volatile int g_oplock_v;
static void oplock(void) { while (__sync_lock_test_and_set(&g_oplock_v, 1)) sceKernelUsleep(500); }
static void opunlock(void) { __sync_lock_release(&g_oplock_v); }

static void http_route(int c, char *req, int n)
{
    char line[512], *p, *query = NULL;
    int k = 0, post;
    while (k < (int)sizeof(line) - 1 && k < n && req[k] != '\r' && req[k] != '\n') {
        line[k] = req[k];
        k++;
    }
    line[k] = 0;
    post = strncmp(line, "POST ", 5) == 0;
    if (!post && strncmp(line, "GET ", 4) != 0) {
        send_text(c, "404 Not Found", "text/plain", "", 0);
        return;
    }
    p = line + (post ? 5 : 4);
    {
        char *sp = strchr(p, ' ');
        if (sp) *sp = 0;
    }
    query = strchr(p, '?');
    if (query) *query++ = 0;

    if (!post) {
        if (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0) {
            http_head(c, "200 OK", "text/html; charset=utf-8", (int)sizeof(WEB_PAGE));
            http_send(c, WEB_PAGE, sizeof(WEB_PAGE));
        } else if (strcmp(p, "/api/state") == 0) {
            send_state(c, NULL);
        } else if (strcmp(p, "/api/log") == 0) {
            static char copy[8192] IN_DATA;
            int len;
            lock();
            len = g_loglen;
            memcpy(copy, g_log, len);
            unlock();
            send_text(c, "200 OK", "text/plain; charset=utf-8", copy, len);
        } else {
            send_text(c, "404 Not Found", "text/plain", "", 0);
        }
        return;
    }

    if (strcmp(p, "/api/upload") == 0) {
        do_upload(c, query, req, n);
    } else if (strcmp(p, "/api/place") == 0) {
        static uint8_t jb[512] IN_DATA;
        char name[256] = "";
        int slot, id = -1, i, r;
        if (get_body(c, req, n, jb, sizeof(jb) - 1) < 0) {
            send_state(c, "Pedido inválido");
            return;
        }
        slot = json_int((const char *)jb, "slot", -1);
        json_str((const char *)jb, "name", name, sizeof(name));
        for (i = 0; i < g_lib_n; i++)
            if (strcmp(lib_rel(i), name) == 0) { id = i; break; }
        r = place_figure(slot, id);
        send_state(c, r == 0 ? "" :
                   r == -3 ? "Posição ocupada: tire a figura antes" :
                   r == -2 ? "Esse arquivo não é uma figura de 180 bytes" :
                             "Figura ou posição inválida");
    } else if (strcmp(p, "/api/remove") == 0) {
        static uint8_t jb[256] IN_DATA;
        if (get_body(c, req, n, jb, sizeof(jb) - 1) < 0) {
            send_state(c, "Pedido inválido");
            return;
        }
        remove_figure(json_int((const char *)jb, "slot", -1));
        send_state(c, NULL);
    } else {
        send_text(c, "404 Not Found", "text/plain", "", 0);
    }
}

static void http_handle(int c, char *req, int n)
{
    oplock();
    http_route(c, req, n);
    opunlock();
}

#define WEB_MAX_CONN 8
static volatile int g_web_active;

static void *conn_thread(void *arg)
{
    int c = (int)(intptr_t)arg;
    http_serve(c);
    close(c);
    __sync_fetch_and_sub(&g_web_active, 1);
    return NULL;
}

static void *web_thread(void *arg)
{
    struct { uint8_t len, fam; uint16_t port; uint32_t addr; uint8_t zero[8]; } sa;
    int s, one = 1;
    (void)arg;
    rescan();
    load_state();
    s = socket(2, 1, 0); /* AF_INET, SOCK_STREAM */
    if (s < 0) {
        logf_("web: socket failed");
        return NULL;
    }
    setsockopt(s, 0xffff, 4, &one, sizeof(one)); /* SO_REUSEADDR */
    memset(&sa, 0, sizeof(sa));
    sa.len = 16;
    sa.fam = 2;
    sa.port = (uint16_t)((WEB_PORT >> 8) | ((WEB_PORT & 0xff) << 8));
    sa.addr = 0;
    if (bind(s, &sa, sizeof(sa)) != 0 || listen(s, 8) != 0) {
        logf_("web: port %d busy (is another plugin using it?)", WEB_PORT);
        close(s);
        return NULL;
    }
    logf_("web page ready on port %d", WEB_PORT);
    for (;;) {
        struct { long sec; long usec; } tv = {5, 0};
        uint8_t peer[16] = {0};
        unsigned plen = sizeof(peer);
        int c = accept(s, peer, &plen);
        OrbisPthread th;
        OrbisPthreadAttr attr;
        if (c < 0) {
            sceKernelUsleep(100000);
            continue;
        }
        setsockopt(c, 0xffff, 0x1006, &tv, sizeof(tv));   /* SO_RCVTIMEO */
        setsockopt(c, 0xffff, 0x1005, &tv, sizeof(tv));   /* SO_SNDTIMEO */
        setsockopt(c, 0xffff, 0x800, &one, sizeof(one));  /* SO_NOSIGPIPE */
        /* phone browsers open extra connections and leave some idle: each one gets its own thread */
        if (g_web_active < WEB_MAX_CONN) {
            __sync_fetch_and_add(&g_web_active, 1);
            scePthreadAttrInit(&attr);
            scePthreadAttrSetstacksize(&attr, 96 * 1024);
            scePthreadAttrSetdetachstate(&attr, 1); /* PTHREAD_CREATE_DETACHED */
            if (scePthreadCreate(&th, &attr, conn_thread, (void *)(intptr_t)c, "lego_conn") == 0) {
                scePthreadAttrDestroy(&attr);
                continue;
            }
            scePthreadAttrDestroy(&attr);
            __sync_fetch_and_sub(&g_web_active, 1);
        }
        http_serve(c); /* too many at once: serve it here */
        close(c);
    }
    return NULL;
}

/* ------------------------------------------------------------------ plugin entry points */

attr_public int32_t plugin_load(int32_t argc, const char *argv[])
{
    OrbisPthread th;
    (void)argc;
    (void)argv;

    sceKernelMkdir(DIR_MAIN, 0777);
    sceKernelMkdir(DIR_FIGS, 0777);
    sceKernelMkdir(DIR_SAVES, 0777);
    g_logfd = sceKernelOpen(DIR_MAIN "/log.txt", 0x601, 0666);
    logf_("lego_toypad " PLUGIN_VERSION_STR " loaded");

    lego_init(&g_pad);
    slot_lib_init();
    g_pool = (char *)malloc(POOL_SIZE);
    g_lib_off = (int *)malloc(sizeof(int) * LIB_MAX);
    if (!g_pool || !g_lib_off)
        logf_("out of memory for the figure library");

    {
        OrbisPthreadAttr attr; /* bigger stacks: folder scanning and the web pages use a few KB each */
        scePthreadAttrInit(&attr);
        scePthreadAttrSetstacksize(&attr, 256 * 1024);
        scePthreadCreate(&th, &attr, watch_thread, NULL, "lego_watch");
        scePthreadCreate(&th, &attr, worker_thread, NULL, "lego_worker");
        scePthreadCreate(&th, &attr, web_thread, NULL, "lego_web");
        scePthreadAttrDestroy(&attr);
    }
    return 0;
}

attr_public int32_t plugin_unload(int32_t argc, const char *argv[])
{
    int s;
    (void)argc;
    (void)argv;
    for (s = 0; s < LEGO_SLOTS; s++)
        flush_slot(s);
    return 0;
}

int32_t module_start(size_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}

int32_t module_stop(size_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}
