/* sim_web.c: optional embedded web server, WebSocket and REST API

   Copyright (c) 2026, the OpenSIMH contributors

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   This module is compiled only when USE_WEBSERVER is defined.

   Usage:

        sim> SET WEB TOKEN=secret       (optional shared secret)
        sim> SET WEB MEDIA=/srv/media   (directory the media browser may list)
        sim> ATTACH WEB 8080            (binds 127.0.0.1:8080 only)
        sim> ATTACH WEB 0.0.0.0:8080    (explicitly listen on all interfaces)
        sim> BOOT ...                   (or leave the machine stopped: start it from the web)

   Design:

   The server is single threaded and non-blocking, so it needs no locking.
   It is serviced from the simulator's main thread in two places:

     - while the simulation is running, by an event-queue service routine
       that fires every 20 ms of wall-clock time;
     - while the simulator is stopped, by sim_web_prompt_wait(), which the
       sim> command loop calls instead of blocking on stdin.  Commands from
       the web (CONTINUE, RUN, BOOT, RESET, STEP, any SCP command) are handed
       to that loop and executed exactly as if typed at the console.

   The guest console is available as a terminal: output written through
   sim_putchar is pushed as "console" events (with a replay of recent history
   for new clients) and {"cmd":"input","data":".."} feeds sim_poll_kbd, so
   you can log in to the guest from the browser while it runs.  The WRU
   character (^E) typed there stops the simulation, as at a real console.

   "mediaio" events carry the read/write operation counts of every loaded
   disk and tape unit (pushed when they change).

   {"cmd":"history","count":20,"enable":true} switches the CPU's instruction
   history on (SET CPU HISTORY) and subscribes to "history" events with the
   last 20 records (the CPU's own SHOW CPU HISTORY output).

   The web interface is therefore live in every state: stopped, running and
   paused.  PAUSE holds the simulation inside the service routine, still
   servicing web requests, so RESUME, STEP and examine/deposit work while the
   emulated machine is frozen.  HALT stops the run and returns to sim>.

   While WEB is attached the sim> prompt reads stdin non-blocking (no line
   editing/history), and EOF on a non-tty stdin no longer exits the simulator,
   so it can run headless (pdp11 foo.ini </dev/null) and be driven from the
   web.  Use the web "command" endpoint with "exit" (or kill it) to quit.

   REST API (JSON):

        GET  /                          Blinkenlights front panel (HTML)
        GET  /api/state                 run state, PC, status, R0-R15, lights
        GET  /api/registers[?device=D]  all registers of a device (default CPU)
        PUT  /api/registers/NAME        {"value":..,"index":..}  (stopped/paused)
        GET  /api/devices               devices, units and attached files
        POST /api/attach | detach       {"unit":"RQ0","file":..,"readonly":..,"new":..}
                                        (any state)
        GET  /api/media[?path=sub]      list files under the MEDIA directory
        GET  /api/network               Ethernet adapters: MAC, counters
        GET  /api/memory?addr=A&count=N physical memory (RAM only)
        PUT  /api/memory                {"addr":..,"value":..}   (stopped/paused)
        POST /api/pause | resume | continue | step | halt
                                        (running/paused; resume and step also
                                        work when stopped; step takes "count")
        POST /api/go | run              {"addr":..} optional      (stopped only)
                                        (RUN resets devices and resumes at the PC;
                                        it does not reboot the guest)
        POST /api/restart               halt if needed, then BOOT the selected boot
                                        device (or first attached disk)
        POST /api/bootdev               {"device":"RQ0"}: choose the boot device
        POST /api/boot                  {"device":"RQ0"}          (stopped only)
        POST /api/reset                                           (stopped only)
        POST /api/command               {"command":"attach rq0 x.dsk"} (stopped)
        GET  /ws                        WebSocket.  The server pushes "state"
                                        (~10/s), and "devices" / "network"
                                        whenever they change.  Requests
                                        ({"cmd":"pause"}, "media", "memory",
                                        "deposit", "attach", "detach", "run",
                                        "boot", "command", "input", "history" ...) are answered
                                        by "media"/"memory"/"result"/"error"
                                        events.  The web page uses only this.

   Numbers in requests may be JSON numbers (decimal) or strings; strings are
   hexadecimal with a 0x prefix, otherwise in the radix of the target.

   Security: listens on loopback unless told otherwise; rejects cross-origin
   requests and (on loopback) foreign Host headers to defeat DNS rebinding;
   optionally requires a token ("Authorization: Bearer" or ?token=).
*/

#include "sim_defs.h"

#ifdef USE_WEBSERVER

#include <ctype.h>
#include <stdarg.h>
#if !defined (_WIN32)
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#endif
#include "sim_web.h"
#include "sim_sock.h"
#include "sim_timer.h"
#include "sim_console.h"
#include "sim_web_page.h"

extern void put_rval (REG *rptr, uint32 idx, t_value val);
extern void sim_disk_io_counts (UNIT *uptr, uint32 *reads, uint32 *writes);   /* sim_disk.c (USE_WEBSERVER) */
extern void sim_tape_io_counts (UNIT *uptr, uint32 *reads, uint32 *writes);   /* sim_tape.c (USE_WEBSERVER) */

#define WEB_MAX_CLIENTS     8
#define WEB_RX_MAX          (64 * 1024)         /* request header + body limit */
#define WEB_TX_MAX          (1024 * 1024)       /* queued output limit per client */
#define WEB_WS_MAX          4096                /* WebSocket message limit */
#define WEB_POLL_USEC       20000               /* service interval */
#define WEB_PUSH_MSEC       100                 /* WebSocket state push interval */
#define WEB_EVENT_MSEC      500                 /* devices / network change check interval */
#define WEB_MEM_MAX         512                 /* max values per memory read */
#define WEB_REG_DEPTH_MAX   1024                /* max elements shown per register */

/* ---------------------------------------------------------------------- */
/* Growable string buffer                                                  */
/* ---------------------------------------------------------------------- */

typedef struct {
    char    *p;
    size_t  len;
    size_t  cap;
    t_bool  err;
    } SB;

static void sb_reserve (SB *sb, size_t add)
{
char *np;
size_t ncap;

if (sb->err)
    return;
if (sb->len + add + 1 <= sb->cap)
    return;
ncap = (sb->cap == 0) ? 1024 : sb->cap;
while (ncap < sb->len + add + 1)
    ncap *= 2;
np = (char *)realloc (sb->p, ncap);
if (np == NULL) {
    sb->err = TRUE;
    return;
    }
sb->p = np;
sb->cap = ncap;
}

static void sb_put (SB *sb, const char *s, size_t n)
{
sb_reserve (sb, n);
if (sb->err)
    return;
memcpy (sb->p + sb->len, s, n);
sb->len += n;
sb->p[sb->len] = '\0';
}

static void sb_puts (SB *sb, const char *s)
{
sb_put (sb, s, strlen (s));
}

static void sb_printf (SB *sb, const char *fmt, ...)
{
char tmp[512];
va_list ap;
int n;

va_start (ap, fmt);
n = vsnprintf (tmp, sizeof (tmp), fmt, ap);
va_end (ap);
if (n < 0)
    return;
if ((size_t)n >= sizeof (tmp))
    n = (int)sizeof (tmp) - 1;                          /* callers keep items small */
sb_put (sb, tmp, (size_t)n);
}

static void sb_json_str (SB *sb, const char *s)
{
sb_puts (sb, "\"");
for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;

    if (c == '"' || c == '\\')
        sb_printf (sb, "\\%c", c);
    else if (c < 0x20)
        sb_printf (sb, "\\u%04x", c);
    else
        sb_put (sb, (const char *)s, 1);
    }
sb_puts (sb, "\"");
}

/* Values beyond 2**53 cannot be held exactly by a JavaScript number */

static void sb_json_val (SB *sb, t_value v)
{
unsigned long long u = (unsigned long long)v;

if (u > 0x1FFFFFFFFFFFFFULL)
    sb_printf (sb, "\"0x%" LL_FMT "x\"", u);
else
    sb_printf (sb, "%" LL_FMT "u", u);
}

/* ---------------------------------------------------------------------- */
/* SHA-1 and base64 (needed only for the WebSocket opening handshake)      */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32  h[5];
    uint32  w[80];
    unsigned char buf[64];
    size_t  fill;
    t_uint64  total;
    } SHA1_CTX;

#define ROL32(v,n)  (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block (SHA1_CTX *c, const unsigned char *b)
{
uint32 a, bb, cc, d, e, f, k, t;
int i;

for (i = 0; i < 16; i++)
    c->w[i] = ((uint32)b[4*i] << 24) | ((uint32)b[4*i+1] << 16) |
              ((uint32)b[4*i+2] << 8) | (uint32)b[4*i+3];
for (i = 16; i < 80; i++)
    c->w[i] = ROL32 (c->w[i-3] ^ c->w[i-8] ^ c->w[i-14] ^ c->w[i-16], 1);
a = c->h[0]; bb = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
for (i = 0; i < 80; i++) {
    if (i < 20)      { f = (bb & cc) | (~bb & d);          k = 0x5A827999; }
    else if (i < 40) { f = bb ^ cc ^ d;                    k = 0x6ED9EBA1; }
    else if (i < 60) { f = (bb & cc) | (bb & d) | (cc & d); k = 0x8F1BBCDC; }
    else             { f = bb ^ cc ^ d;                    k = 0xCA62C1D6; }
    t = ROL32 (a, 5) + f + e + k + c->w[i];
    e = d; d = cc; cc = ROL32 (bb, 30); bb = a; a = t;
    }
c->h[0] += a; c->h[1] += bb; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init (SHA1_CTX *c)
{
c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
c->fill = 0;
c->total = 0;
}

static void sha1_update (SHA1_CTX *c, const unsigned char *d, size_t n)
{
c->total += n;
while (n > 0) {
    size_t take = 64 - c->fill;

    if (take > n)
        take = n;
    memcpy (c->buf + c->fill, d, take);
    c->fill += take;
    d += take;
    n -= take;
    if (c->fill == 64) {
        sha1_block (c, c->buf);
        c->fill = 0;
        }
    }
}

static void sha1_final (SHA1_CTX *c, unsigned char out[20])
{
t_uint64 bits = c->total * 8;
unsigned char pad = 0x80;
unsigned char zero = 0;
unsigned char len[8];
int i;

sha1_update (c, &pad, 1);
while (c->fill != 56)
    sha1_update (c, &zero, 1);
for (i = 0; i < 8; i++)
    len[i] = (unsigned char)(bits >> (56 - 8 * i));
sha1_update (c, len, 8);
for (i = 0; i < 20; i++)
    out[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

static void base64_encode (const unsigned char *in, size_t n, char *out)
{
static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
size_t i;

for (i = 0; i + 2 < n; i += 3) {
    uint32 v = ((uint32)in[i] << 16) | ((uint32)in[i+1] << 8) | in[i+2];

    *out++ = tab[(v >> 18) & 63]; *out++ = tab[(v >> 12) & 63];
    *out++ = tab[(v >> 6) & 63];  *out++ = tab[v & 63];
    }
if (i < n) {
    uint32 v = (uint32)in[i] << 16;

    if (i + 1 < n)
        v |= (uint32)in[i+1] << 8;
    *out++ = tab[(v >> 18) & 63];
    *out++ = tab[(v >> 12) & 63];
    *out++ = (i + 1 < n) ? tab[(v >> 6) & 63] : '=';
    *out++ = '=';
    }
*out = '\0';
}

/* ---------------------------------------------------------------------- */
/* Server state                                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
    SOCKET      sock;                                   /* INVALID_SOCKET if free */
    t_bool      ws;                                     /* upgraded to WebSocket */
    t_bool      close_after;                            /* close once tx drained */
    char        *rx;
    size_t      rxlen;
    size_t      rxcap;
    char        *tx;
    size_t      txlen;
    size_t      txoff;
    uint32      last_push;
    uint32      serial;                                 /* identifies this connection */
    uint32      dev_ver;                                /* last devices/network event versions sent */
    uint32      net_ver;
    t_bool      con_init;                               /* console history sent */
    uint32      io_ver;                                 /* media I/O event version sent */
    uint32      hist_n;                                 /* instruction-history subscription: records wanted */
    uint32      hist_ver;
    } WEB_CLIENT;

static WEB_CLIENT web_clients[WEB_MAX_CLIENTS];
static SOCKET web_master = INVALID_SOCKET;
static t_bool web_loopback = TRUE;                      /* bound to loopback only */
static char web_token[64] = "";
static char web_media[256] = ".";                        /* root of the media browser */
static t_bool web_paused = FALSE;
static t_bool web_step_pending = FALSE;
static t_bool web_halt_req = FALSE;
static char web_cmd[512];                               /* command injected into the sim> loop */
static t_bool web_cmd_pending = FALSE;
static int web_reply_slot = -1;                         /* client awaiting the result of web_cmd */
static uint32 web_reply_serial = 0;
static uint32 web_serial = 0;
#define WEB_HIST_MSEC       250                 /* instruction-history refresh interval */
#define WEB_HIST_SIZE       "128"                /* records kept while the web has it switched on */
static char *web_last_hist = NULL;
static size_t web_last_hist_len = 0;
static uint32 web_hist_ver = 0, web_hist_count = 0, web_last_hist_time = 0;
static char *web_last_io = NULL;
static size_t web_last_io_len = 0;
static uint32 web_io_ver = 0, web_last_io_time = 0;
#define WEB_IO_MSEC         250                 /* media I/O counter refresh interval */
static char web_last_boot[32] = "";                   /* device of the last BOOT, for Restart */
static t_bool web_restart_pending = FALSE;              /* halting, then BOOT */
static t_bool web_stdin_eof = FALSE;                    /* stdin closed: keep serving the web */
static uint32 web_last_push = 0;

static t_stat web_svc (UNIT *uptr);
static t_stat web_attach (UNIT *uptr, CONST char *cptr);
static t_stat web_detach (UNIT *uptr);
static t_stat web_reset (DEVICE *dptr);
static t_stat web_set_token (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat web_set_media (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat web_show_media (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
static t_stat web_show_status (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
static t_stat web_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
static const char *web_description (DEVICE *dptr);
static void web_poll (void);
static void web_broadcast_state (t_bool force);
static void web_broadcast_events (void);
static void web_broadcast_console (void);
static void web_broadcast_history (void);
static void web_broadcast_mediaio (void);
static t_stat web_history_set (const char *arg);
static size_t web_con_queue (const unsigned char *d, size_t n);
static void web_finish_reply (t_bool started);

static UNIT web_unit = { UDATA (&web_svc, UNIT_ATTABLE, 0) };

static MTAB web_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR|MTAB_NC, 0, "TOKEN", "TOKEN=secret",
      &web_set_token, NULL, NULL, "Require this token on every request (empty to clear)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR|MTAB_NC, 0, "MEDIA", "MEDIA=directory",
      &web_set_media, &web_show_media, NULL, "Directory the web media browser may list (default .)" },
    { MTAB_XTD|MTAB_VDV|MTAB_NMO, 0, "STATUS", NULL,
      NULL, &web_show_status, NULL, "Show listener, clients and pause state" },
    { 0 }
    };

DEVICE web_dev = {
    "WEB", &web_unit, NULL, web_mod,
    1, 0, 0, 0, 0, 0,
    NULL, NULL, &web_reset, NULL, &web_attach, &web_detach,
    NULL, DEV_NOSAVE, 0,
    NULL, NULL, NULL, &web_help, NULL, NULL,
    &web_description
    };

/* ---------------------------------------------------------------------- */
/* Client plumbing                                                         */
/* ---------------------------------------------------------------------- */

static void client_close (WEB_CLIENT *c)
{
if (c->sock != INVALID_SOCKET)
    sim_close_sock (c->sock);
free (c->rx);
free (c->tx);
memset (c, 0, sizeof (*c));
c->sock = INVALID_SOCKET;
}

static void client_send (WEB_CLIENT *c, const char *data, size_t n)
{
char *nt;

if (c->sock == INVALID_SOCKET)
    return;
if (c->txoff == c->txlen)                               /* drained: restart buffer */
    c->txoff = c->txlen = 0;
if (c->txlen - c->txoff + n > WEB_TX_MAX) {             /* client not reading: drop it */
    client_close (c);
    return;
    }
nt = (char *)realloc (c->tx, c->txlen + n);
if (nt == NULL) {
    client_close (c);
    return;
    }
c->tx = nt;
memcpy (c->tx + c->txlen, data, n);
c->txlen += n;
}

static void client_flush (WEB_CLIENT *c)
{
while ((c->sock != INVALID_SOCKET) && (c->txoff < c->txlen)) {
    int n = sim_write_sock (c->sock, c->tx + c->txoff, (int)(c->txlen - c->txoff));

    if (n < 0) {
        client_close (c);
        return;
        }
    if (n == 0)                                         /* would block */
        break;
    c->txoff += (size_t)n;
    }
if ((c->sock != INVALID_SOCKET) && (c->txoff == c->txlen)) {
    c->txoff = c->txlen = 0;
    if (c->close_after)
        client_close (c);
    }
}

static void ws_send_text (WEB_CLIENT *c, const char *msg, size_t n)
{
unsigned char hdr[10];
size_t hl;

hdr[0] = 0x81;                                          /* FIN + text */
if (n < 126) {
    hdr[1] = (unsigned char)n;
    hl = 2;
    }
else if (n < 65536) {
    hdr[1] = 126;
    hdr[2] = (unsigned char)(n >> 8);
    hdr[3] = (unsigned char)n;
    hl = 4;
    }
else {
    int i;

    hdr[1] = 127;
    for (i = 0; i < 8; i++)
        hdr[2 + i] = (unsigned char)(((t_uint64)n) >> (56 - 8 * i));
    hl = 10;
    }
client_send (c, (const char *)hdr, hl);
client_send (c, msg, n);
}

/* ---------------------------------------------------------------------- */
/* Number and query parsing                                                */
/* ---------------------------------------------------------------------- */

static t_bool parse_num (const char *s, uint32 radix, t_value *val)
{
char *end;
unsigned long long v;
int base = (int)radix;

while (isspace ((unsigned char)*s))
    s++;
if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
    base = 16;
    s += 2;
    }
if ((*s == '\0') || (*s == '-') || (*s == '+'))
    return FALSE;
errno = 0;
v = strtoull (s, &end, base);
if ((errno != 0) || (*end != '\0'))
    return FALSE;
*val = (t_value)v;
return TRUE;
}

static int hexval (int c)
{
if ((c >= '0') && (c <= '9')) return c - '0';
if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
return -1;
}

/* Fetch a value from a query string ("a=1&b=2"), percent-decoded */

static t_bool query_get (const char *q, const char *key, char *out, size_t outsz)
{
size_t klen = strlen (key);

while (q && *q) {
    if ((strncmp (q, key, klen) == 0) && (q[klen] == '=' || q[klen] == '&' || q[klen] == '\0')) {
        const char *v = (q[klen] == '=') ? q + klen + 1 : "";
        size_t o = 0;

        while (*v && (*v != '&') && (o + 1 < outsz)) {
            if ((*v == '%') && (hexval (v[1]) >= 0) && (hexval (v[2]) >= 0)) {
                out[o++] = (char)(hexval (v[1]) * 16 + hexval (v[2]));
                v += 3;
                }
            else {
                out[o++] = (*v == '+') ? ' ' : *v;
                v++;
                }
            }
        out[o] = '\0';
        return TRUE;
        }
    q = strchr (q, '&');
    if (q)
        q++;
    }
return FALSE;
}

/* Minimal flat JSON extractor: finds "key": and returns the scalar after it
   as text (strings unquoted, numbers verbatim).  Sufficient for the small,
   flat request bodies this API accepts. */

/* Locate the value of "key": in a flat JSON object (the key, not a value
   that merely equals it).  Returns a pointer at the value, or NULL. */

static const char *json_value (const char *body, const char *key)
{
char pat[64];
const char *p = body;

if ((body == NULL) || (strlen (key) + 3 > sizeof (pat)))
    return NULL;
snprintf (pat, sizeof (pat), "\"%s\"", key);
for (;;) {
    p = strstr (p, pat);
    if (p == NULL)
        return NULL;
    p += strlen (pat);
    while (isspace ((unsigned char)*p)) p++;
    if (*p == ':')
        break;
    }
p++;
while (isspace ((unsigned char)*p)) p++;
return p;
}

static t_bool json_get (const char *body, const char *key, char *out, size_t outsz)
{
const char *p = json_value (body, key);
size_t o = 0;

if (p == NULL)
    return FALSE;
if (*p == '"') {
    p++;
    while (*p && (*p != '"') && (o + 1 < outsz)) {
        if ((*p == '\\') && p[1])
            p++;
        out[o++] = *p++;
        }
    }
else {
    while (*p && (*p != ',') && (*p != '}') && !isspace ((unsigned char)*p) && (o + 1 < outsz))
        out[o++] = *p++;
    }
out[o] = '\0';
return o > 0;
}

/* Like json_get for a string value, but decodes JSON escapes (\n \r \t \b \f
   \uXXXX ...) into bytes, so control characters typed in the web terminal
   arrive intact.  Returns the number of bytes decoded (may contain NULs). */

static size_t json_get_bytes (const char *body, const char *key, unsigned char *out, size_t outsz)
{
const char *p = json_value (body, key);
size_t o = 0;

if ((p == NULL) || (*p != '"'))
    return 0;
for (p++; *p && (*p != '"') && (o < outsz); p++) {
    if (*p != '\\') {
        out[o++] = (unsigned char)*p;
        continue;
        }
    p++;
    switch (*p) {
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'u': {
            unsigned v = 0;
            int i;

            for (i = 1; i <= 4; i++) {
                int h = hexval (p[i]);

                if (h < 0)
                    break;
                v = (v << 4) | (unsigned)h;
                }
            if (i == 5) {
                out[o++] = (unsigned char)(v & 0xFF);
                p += 4;
                }
            break;
            }
        case '\0': return o;
        default: out[o++] = (unsigned char)*p; break;     /* \" \\ \/ */
        }
    }
return o;
}

/* ---------------------------------------------------------------------- */
/* Simulator state -> JSON                                                 */
/* ---------------------------------------------------------------------- */

static REG *web_find_reg (const char *name, DEVICE *dptr)
{
REG *r;

if ((dptr == NULL) || (dptr->registers == NULL))
    return NULL;
for (r = dptr->registers; r->name != NULL; r++)
    if (strcasecmp (r->name, name) == 0)
        return r;
return NULL;
}

static const char *web_last_status (void)
{
t_stat stat = SCPE_BARE_STATUS (sim_last_cmd_stat);

if (stat == SCPE_OK)
    return "OK";
if (stat >= SCPE_BASE)
    return sim_error_text (stat);
return sim_stop_messages[stat] ? sim_stop_messages[stat] : "Stopped";
}

/* Remember the device of every BOOT (typed at the console, in a script, or
   sent from the web) so the web can offer a one-click Restart. */

void sim_web_note_command (const char *line)
{
const char *p = line;
size_t n = 0, i;
char word[8];

while (*p == ' ' || *p == '\t')
    p++;
while (p[n] && !isspace ((unsigned char)p[n]) && (n < sizeof (word) - 1)) {
    word[n] = (char)toupper ((unsigned char)p[n]);
    n++;
    }
word[n] = '\0';
if ((n < 2) || (strncmp ("BOOT", word, n) != 0) || (p[n] && !isspace ((unsigned char)p[n])))
    return;
p += n;
while (*p == ' ' || *p == '\t')
    p++;
while (*p == '-')                                       /* skip switches */
    {
    while (*p && !isspace ((unsigned char)*p)) p++;
    while (*p == ' ' || *p == '\t') p++;
    }
for (i = 0; p[i] && !isspace ((unsigned char)p[i]) && (i < sizeof (web_last_boot) - 1); i++)
    web_last_boot[i] = (char)toupper ((unsigned char)p[i]);
if (i > 0)
    web_last_boot[i] = '\0';
}

/* The device Restart will boot: the last one booted, else the first attached
   disk (then any other attached unit) whose device has a boot routine. */

static const char *web_boot_default (void)
{
static char found[64];
DEVICE *dptr;
uint32 i, j, pass;

if (web_last_boot[0])
    return web_last_boot;
for (pass = 0; pass < 2; pass++)
    for (i = 0; (dptr = sim_devices[i]) != NULL; i++) {
        if ((dptr->boot == NULL) || (dptr->flags & DEV_DIS))
            continue;
        if ((pass == 0) && (DEV_TYPE (dptr) != DEV_DISK))
            continue;
        for (j = 0; j < dptr->numunits; j++)
            if (dptr->units[j].flags & UNIT_ATT) {
                strlcpy (found, sim_uname (&dptr->units[j]), sizeof (found));
                return found;
                }
        }
return "";
}

static const char *web_run_state (void)
{
return !sim_is_running ? "stopped" : (web_paused ? "paused" : "running");
}

static void json_state (SB *sb)
{
DEVICE *dptr = sim_dflt_dev;
REG *pc = sim_PC;
REG *st = NULL;
t_bool first = TRUE;
unsigned long long dv = 0;
uint32 dwidth = 0, dbits, i, n, nclients = 0;
t_bool dvalid = FALSE;

sb_printf (sb, "{\"type\":\"state\",\"sim\":");
sb_json_str (sb, sim_name);
sb_printf (sb, ",\"state\":\"%s\",\"time\":%.0f", web_run_state (), sim_gtime ());

for (i = 0; i < WEB_MAX_CLIENTS; i++)
    if (web_clients[i].sock != INVALID_SOCKET)
        nclients++;
sb_puts (sb, ",\"boot\":");
sb_json_str (sb, web_boot_default ());
sb_printf (sb, ",\"clients\":%u,\"last_status\":", (unsigned)nclients);
sb_json_str (sb, web_last_status ());

if ((dptr != NULL) && (pc != NULL)) {
    t_value pcv = get_rval (pc, 0);
    uint32 abits = (dptr->awidth != 0) ? dptr->awidth : pc->width;

    /* Address lights follow the PC; data lights show the memory at the PC
       (the next instruction), up to 32 bits wide. */

    sb_printf (sb, ",\"pc\":{\"name\":");
    sb_json_str (sb, pc->name);
    sb_printf (sb, ",\"value\":");
    sb_json_val (sb, pcv);
    sb_printf (sb, ",\"width\":%u,\"radix\":%u}", (unsigned)pc->width, (unsigned)pc->radix);
    dwidth = dptr->dwidth;
    if ((dptr->examine != NULL) && (dwidth > 0) && (dwidth <= 32)) {
        n = (dwidth >= 16) ? 1 : (32 / dwidth);
        for (i = 0; i < n; i++) {
            t_value w;

            if (dptr->examine (&w, (t_addr)pcv + i * dptr->aincr, dptr->units,
                               SWMASK ('V') | SIM_SW_STOP) != SCPE_OK) {
                dvalid = FALSE;
                dv = 0;
                break;
                }
            dv |= ((unsigned long long)w & ((1ULL << dwidth) - 1)) << (i * dwidth);
            dvalid = TRUE;
            }
        dbits = n * dwidth;
        }
    else
        dbits = 0;
    sb_printf (sb, ",\"lights\":{\"addr_bits\":%u,\"data_bits\":%u,\"data_valid\":%s,\"data\":",
               (unsigned)abits, (unsigned)dbits, dvalid ? "true" : "false");
    sb_json_val (sb, (t_value)dv);
    sb_puts (sb, "}");

    st = web_find_reg ("PSL", dptr);
    if (st == NULL)
        st = web_find_reg ("PSW", dptr);
    if (st == NULL)                                     /* accumulator machines (PDP-4/7/9/15) */
        st = web_find_reg ("AC", dptr);
    if (st != NULL) {
        sb_printf (sb, ",\"status\":{\"name\":");
        sb_json_str (sb, st->name);
        sb_printf (sb, ",\"width\":%u,\"value\":", (unsigned)st->width);
        sb_json_val (sb, get_rval (st, 0));
        sb_puts (sb, "}");
        }
    sb_puts (sb, ",\"regs\":{");
    /* R0, R1, ... for as long as they are consecutive (PDP-11 has R0-R5 and
       an alternate set called R10-R15 that must not be mistaken for them),
       then the usual aliases. */
    for (i = 0; i < 16; i++) {
        char nm[8];
        REG *r;

        snprintf (nm, sizeof (nm), "R%u", (unsigned)i);
        r = web_find_reg (nm, dptr);
        if ((r == NULL) || (r->depth > 1))
            break;
        if (!first)
            sb_puts (sb, ",");
        first = FALSE;
        sb_json_str (sb, r->name);
        sb_puts (sb, ":");
        sb_json_val (sb, get_rval (r, 0));
        }
    for (i = 0; i < 3; i++) {
        static const char *alias[] = { "SP", "AP", "FP" };
        REG *r = web_find_reg (alias[i], dptr);

        if ((r == NULL) || (r->depth > 1))
            continue;
        if (!first)
            sb_puts (sb, ",");
        first = FALSE;
        sb_json_str (sb, r->name);
        sb_puts (sb, ":");
        sb_json_val (sb, get_rval (r, 0));
        }
    if (first) {                                        /* no R0..: accumulator-style machine */
        static const char *pref[] = { "AC", "L", "MQ", "SC", "XR", "LR", "BR", "SR", "ASW", "IORS" };
        REG *r;

        for (i = 0; i < sizeof (pref) / sizeof (pref[0]); i++) {
            r = web_find_reg (pref[i], dptr);
            if ((r == NULL) || (r->depth > 1))
                continue;
            if (!first)
                sb_puts (sb, ",");
            first = FALSE;
            sb_json_str (sb, r->name);
            sb_puts (sb, ":");
            sb_json_val (sb, get_rval (r, 0));
            }
        if (first && (dptr->registers != NULL)) {       /* anything else: the first few plain registers */
            uint32 shown = 0;

            for (r = dptr->registers; (r->name != NULL) && (shown < 8); r++) {
                if ((r == pc) || (r->depth > 1) || (r->flags & REG_HIDDEN))
                    continue;
                if (!first)
                    sb_puts (sb, ",");
                first = FALSE;
                shown++;
                sb_json_str (sb, r->name);
                sb_puts (sb, ":");
                sb_json_val (sb, get_rval (r, 0));
                }
            }
        }
    sb_puts (sb, "}");
    }
sb_puts (sb, "}");
}

static void json_reg (SB *sb, REG *r)
{
uint32 idx, depth = (r->depth > 1) ? r->depth : 1;

sb_puts (sb, "{\"name\":");
sb_json_str (sb, r->name);
if (r->desc) {
    sb_puts (sb, ",\"desc\":");
    sb_json_str (sb, r->desc);
    }
sb_printf (sb, ",\"width\":%u,\"radix\":%u,\"readonly\":%s",
           (unsigned)r->width, (unsigned)r->radix, (r->flags & REG_RO) ? "true" : "false");
if (depth > 1) {
    if (depth > WEB_REG_DEPTH_MAX)
        depth = WEB_REG_DEPTH_MAX;
    sb_printf (sb, ",\"depth\":%u,\"values\":[", (unsigned)r->depth);
    for (idx = 0; idx < depth; idx++) {
        if (idx)
            sb_puts (sb, ",");
        sb_json_val (sb, get_rval (r, idx));
        }
    sb_puts (sb, "]");
    }
else {
    sb_puts (sb, ",\"value\":");
    sb_json_val (sb, get_rval (r, 0));
    }
sb_puts (sb, "}");
}

static const char *dev_kind (DEVICE *dptr)
{
switch (DEV_TYPE (dptr)) {
    case DEV_DISK:    return "disk";
    case DEV_TAPE:    return "tape";
    case DEV_MUX:     return "mux";
    case DEV_CARD:    return "card";
    case DEV_ETHER:   return "net";
    case DEV_DISPLAY: return "display";
    default:          return "other";
    }
}

static void json_devices (SB *sb)
{
DEVICE *dptr;
uint32 i, j;

sb_puts (sb, "{\"type\":\"devices\",\"devices\":[");
for (i = 0; (dptr = sim_devices[i]) != NULL; i++) {
    if (i)
        sb_puts (sb, ",");
    sb_puts (sb, "{\"name\":");
    sb_json_str (sb, sim_dname (dptr));
    sb_printf (sb, ",\"kind\":\"%s\",\"bootable\":%s,\"enabled\":%s", dev_kind (dptr),
               (dptr->boot != NULL) ? "true" : "false",
               (dptr->flags & DEV_DIS) ? "false" : "true");
    if (dptr->description) {
        sb_puts (sb, ",\"description\":");
        sb_json_str (sb, dptr->description (dptr));
        }
    sb_puts (sb, ",\"units\":[");
    for (j = 0; j < dptr->numunits; j++) {
        UNIT *uptr = dptr->units + j;
        double bytes = (double)uptr->capac;

        if (j)
            sb_puts (sb, ",");
        sb_puts (sb, "{\"name\":");
        sb_json_str (sb, sim_uname (uptr));
        sb_printf (sb, ",\"attachable\":%s,\"attached\":%s,\"readonly\":%s,\"disabled\":%s",
                   (uptr->flags & UNIT_ATTABLE) ? "true" : "false",
                   (uptr->flags & UNIT_ATT) ? "true" : "false",
                   (uptr->flags & UNIT_RO) ? "true" : "false",
                   (uptr->flags & UNIT_DIS) ? "true" : "false");
        if ((uptr->flags & UNIT_ATT) && uptr->filename) {
            sb_puts (sb, ",\"file\":");
            sb_json_str (sb, uptr->filename);
            }
        if (DEV_TYPE (dptr) == DEV_DISK)                /* capacity in bytes where we can tell */
            bytes *= (dptr->flags & DEV_SECTORS) ? 512.0 : (double)((dptr->dwidth + 7) / 8);
        else
            bytes = 0;
        sb_printf (sb, ",\"bytes\":%.0f}", bytes);
        }
    sb_puts (sb, "]}");
    }
sb_puts (sb, "]}");
}

/* Run a device's SHOW display routine into a temporary file and return the
   text (caller frees).  This reuses the devices' own statistics code
   (SHOW XQ STATS etc.) rather than reaching into their private state. */

static char *web_capture_show_arg (DEVICE *dptr, const char *modname, const char *arg, t_stat *stat)
{
MTAB *mptr;
FILE *f;
char *buf;
size_t n;
t_stat r;

if (stat)
    *stat = SCPE_NOFNC;
if ((dptr == NULL) || (dptr->modifiers == NULL) || (dptr->units == NULL))
    return NULL;
for (mptr = dptr->modifiers; mptr->mask != 0; mptr++)
    if ((mptr->pstring != NULL) && (mptr->disp != NULL) && (strcasecmp (mptr->pstring, modname) == 0))
        break;
if (mptr->mask == 0)
    return NULL;
f = tmpfile ();
if (f == NULL)
    return NULL;
buf = (char *)malloc (65536);
if (buf == NULL) {
    fclose (f);
    return NULL;
    }
r = mptr->disp (f, dptr->units, mptr->match, arg ? (CONST void *)arg : mptr->desc);
if (stat)
    *stat = r;
fflush (f);
rewind (f);
n = fread (buf, 1, 65535, f);
buf[n] = '\0';
fclose (f);
return buf;
}

static char *web_capture_show (DEVICE *dptr, const char *modname)
{
return web_capture_show_arg (dptr, modname, NULL, NULL);
}

/* "Recv:        123" -> counter; anything else is an information line */

static void json_network (SB *sb)
{
DEVICE *dptr;
uint32 i;
t_bool first = TRUE;

sb_puts (sb, "{\"type\":\"network\",\"networks\":[");
for (i = 0; (dptr = sim_devices[i]) != NULL; i++) {
    char *stats, *mac, *type, *line, *next;
    UNIT *uptr = dptr->units;
    t_bool fc = TRUE, fi = TRUE;
    SB info = { NULL, 0, 0, FALSE };

    if ((DEV_TYPE (dptr) != DEV_ETHER) || (dptr->flags & DEV_DIS))
        continue;
    stats = web_capture_show (dptr, "STATS");
    mac = web_capture_show (dptr, "MAC");
    type = web_capture_show (dptr, "TYPE");
    if (!first)
        sb_puts (sb, ",");
    first = FALSE;
    sb_puts (sb, "{\"device\":");
    sb_json_str (sb, sim_dname (dptr));
    if (dptr->description) {
        sb_puts (sb, ",\"description\":");
        sb_json_str (sb, dptr->description (dptr));
        }
    sb_printf (sb, ",\"attached\":%s", (uptr && (uptr->flags & UNIT_ATT)) ? "true" : "false");
    if (uptr && (uptr->flags & UNIT_ATT) && uptr->filename) {
        sb_puts (sb, ",\"adapter\":");
        sb_json_str (sb, uptr->filename);
        }
    if (mac) {                                          /* "MAC=00:11:..." */
        char *e = mac + strlen (mac);

        while ((e > mac) && isspace ((unsigned char)e[-1]))
            *--e = '\0';
        sb_puts (sb, ",\"mac\":");
        sb_json_str (sb, (strncasecmp (mac, "MAC=", 4) == 0) ? mac + 4 : mac);
        }
    if (type) {
        char *e = type + strlen (type);

        while ((e > type) && isspace ((unsigned char)e[-1]))
            *--e = '\0';
        sb_puts (sb, ",\"type\":");
        sb_json_str (sb, (strncasecmp (type, "TYPE=", 5) == 0) ? type + 5 : type);
        }
    sb_puts (sb, ",\"counters\":[");
    for (line = stats; line && *line; line = next) {
        char *colon, *v, *e;

        next = strchr (line, '\n');
        if (next)
            *next++ = '\0';
        while (isspace ((unsigned char)*line))
            line++;
        e = line + strlen (line);
        while ((e > line) && isspace ((unsigned char)e[-1]))
            *--e = '\0';
        if (*line == '\0')
            continue;
        colon = strrchr (line, ':');
        v = colon ? colon + 1 : NULL;
        if (v) {
            char *end;
            unsigned long long val;

            while (isspace ((unsigned char)*v))
                v++;
            val = strtoull (v, &end, 10);
            if ((*v != '\0') && (*end == '\0') && isdigit ((unsigned char)*v)) {
                *colon = '\0';
                if (!fc)
                    sb_puts (sb, ",");
                fc = FALSE;
                sb_puts (sb, "{\"name\":");
                sb_json_str (sb, line);
                sb_printf (sb, ",\"value\":%" LL_FMT "u}", val);
                continue;
                }
            }
        if (!fi)
            sb_puts (&info, ",");
        fi = FALSE;
        sb_json_str (&info, line);
        }
    sb_puts (sb, "],\"info\":[");
    if (info.p)
        sb_puts (sb, info.p);
    sb_puts (sb, "]}");
    free (info.p);
    free (stats);
    free (mac);
    free (type);
    }
sb_puts (sb, "]}");
}

/* Directory listing for the media browser.  Confined to the MEDIA root: the
   sub-path must be relative and contain no ".." component. */

static t_bool media_path_ok (const char *p)
{
const char *s = p;

if ((*p == '/') || (*p == '\\') || (strchr (p, ':') != NULL))
    return FALSE;
for (; *s; s++)
    if (((unsigned char)*s < 0x20) || (*s == '\\'))
        return FALSE;
for (s = p; *s; ) {
    size_t n = strcspn (s, "/");

    if ((n == 2) && (s[0] == '.') && (s[1] == '.'))
        return FALSE;
    s += n;
    if (*s == '/')
        s++;
    }
return TRUE;
}

static int json_media (SB *sb, const char *sub)
{
#if defined (_WIN32)
return 501;
#else
char full[1024];
DIR *d;
struct dirent *de;
t_bool first = TRUE;

if (!media_path_ok (sub))
    return 400;
snprintf (full, sizeof (full), "%s%s%s", web_media, *sub ? "/" : "", sub);
d = opendir (full);
if (d == NULL)
    return 404;
sb_puts (sb, "{\"type\":\"media\",\"root\":");
sb_json_str (sb, web_media);
sb_puts (sb, ",\"path\":");
sb_json_str (sb, sub);
sb_puts (sb, ",\"entries\":[");
while ((de = readdir (d)) != NULL) {
    char ent[1400];
    struct stat sbuf;

    if ((strcmp (de->d_name, ".") == 0) || (strcmp (de->d_name, "..") == 0) || (de->d_name[0] == '.'))
        continue;
    snprintf (ent, sizeof (ent), "%s/%s", full, de->d_name);
    if (stat (ent, &sbuf) != 0)
        continue;
    if (!S_ISDIR (sbuf.st_mode) && !S_ISREG (sbuf.st_mode))
        continue;
    if (!first)
        sb_puts (sb, ",");
    first = FALSE;
    sb_puts (sb, "{\"name\":");
    sb_json_str (sb, de->d_name);
    sb_printf (sb, ",\"dir\":%s,\"size\":%.0f}", S_ISDIR (sbuf.st_mode) ? "true" : "false",
               S_ISDIR (sbuf.st_mode) ? 0.0 : (double)sbuf.st_size);
    }
closedir (d);
sb_puts (sb, "]}");
return 200;
#endif
}

/* ---------------------------------------------------------------------- */
/* HTTP                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    char    method[8];
    char    *path;
    char    *query;
    char    *host;
    char    *origin;
    char    *auth;
    char    *upgrade;
    char    *wskey;
    char    *body;
    size_t  bodylen;
    } WEB_REQ;

static const char *status_text (int code)
{
switch (code) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    default:  return "Internal Server Error";
    }
}

static void respond (WEB_CLIENT *c, int code, const char *ctype, const char *body, size_t n)
{
char hdr[512];
int hl;

hl = snprintf (hdr, sizeof (hdr),
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
    "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
    "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; "
    "style-src 'unsafe-inline'; connect-src 'self' ws: wss:; frame-ancestors 'none'\r\n"
    "Connection: close\r\n\r\n",
    code, status_text (code), ctype, (unsigned long)n);
client_send (c, hdr, (size_t)hl);
client_send (c, body, n);
c->close_after = TRUE;
}

static void respond_json (WEB_CLIENT *c, int code, SB *sb)
{
respond (c, code, "application/json", sb->p ? sb->p : "{}", sb->p ? sb->len : 2);
}

static void respond_error (WEB_CLIENT *c, int code, const char *msg)
{
SB sb = { NULL, 0, 0, FALSE };

sb_puts (&sb, "{\"error\":");
sb_json_str (&sb, msg);
sb_puts (&sb, "}");
respond_json (c, code, &sb);
free (sb.p);
}

static void respond_stat (WEB_CLIENT *c, t_stat r)
{
respond_error (c, 400, sim_error_text (r));
}

/* Constant-time comparison so response timing does not leak the token */

static t_bool token_ok (const char *given)
{
size_t i, n = strlen (web_token);
unsigned diff = (unsigned)(strlen (given) ^ n);

for (i = 0; i < n; i++)
    diff |= (unsigned char)web_token[i] ^ (unsigned char)(given[i] ? given[i] : 0);
return diff == 0;
}

/* Origin and Host validation.  A browser page on another site (or a DNS
   rebinding attack against localhost) must not be able to drive the
   simulator, so cross-origin requests are refused outright. */

static t_bool host_is_loopback (const char *host)
{
size_t n = 0;

if (host[0] == '[') {
    const char *e = strchr (host, ']');
    return (e != NULL) && ((size_t)(e - host) == 4) && (strncmp (host, "[::1]", 5) == 0);
    }
while (host[n] && (host[n] != ':'))
    n++;
if ((n == 9) && (strncasecmp (host, "localhost", 9) == 0))
    return TRUE;
return (n >= 4) && (strncmp (host, "127.", 4) == 0);
}

static t_bool request_allowed (WEB_CLIENT *c, WEB_REQ *rq)
{
char tok[128];

if (rq->host == NULL) {
    respond_error (c, 400, "Host header required");
    return FALSE;
    }
if (web_loopback && !host_is_loopback (rq->host)) {
    respond_error (c, 403, "Host not permitted");
    return FALSE;
    }
if (rq->origin != NULL) {
    const char *o = strstr (rq->origin, "://");

    o = o ? o + 3 : rq->origin;
    if (strcasecmp (o, rq->host) != 0) {
        respond_error (c, 403, "Cross-origin request refused");
        return FALSE;
        }
    }
if (web_token[0] != '\0') {
    tok[0] = '\0';
    if (rq->auth && (strncasecmp (rq->auth, "Bearer ", 7) == 0))
        strlcpy (tok, rq->auth + 7, sizeof (tok));
    else if (rq->query)
        query_get (rq->query, "token", tok, sizeof (tok));
    if (!token_ok (tok)) {
        respond_error (c, 401, "Token required");
        return FALSE;
        }
    }
return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Commands                                                                */
/* ---------------------------------------------------------------------- */

/* Commands that need the simulator stopped are not run here: the line is
   handed to the sim> command loop (sim_web_prompt_wait) so that SCP executes
   it exactly as if typed at the console.  The HTTP reply is deferred until
   the command finishes, or the simulation starts running. */

static t_bool web_may_modify (void)
{
return !sim_is_running || web_paused;
}

static int web_inject (WEB_CLIENT *c, const char *line, const char **msg)
{
size_t i;

while (*line == ' ' || *line == '\t')
    line++;
if (sim_is_running) {
    *msg = "Simulator is running; halt it first";
    return 409;
    }
if (web_cmd_pending || (web_reply_slot >= 0)) {
    *msg = "Another command is in progress";
    return 409;
    }
if (*line == '\0') {
    *msg = "Empty command";
    return 400;
    }
if (strlen (line) >= sizeof (web_cmd)) {
    *msg = "Command too long";
    return 400;
    }
for (i = 0; line[i]; i++)
    if (((unsigned char)line[i] < 0x20) || (line[i] == 0x7f)) {
        *msg = "Control characters are not allowed";
        return 400;
        }
if ((*line == '!') || (strncasecmp (line, "SPAWN", 5) == 0)) {
    *msg = "Host shell commands are not allowed";
    return 403;
    }
strlcpy (web_cmd, line, sizeof (web_cmd));
sim_web_note_command (web_cmd);
web_cmd_pending = TRUE;
if (c) {
    web_reply_slot = (int)(c - web_clients);
    web_reply_serial = c->serial;
    return 202;                                         /* reply deferred */
    }
return 200;
}

/* Run-state commands.  "body" is the JSON request body or WebSocket message,
   from which the optional argument is taken.  Returns an HTTP status. */

static int web_command (WEB_CLIENT *c, const char *cmd, const char *body, const char **msg)
{
char arg[256] = "", line[320];

*msg = NULL;
if (strcmp (cmd, "pause") == 0) {
    if (!sim_is_running) {
        *msg = "Simulator is not running";
        return 409;
        }
    web_paused = TRUE;
    web_step_pending = FALSE;
    }
else if ((strcmp (cmd, "resume") == 0) || (strcmp (cmd, "continue") == 0)) {
    if (!sim_is_running)
        return web_inject (c, "CONTINUE", msg);
    web_paused = FALSE;
    web_step_pending = FALSE;
    }
else if (strcmp (cmd, "step") == 0) {
    if (!sim_is_running) {
        json_get (body, "count", arg, sizeof (arg));
        snprintf (line, sizeof (line), "STEP %s", arg);
        return web_inject (c, line, msg);
        }
    if (!web_paused) {
        *msg = "Simulator must be paused or stopped to step";
        return 409;
        }
    web_paused = FALSE;                                 /* leave the pause loop... */
    web_step_pending = TRUE;                            /* ...and pause again after one instruction */
    }
else if (strcmp (cmd, "halt") == 0) {
    if (sim_is_running)
        web_halt_req = TRUE;
    }
else if ((strcmp (cmd, "go") == 0) || (strcmp (cmd, "run") == 0)) {
    json_get (body, "addr", arg, sizeof (arg));
    snprintf (line, sizeof (line), "%s %s", cmd, arg);
    return web_inject (c, line, msg);
    }
else if (strcmp (cmd, "boot") == 0) {
    if (!json_get (body, "device", arg, sizeof (arg))) {
        *msg = "Body needs \"device\"";
        return 400;
        }
    snprintf (line, sizeof (line), "BOOT %s", arg);
    return web_inject (c, line, msg);
    }
else if (strcmp (cmd, "bootdev") == 0) {                /* choose the unit Restart will boot */
    DEVICE *bd;
    UNIT *bu = NULL;
    size_t k;

    if (!json_get (body, "device", arg, sizeof (arg)) ||
        (strspn (arg, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen (arg))) {
        *msg = "Body needs a valid \"device\"";
        return 400;
        }
    for (k = 0; arg[k]; k++)
        arg[k] = (char)toupper ((unsigned char)arg[k]);
    bd = find_unit (arg, &bu);
    if ((bd == NULL) || (bu == NULL)) {
        *msg = "No such unit";
        return 404;
        }
    if (bd->boot == NULL) {
        *msg = "That device cannot be booted";
        return 400;
        }
    if (!(bu->flags & UNIT_ATT)) {
        *msg = "That unit has no media loaded";
        return 400;
        }
    strlcpy (web_last_boot, arg, sizeof (web_last_boot));
    }
else if (strcmp (cmd, "reset") == 0)
    return web_inject (c, "RESET", msg);
else if (strcmp (cmd, "restart") == 0) {                /* halt if need be, then BOOT */
    const char *dev = web_boot_default ();

    if (*dev == '\0') {
        *msg = "No boot device known: attach a disk or use Boot";
        return 409;
        }
    snprintf (line, sizeof (line), "BOOT %s", dev);
    if (!sim_is_running)
        return web_inject (c, line, msg);
    if (web_cmd_pending || web_restart_pending || (web_reply_slot >= 0)) {
        *msg = "Another command is in progress";
        return 409;
        }
    web_restart_pending = TRUE;                         /* the sim> loop boots once we have stopped */
    web_halt_req = TRUE;
    if (c) {
        web_reply_slot = (int)(c - web_clients);
        web_reply_serial = c->serial;
        return 202;
        }
    return 200;
    }
else if (strcmp (cmd, "command") == 0) {
    if (!json_get (body, "command", arg, sizeof (arg))) {
        *msg = "Body needs \"command\"";
        return 400;
        }
    return web_inject (c, arg, msg);
    }
else {
    *msg = "Unknown command";
    return 404;
    }
return 200;
}

/* Memory examine/deposit shared by REST and WebSocket.  RAM only: examining
   the I/O page could have side effects on devices.  Returns an HTTP status;
   on 200 the JSON is in sb, otherwise *msg says why. */

static int web_memory (SB *sb, const char *addr_s, const char *count_s, const char *val_s,
                       t_bool write, const char **msg)
{
DEVICE *dptr = sim_dflt_dev;
UNIT *uptr = (dptr != NULL) ? dptr->units : NULL;
t_value addr, count = 1, val = 0;
uint32 i;

if ((dptr == NULL) || (dptr->examine == NULL) || (dptr->deposit == NULL) || (uptr == NULL)) {
    *msg = "Simulator has no addressable memory";
    return 404;
    }
if (!parse_num (addr_s, dptr->aradix, &addr)) {
    *msg = "addr required";
    return 400;
    }
if (count_s && *count_s && !parse_num (count_s, 10, &count)) {
    *msg = "Bad count";
    return 400;
    }
if (write) {
    if (!val_s || !parse_num (val_s, dptr->dradix, &val)) {
        *msg = "Numeric value required";
        return 400;
        }
    if (!web_may_modify ()) {
        *msg = "Simulator must be stopped or paused to modify memory";
        return 409;
        }
    count = 1;
    }
if ((count < 1) || (count > WEB_MEM_MAX)) {
    *msg = "count must be 1..512";
    return 400;
    }
if (addr + count * dptr->aincr > (t_value)uptr->capac) {
    *msg = "Address outside physical memory";
    return 400;
    }
if (write) {
    t_stat r = dptr->deposit (val, (t_addr)addr, uptr, 0);

    if (r != SCPE_OK) {
        *msg = sim_error_text (r);
        return 400;
        }
    }
sb_puts (sb, "{\"type\":\"memory\",\"addr\":");
sb_json_val (sb, addr);
sb_printf (sb, ",\"aincr\":%u,\"dwidth\":%u,\"aradix\":%u,\"dradix\":%u,\"values\":[",
           (unsigned)dptr->aincr, (unsigned)dptr->dwidth, (unsigned)dptr->aradix, (unsigned)dptr->dradix);
for (i = 0; i < (uint32)count; i++) {
    t_stat r = dptr->examine (&val, (t_addr)(addr + i * dptr->aincr), uptr, 0);

    if (i)
        sb_puts (sb, ",");
    if (r == SCPE_OK)
        sb_json_val (sb, val);
    else
        sb_puts (sb, "null");
    }
sb_puts (sb, "]}");
return 200;
}

/* ATTACH / DETACH shared by REST and WebSocket.  "body" is JSON with "unit",
   and for attach "file" and optional "readonly"/"new".  Runs SCP's own
   attach/detach commands directly.  We are on the simulator's main thread
   (between instructions when running), so media can be changed in any state,
   like swapping a pack on a live drive: the guest sees the unit go offline or
   online. */

static int web_media_op (SB *sb, t_bool attach, const char *body, const char **msg)
{
char unit[64], file[512], line[700], flag[8], opt[16];
DEVICE *dptr;
UNIT *uptr = NULL;
t_stat r;
size_t i;

if (!json_get (body, "unit", unit, sizeof (unit)) ||
    (strspn (unit, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen (unit))) {
    *msg = "Body needs a valid \"unit\"";
    return 400;
    }
for (i = 0; unit[i]; i++)
    unit[i] = (char)toupper ((unsigned char)unit[i]);
dptr = find_unit (unit, &uptr);
if ((dptr == NULL) || (uptr == NULL)) {
    *msg = "No such unit";
    return 404;
    }
sim_switches = 0;
if (attach) {
    size_t fl = 0;

    if (!json_get (body, "file", file, sizeof (file))) {
        *msg = "Body needs \"file\"";
        return 400;
        }
    for (i = 0; file[i]; i++)
        if (((unsigned char)file[i] < 0x20) || (file[i] == 0x7f)) {
            *msg = "Control characters are not allowed";
            return 400;
            }
    flag[0] = '\0';
    if (json_get (body, "readonly", opt, sizeof (opt)) && (strcmp (opt, "true") == 0))
        fl += (size_t)snprintf (flag + fl, sizeof (flag) - fl, "-R ");
    if (json_get (body, "new", opt, sizeof (opt)) && (strcmp (opt, "true") == 0))
        fl += (size_t)snprintf (flag + fl, sizeof (flag) - fl, "-N ");
    snprintf (line, sizeof (line), "%s%s %s", flag, unit, file);
    r = attach_cmd (0, line);
    }
else
    r = detach_cmd (0, unit);
r = SCPE_BARE_STATUS (r);
sb_printf (sb, "{\"type\":\"result\",\"op\":\"%s\",\"unit\":", attach ? "attach" : "detach");
sb_json_str (sb, unit);
sb_printf (sb, ",\"ok\":%s,\"status\":", (r == SCPE_OK) ? "true" : "false");
sb_json_str (sb, (r == SCPE_OK) ? "OK" : sim_error_text (r));
sb_puts (sb, "}");
return 200;
}

static void route_api (WEB_CLIENT *c, WEB_REQ *rq)
{
SB sb = { NULL, 0, 0, FALSE };
const char *path = rq->path;
t_bool is_get = (strcmp (rq->method, "GET") == 0);
t_bool is_write = (strcmp (rq->method, "POST") == 0) || (strcmp (rq->method, "PUT") == 0);
char arg[128], arg2[128];

if (strcmp (path, "/api/state") == 0) {
    if (!is_get) goto bad_method;
    json_state (&sb);
    respond_json (c, 200, &sb);
    }
else if (strcmp (path, "/api/devices") == 0) {
    if (!is_get) goto bad_method;
    json_devices (&sb);
    respond_json (c, 200, &sb);
    }
else if (strcmp (path, "/api/registers") == 0) {
    DEVICE *dptr = sim_dflt_dev;
    REG *r;
    t_bool first = TRUE;

    if (!is_get) goto bad_method;
    if (query_get (rq->query, "device", arg, sizeof (arg)))
        dptr = find_dev (arg);
    if ((dptr == NULL) || (dptr->registers == NULL)) {
        respond_error (c, 404, "No such device or device has no registers");
        goto done;
        }
    sb_puts (&sb, "{\"device\":");
    sb_json_str (&sb, sim_dname (dptr));
    sb_puts (&sb, ",\"registers\":[");
    for (r = dptr->registers; r->name != NULL; r++) {
        if (r->flags & REG_HIDDEN)
            continue;
        if (!first)
            sb_puts (&sb, ",");
        first = FALSE;
        json_reg (&sb, r);
        }
    sb_puts (&sb, "]}");
    respond_json (c, 200, &sb);
    }
else if (strncmp (path, "/api/registers/", 15) == 0) {
    DEVICE *dptr = sim_dflt_dev;
    REG *r;
    t_value val, idx = 0;

    if (!is_write) goto bad_method;
    if (query_get (rq->query, "device", arg, sizeof (arg)))
        dptr = find_dev (arg);
    r = web_find_reg (path + 15, dptr);
    if (r == NULL) {
        respond_error (c, 404, "No such register");
        goto done;
        }
    if (!web_may_modify ()) {
        respond_error (c, 409, "Simulator must be stopped or paused to modify registers");
        goto done;
        }
    if (r->flags & REG_RO) {
        respond_error (c, 403, "Register is read only");
        goto done;
        }
    if (!json_get (rq->body, "value", arg, sizeof (arg)) || !parse_num (arg, r->radix, &val)) {
        respond_error (c, 400, "Body needs a numeric \"value\"");
        goto done;
        }
    if (json_get (rq->body, "index", arg2, sizeof (arg2)) && !parse_num (arg2, 10, &idx)) {
        respond_error (c, 400, "Bad \"index\"");
        goto done;
        }
    if ((r->depth > 1 ? idx >= r->depth : idx != 0)) {
        respond_error (c, 400, "Index out of range");
        goto done;
        }
    put_rval (r, (uint32)idx, val);
    json_reg (&sb, r);
    respond_json (c, 200, &sb);
    }
else if (strcmp (path, "/api/memory") == 0) {
    char a[64] = "", n[32] = "", v[64] = "";
    const char *msg;
    t_bool have_v = FALSE;
    int code;

    if (is_get)
        query_get (rq->query, "addr", a, sizeof (a)), query_get (rq->query, "count", n, sizeof (n));
    else if (is_write) {
        json_get (rq->body, "addr", a, sizeof (a));
        json_get (rq->body, "count", n, sizeof (n));
        have_v = json_get (rq->body, "value", v, sizeof (v));
        if (!have_v) {
            respond_error (c, 400, "Body needs numeric \"addr\" and \"value\"");
            goto done;
            }
        }
    else
        goto bad_method;
    code = web_memory (&sb, a, n, v, is_write, &msg);
    if (code != 200)
        respond_error (c, code, msg);
    else
        respond_json (c, 200, &sb);
    }
else if (strcmp (path, "/api/network") == 0) {
    if (!is_get) goto bad_method;
    json_network (&sb);
    respond_json (c, 200, &sb);
    }
else if (strcmp (path, "/api/media") == 0) {
    int code;

    if (!is_get) goto bad_method;
    arg[0] = '\0';
    query_get (rq->query, "path", arg, sizeof (arg));
    code = json_media (&sb, arg);
    if (code != 200)
        respond_error (c, code, (code == 400) ? "Bad path" : (code == 404) ? "No such directory" : "Not supported");
    else
        respond_json (c, 200, &sb);
    }
else if ((strcmp (path, "/api/attach") == 0) || (strcmp (path, "/api/detach") == 0)) {
    const char *msg;
    int code;

    if (!is_write) goto bad_method;
    code = web_media_op (&sb, path[5] == 'a', rq->body, &msg);
    if (code != 200)
        respond_error (c, code, msg);
    else
        respond_json (c, 200, &sb);
    }
else if (strncmp (path, "/api/", 5) == 0) {                /* /api/pause etc. */
    const char *msg;
    int code;

    if (!is_write) goto bad_method;
    code = web_command (c, path + 5, rq->body, &msg);
    if (code == 202)                                    /* reply sent when the command completes */
        goto done;
    if (code != 200)
        respond_error (c, code, msg);
    else {
        json_state (&sb);
        respond_json (c, 200, &sb);
        }
    }
else
    respond_error (c, 404, "Not found");
goto done;

bad_method:
respond_error (c, 405, "Method not allowed");
done:
free (sb.p);
}

static void ws_handshake (WEB_CLIENT *c, WEB_REQ *rq)
{
static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
SHA1_CTX ctx;
unsigned char digest[20];
char accept[32], resp[256];
int n;

if ((rq->wskey == NULL) || (strlen (rq->wskey) > 64)) {
    respond_error (c, 400, "Bad WebSocket handshake");
    return;
    }
sha1_init (&ctx);
sha1_update (&ctx, (const unsigned char *)rq->wskey, strlen (rq->wskey));
sha1_update (&ctx, (const unsigned char *)guid, strlen (guid));
sha1_final (&ctx, digest);
base64_encode (digest, sizeof (digest), accept);
n = snprintf (resp, sizeof (resp),
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
    "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
client_send (c, resp, (size_t)n);
c->ws = TRUE;
c->last_push = 0;                                       /* push state immediately */
}

static void handle_request (WEB_CLIENT *c, WEB_REQ *rq)
{
if (!request_allowed (c, rq))
    return;
if (strcmp (rq->path, "/ws") == 0) {
    if (rq->upgrade && (strcasecmp (rq->upgrade, "websocket") == 0))
        ws_handshake (c, rq);
    else
        respond_error (c, 400, "WebSocket upgrade required");
    }
else if (strncmp (rq->path, "/api/", 5) == 0)
    route_api (c, rq);
else if ((strcmp (rq->path, "/") == 0) || (strcmp (rq->path, "/index.html") == 0)) {
    if (strcmp (rq->method, "GET") != 0)
        respond_error (c, 405, "Method not allowed");
    else
        respond (c, 200, "text/html; charset=utf-8", (const char *)web_page_html, web_page_html_len);
    }
else
    respond_error (c, 404, "Not found");
}

/* Parse the request buffered in c->rx.  The caller (client_read) has already
   established that the headers and the whole body are present. */

static t_bool http_parse (WEB_CLIENT *c)
{
WEB_REQ rq;
char *hdr_end, *line, *next, *sp1, *sp2;
size_t clen = 0;

c->rx[c->rxlen] = '\0';
hdr_end = strstr (c->rx, "\r\n\r\n");
if (hdr_end == NULL)
    return FALSE;
memset (&rq, 0, sizeof (rq));
*hdr_end = '\0';
next = strstr (c->rx, "\r\n");
if (next) {
    *next = '\0';
    next += 2;
    }
sp1 = strchr (c->rx, ' ');
sp2 = sp1 ? strchr (sp1 + 1, ' ') : NULL;
if ((sp1 == NULL) || (sp2 == NULL) || ((size_t)(sp1 - c->rx) >= sizeof (rq.method))) {
    respond_error (c, 400, "Bad request");
    return TRUE;
    }
*sp1 = '\0';
*sp2 = '\0';
strlcpy (rq.method, c->rx, sizeof (rq.method));
rq.path = sp1 + 1;
rq.query = strchr (rq.path, '?');
if (rq.query)
    *rq.query++ = '\0';
for (line = next; line && *line; line = next) {
    char *colon, *val;

    next = strstr (line, "\r\n");
    if (next) {
        *next = '\0';
        next += 2;
        }
    colon = strchr (line, ':');
    if (colon == NULL)
        continue;
    *colon = '\0';
    val = colon + 1;
    while (*val == ' ' || *val == '\t')
        val++;
    if (strcasecmp (line, "Host") == 0)                     rq.host = val;
    else if (strcasecmp (line, "Origin") == 0)              rq.origin = val;
    else if (strcasecmp (line, "Authorization") == 0)       rq.auth = val;
    else if (strcasecmp (line, "Upgrade") == 0)             rq.upgrade = val;
    else if (strcasecmp (line, "Sec-WebSocket-Key") == 0)   rq.wskey = val;
    else if (strcasecmp (line, "Content-Length") == 0)      clen = (size_t)strtoul (val, NULL, 10);
    }
rq.body = hdr_end + 4;
if (clen > WEB_RX_MAX - 1024) {
    respond_error (c, 413, "Request too large");
    return TRUE;
    }
rq.bodylen = clen;
rq.body[clen] = '\0';
handle_request (c, &rq);
c->rxlen = 0;
return TRUE;
}

/* ---------------------------------------------------------------------- */
/* WebSocket frames from the client                                        */
/* ---------------------------------------------------------------------- */

static void ws_error (WEB_CLIENT *c, const char *err)
{
SB sb = { NULL, 0, 0, FALSE };

sb_puts (&sb, "{\"type\":\"error\",\"error\":");
sb_json_str (&sb, err);
sb_puts (&sb, "}");
if (sb.p)
    ws_send_text (c, sb.p, sb.len);
free (sb.p);
}

/* WebSocket requests.  Everything the panel needs goes through here, so the
   page is driven purely by events: state / devices / network are pushed by
   the server; media, memory, attach, detach and command results come back
   as events answering a request. */

static void ws_handle_message (WEB_CLIENT *c, const char *msg)
{
char cmd[32], a[512], n[32], v[64];
SB sb = { NULL, 0, 0, FALSE };
const char *err = NULL;
int code = 200;

if (!json_get (msg, "cmd", cmd, sizeof (cmd)))
    return;
if (strcmp (cmd, "media") == 0) {
    a[0] = '\0';
    json_get (msg, "path", a, sizeof (a));
    code = json_media (&sb, a);
    err = (code == 400) ? "Bad path" : (code == 404) ? "No such directory" : "Not supported";
    }
else if ((strcmp (cmd, "memory") == 0) || (strcmp (cmd, "deposit") == 0)) {
    t_bool write = (cmd[0] == 'd');

    a[0] = n[0] = v[0] = '\0';
    json_get (msg, "addr", a, sizeof (a));
    json_get (msg, "count", n, sizeof (n));
    if (write && !json_get (msg, "value", v, sizeof (v)))
        code = 400, err = "Numeric value required";
    else
        code = web_memory (&sb, a, n, v, write, &err);
    }
else if ((strcmp (cmd, "attach") == 0) || (strcmp (cmd, "detach") == 0))
    code = web_media_op (&sb, cmd[0] == 'a', msg, &err);
else if (strcmp (cmd, "input") == 0) {                      /* keystrokes for the guest console */
    unsigned char data[512];
    size_t n = json_get_bytes (msg, "data", data, sizeof (data));

    if (!sim_is_running || web_paused) {
        code = 409;
        err = "Console input needs the simulator running";
        }
    else
        web_con_queue (data, n);
    if (code == 200)
        code = 0;
    }
else if (strcmp (cmd, "history") == 0) {                    /* subscribe / enable / disable instruction history */
    char v[16];
    t_value n = 0;

    if (json_get (msg, "count", v, sizeof (v)) && parse_num (v, 10, &n))
        c->hist_n = (n > 100) ? 100 : (uint32)n;
    if (json_get (msg, "enable", v, sizeof (v))) {
        t_stat r = web_history_set ((strcmp (v, "true") == 0) ? WEB_HIST_SIZE : "0");

        if (r != SCPE_OK) {
            code = 400;
            err = sim_error_text (SCPE_BARE_STATUS (r));
            }
        }
    c->hist_ver = 0;
    web_hist_ver++;                                         /* force a fresh snapshot for everyone */
    web_last_hist_time = 0;
    }
else if (strcmp (cmd, "devices") == 0) {
    c->dev_ver = 0;
    c->io_ver = 0;
    }                                         /* resend devices and network */
else {
    code = web_command (c, cmd, msg, &err);
    if (code == 202)                                        /* result arrives when the command ends */
        code = 0;
    else if (code == 200) {
        c->last_push = 0;                                   /* show the new state promptly */
        code = 0;
        }
    }
if (code == 200 && sb.p)
    ws_send_text (c, sb.p, sb.len);
else if (code > 200)
    ws_error (c, err ? err : "Request failed");
free (sb.p);
}

static void ws_parse (WEB_CLIENT *c)
{
while (c->sock != INVALID_SOCKET && c->rxlen >= 2) {
    unsigned char *f = (unsigned char *)c->rx;
    int opcode = f[0] & 0x0F;
    t_bool fin = (f[0] & 0x80) != 0;
    t_bool masked = (f[1] & 0x80) != 0;
    t_uint64 plen = f[1] & 0x7F;
    size_t hl = 2, i;
    unsigned char mask[4] = { 0, 0, 0, 0 };

    if (plen == 126) {
        if (c->rxlen < 4)
            return;
        plen = ((t_uint64)f[2] << 8) | f[3];
        hl = 4;
        }
    else if (plen == 127) {
        if (c->rxlen < 10)
            return;
        plen = 0;
        for (i = 0; i < 8; i++)
            plen = (plen << 8) | f[2 + i];
        hl = 10;
        }
    if (!masked || (plen > WEB_WS_MAX) || !fin) {           /* protocol error / unsupported */
        client_close (c);
        return;
        }
    if (c->rxlen < hl + 4 + plen)
        return;
    memcpy (mask, f + hl, 4);
    hl += 4;
    for (i = 0; i < plen; i++)
        f[hl + i] ^= mask[i & 3];
    if (opcode == 0x1) {                                    /* text */
        char saved = (char)f[hl + plen];

        f[hl + plen] = '\0';
        ws_handle_message (c, (const char *)f + hl);
        f[hl + plen] = (unsigned char)saved;
        }
    else if (opcode == 0x9) {                               /* ping -> pong */
        unsigned char pong[2] = { 0x8A, (unsigned char)(plen & 0x7F) };

        if (plen < 126) {
            client_send (c, (const char *)pong, 2);
            client_send (c, (const char *)f + hl, (size_t)plen);
            }
        }
    else if (opcode == 0x8) {                               /* close */
        client_close (c);
        return;
        }
    if (c->sock == INVALID_SOCKET)
        return;
    memmove (c->rx, c->rx + hl + plen, c->rxlen - (hl + (size_t)plen));
    c->rxlen -= hl + (size_t)plen;
    }
}

static void client_read (WEB_CLIENT *c)
{
int n;

if (c->rxlen + 1024 > c->rxcap) {
    size_t ncap = c->rxcap ? c->rxcap * 2 : 4096;
    char *nr;

    if (ncap > WEB_RX_MAX + 16)
        ncap = WEB_RX_MAX + 16;
    if (c->rxlen + 1024 > ncap) {                           /* buffer full without a request */
        client_close (c);
        return;
        }
    nr = (char *)realloc (c->rx, ncap);
    if (nr == NULL) {
        client_close (c);
        return;
        }
    c->rx = nr;
    c->rxcap = ncap;
    }
n = sim_read_sock (c->sock, c->rx + c->rxlen, (int)(c->rxcap - c->rxlen - 1));
if (n < 0) {
    client_close (c);
    return;
    }
if (n == 0)
    return;
c->rxlen += (size_t)n;
if (c->close_after)                                         /* response already queued */
    c->rxlen = 0;
else if (c->ws)
    ws_parse (c);
else {
    /* Defer parsing until the complete request (headers + body) is in */
    char *he;
    size_t clen = 0;

    c->rx[c->rxlen] = '\0';
    he = strstr (c->rx, "\r\n\r\n");
    if (he != NULL) {
        char *cl = c->rx;

        while ((cl = strstr (cl, "\r\n")) != NULL && cl < he) {
            cl += 2;
            if (strncasecmp (cl, "Content-Length:", 15) == 0) {
                clen = (size_t)strtoul (cl + 15, NULL, 10);
                break;
                }
            }
        if ((size_t)(he + 4 - c->rx) + clen > c->rxlen)
            return;                                         /* wait for the rest of the body */
        }
    http_parse (c);
    }
}

/* ---------------------------------------------------------------------- */
/* Service                                                                 */
/* ---------------------------------------------------------------------- */

static void web_broadcast_state (t_bool force)
{
uint32 now = sim_os_msec ();
uint32 i;
SB sb = { NULL, 0, 0, FALSE };

web_broadcast_events ();

if (!force && ((now - web_last_push) < WEB_PUSH_MSEC))
    return;
for (i = 0; i < WEB_MAX_CLIENTS; i++) {
    WEB_CLIENT *c = &web_clients[i];

    if ((c->sock == INVALID_SOCKET) || !c->ws || c->close_after)
        continue;
    if (!force && c->last_push != 0 && (now - c->last_push) < WEB_PUSH_MSEC)
        continue;
    if (sb.p == NULL) {
        json_state (&sb);
        if (sb.err || sb.p == NULL)
            break;
        }
    if (c->txlen - c->txoff < 64 * 1024) {                  /* skip slow readers rather than queue */
        ws_send_text (c, sb.p, sb.len);
        c->last_push = now ? now : 1;
        }
    }
web_last_push = now;
free (sb.p);
}

/* ---------------------------------------------------------------------- */
/* Guest console <-> web terminal                                          */
/* ---------------------------------------------------------------------- */

#define WEB_CON_RING    32768                           /* replayed to new clients */
#define WEB_CON_PEND    16384
#define WEB_CON_IN      1024

static unsigned char web_con_ring[WEB_CON_RING];
static size_t web_con_total = 0;                        /* bytes ever written to the ring */
static unsigned char web_con_pend[WEB_CON_PEND];
static size_t web_con_pend_len = 0;
static unsigned char web_con_in[WEB_CON_IN];
static size_t web_con_in_head = 0, web_con_in_tail = 0;

void sim_web_console_out (int c)
{
if (web_master == INVALID_SOCKET)
    return;
web_con_ring[web_con_total++ % WEB_CON_RING] = (unsigned char)c;
if (web_con_pend_len < sizeof (web_con_pend))
    web_con_pend[web_con_pend_len++] = (unsigned char)c;
}

int sim_web_console_in (void)
{
int c;

if ((web_master == INVALID_SOCKET) || (web_con_in_head == web_con_in_tail))
    return -1;
c = web_con_in[web_con_in_tail];
web_con_in_tail = (web_con_in_tail + 1) % WEB_CON_IN;
return c;
}

static size_t web_con_queue (const unsigned char *d, size_t n)
{
size_t i, queued = 0;

for (i = 0; i < n; i++) {
    size_t next = (web_con_in_head + 1) % WEB_CON_IN;

    if (next == web_con_in_tail)                        /* full: drop the rest */
        break;
    web_con_in[web_con_in_head] = d[i];
    web_con_in_head = next;
    queued++;
    }
return queued;
}

static void web_con_json (SB *sb, const unsigned char *d, size_t n, t_bool replay)
{
size_t i;

sb_printf (sb, "{\"type\":\"console\",\"replay\":%s,\"data\":\"", replay ? "true" : "false");
for (i = 0; i < n; i++) {
    unsigned char ch = d[i];

    if ((ch == '"') || (ch == '\\'))
        sb_printf (sb, "\\%c", ch);
    else if ((ch < 0x20) || (ch >= 0x7f))
        sb_printf (sb, "\\u%04x", ch);
    else
        sb_put (sb, (const char *)&ch, 1);
    }
sb_puts (sb, "\"}");
}

/* Fresh clients get the recent history; established ones get what is new. */

static void web_broadcast_console (void)
{
uint32 i;
SB live = { NULL, 0, 0, FALSE };
SB hist = { NULL, 0, 0, FALSE };

if ((web_con_pend_len == 0) && (web_con_total == 0))
    return;
for (i = 0; i < WEB_MAX_CLIENTS; i++) {
    WEB_CLIENT *c = &web_clients[i];

    if ((c->sock == INVALID_SOCKET) || !c->ws || c->close_after || (c->txlen - c->txoff >= 64 * 1024))
        continue;
    if (c->con_init) {
        if (web_con_pend_len == 0)
            continue;
        if (live.p == NULL)
            web_con_json (&live, web_con_pend, web_con_pend_len, FALSE);
        if (live.p)
            ws_send_text (c, live.p, live.len);
        }
    else {
        if (hist.p == NULL) {
            size_t n = (web_con_total < WEB_CON_RING) ? web_con_total : WEB_CON_RING;
            size_t start = (web_con_total < WEB_CON_RING) ? 0 : (web_con_total % WEB_CON_RING);
            unsigned char *lin = (unsigned char *)malloc (n ? n : 1);
            size_t k;

            if (lin == NULL)
                continue;
            for (k = 0; k < n; k++)
                lin[k] = web_con_ring[(start + k) % WEB_CON_RING];
            web_con_json (&hist, lin, n, TRUE);
            free (lin);
            }
        if (hist.p)
            ws_send_text (c, hist.p, hist.len);
        c->con_init = TRUE;
        }
    }
web_con_pend_len = 0;
free (live.p);
free (hist.p);
}

/* Instruction history: the CPU's own SET/SHOW CPU HISTORY, pushed as "history"
   events to clients that subscribed.  Recording slows the simulator a little,
   so the page switches it on and off explicitly. */

static t_bool web_history_enabled (void)
{
t_stat r;
char *t = web_capture_show_arg (sim_dflt_dev, "HISTORY", "1", &r);

free (t);
return r == SCPE_OK;
}

static t_stat web_history_set (const char *arg)
{
MTAB *mptr;
DEVICE *dptr = sim_dflt_dev;

if ((dptr == NULL) || (dptr->modifiers == NULL))
    return SCPE_NOFNC;
for (mptr = dptr->modifiers; mptr->mask != 0; mptr++)
    if ((mptr->pstring != NULL) && (mptr->valid != NULL) && (strcasecmp (mptr->pstring, "HISTORY") == 0))
        return mptr->valid (dptr->units, mptr->match, arg, mptr->desc);
return SCPE_NOFNC;
}

static void web_history_json (SB *sb, uint32 count)
{
char arg[16];
t_stat r;
char *text, *line, *next;
t_bool first = TRUE;

snprintf (arg, sizeof (arg), "%u", (unsigned)count);
text = web_capture_show_arg (sim_dflt_dev, "HISTORY", arg, &r);
sb_printf (sb, "{\"type\":\"history\",\"enabled\":%s,\"count\":%u,\"header\":", (r == SCPE_OK) ? "true" : "false", (unsigned)count);
if (r != SCPE_OK) {
    sb_puts (sb, "\"\",\"lines\":[]}");
    free (text);
    return;
    }
line = text;
next = strchr (line, '\n');
if (next)
    *next++ = '\0';
sb_json_str (sb, line);                                 /* column header line */
sb_puts (sb, ",\"lines\":[");
for (line = next; line && *line; line = next) {
    next = strchr (line, '\n');
    if (next)
        *next++ = '\0';
    if ((*line == '\0') || ((line[0] == '\r') && (line[1] == '\0')))
        continue;                                       /* the blank line after the header */
    if (!first)
        sb_puts (sb, ",");
    first = FALSE;
    sb_json_str (sb, line);
    }
sb_puts (sb, "]}");
free (text);
}

static void web_broadcast_history (void)
{
uint32 now = sim_os_msec (), i, want = 0;

for (i = 0; i < WEB_MAX_CLIENTS; i++)
    if ((web_clients[i].sock != INVALID_SOCKET) && web_clients[i].ws && (web_clients[i].hist_n > want))
        want = web_clients[i].hist_n;
if (want == 0)
    return;
if ((want != web_hist_count) || (web_last_hist == NULL) || ((now - web_last_hist_time) >= WEB_HIST_MSEC)) {
    SB sb = { NULL, 0, 0, FALSE };

    web_last_hist_time = now;
    web_history_json (&sb, want);
    if ((sb.p != NULL) && ((web_last_hist == NULL) || (web_last_hist_len != sb.len) ||
                           (memcmp (web_last_hist, sb.p, sb.len) != 0) || (want != web_hist_count))) {
        free (web_last_hist);
        web_last_hist = sb.p;
        web_last_hist_len = sb.len;
        web_hist_count = want;
        web_hist_ver++;
        }
    else
        free (sb.p);
    }
for (i = 0; i < WEB_MAX_CLIENTS; i++) {
    WEB_CLIENT *c = &web_clients[i];

    if ((c->sock == INVALID_SOCKET) || !c->ws || c->close_after || (c->hist_n == 0) ||
        (c->hist_ver == web_hist_ver) || (c->txlen - c->txoff >= 64 * 1024) || (web_last_hist == NULL))
        continue;
    ws_send_text (c, web_last_hist, web_last_hist_len);
    c->hist_ver = web_hist_ver;
    }
}

/* Push device/media and network changes.  The JSON is rebuilt twice a
   second, but only sent when it differs from what clients were last given
   (per-client version numbers, so a slow reader never misses an update). */

static char *web_last_devices = NULL;
static char *web_last_network = NULL;
static size_t web_last_devices_len = 0, web_last_network_len = 0;
static uint32 web_dev_ver = 0, web_net_ver = 0;
static uint32 web_last_events = 0;

static void web_update_cache (SB *fresh, char **cache, size_t *cache_len, uint32 *ver)
{
if (fresh->err || (fresh->p == NULL))
    return;
if ((*cache == NULL) || (*cache_len != fresh->len) || (memcmp (*cache, fresh->p, fresh->len) != 0)) {
    free (*cache);
    *cache = fresh->p;
    *cache_len = fresh->len;
    fresh->p = NULL;
    (*ver)++;
    }
}

/* Media I/O activity: per loaded disk/tape unit, the number of read and write
   operations the simulator has done on it (the disk and tape layers count
   them; see sim_disk.c / sim_tape.c).  The page turns increases into blinking
   R/W lamps, so this is only pushed when something changed. */

static void json_mediaio (SB *sb)
{
DEVICE *dptr;
uint32 i, j;
t_bool first = TRUE;

sb_puts (sb, "{\"type\":\"mediaio\",\"units\":[");
for (i = 0; (dptr = sim_devices[i]) != NULL; i++) {
    uint32 kind = DEV_TYPE (dptr);

    if (((kind != DEV_DISK) && (kind != DEV_TAPE)) || (dptr->flags & DEV_DIS))
        continue;
    for (j = 0; j < dptr->numunits; j++) {
        UNIT *uptr = dptr->units + j;
        uint32 r = 0, w = 0;

        if (!(uptr->flags & UNIT_ATT) || (uptr->flags & UNIT_DIS))
            continue;
        if (kind == DEV_DISK)
            sim_disk_io_counts (uptr, &r, &w);
        else
            sim_tape_io_counts (uptr, &r, &w);
        if (!first)
            sb_puts (sb, ",");
        first = FALSE;
        sb_puts (sb, "{\"unit\":");
        sb_json_str (sb, sim_uname (uptr));
        sb_printf (sb, ",\"kind\":\"%s\",\"readonly\":%s,\"reads\":%u,\"writes\":%u", (kind == DEV_DISK) ? "disk" : "tape",
                   (uptr->flags & UNIT_RO) ? "true" : "false", (unsigned)r, (unsigned)w);
        if (kind == DEV_TAPE)
            sb_printf (sb, ",\"pos\":%.0f", (double)uptr->pos);
        if (uptr->filename) {
            sb_puts (sb, ",\"file\":");
            sb_json_str (sb, uptr->filename);
            }
        sb_puts (sb, "}");
        }
    }
sb_puts (sb, "]}");
}

static void web_broadcast_mediaio (void)
{
uint32 now = sim_os_msec (), i;
t_bool wanted = FALSE;

for (i = 0; i < WEB_MAX_CLIENTS; i++)
    if ((web_clients[i].sock != INVALID_SOCKET) && web_clients[i].ws)
        wanted = TRUE;
if (!wanted)
    return;
if ((web_last_io == NULL) || ((now - web_last_io_time) >= WEB_IO_MSEC)) {
    SB sb = { NULL, 0, 0, FALSE };

    web_last_io_time = now;
    json_mediaio (&sb);
    web_update_cache (&sb, &web_last_io, &web_last_io_len, &web_io_ver);
    free (sb.p);
    }
for (i = 0; i < WEB_MAX_CLIENTS; i++) {
    WEB_CLIENT *c = &web_clients[i];

    if ((c->sock == INVALID_SOCKET) || !c->ws || c->close_after || (c->io_ver == web_io_ver) ||
        (c->txlen - c->txoff >= 64 * 1024) || (web_last_io == NULL))
        continue;
    ws_send_text (c, web_last_io, web_last_io_len);
    c->io_ver = web_io_ver;
    }
}

static void web_broadcast_events (void)
{
uint32 now = sim_os_msec (), i;

web_broadcast_console ();
web_broadcast_history ();
web_broadcast_mediaio ();
t_bool wanted = FALSE;

for (i = 0; i < WEB_MAX_CLIENTS; i++)
    if ((web_clients[i].sock != INVALID_SOCKET) && web_clients[i].ws)
        wanted = TRUE;
if (!wanted)
    return;
if ((web_last_devices == NULL) || ((now - web_last_events) >= WEB_EVENT_MSEC)) {
    SB d = { NULL, 0, 0, FALSE }, n = { NULL, 0, 0, FALSE };

    web_last_events = now;
    json_devices (&d);
    json_network (&n);
    web_update_cache (&d, &web_last_devices, &web_last_devices_len, &web_dev_ver);
    web_update_cache (&n, &web_last_network, &web_last_network_len, &web_net_ver);
    free (d.p);
    free (n.p);
    }
for (i = 0; i < WEB_MAX_CLIENTS; i++) {
    WEB_CLIENT *c = &web_clients[i];

    if ((c->sock == INVALID_SOCKET) || !c->ws || c->close_after || (c->txlen - c->txoff >= 64 * 1024))
        continue;
    if ((c->dev_ver != web_dev_ver) && web_last_devices) {
        ws_send_text (c, web_last_devices, web_last_devices_len);
        c->dev_ver = web_dev_ver;
        }
    if ((c->net_ver != web_net_ver) && web_last_network && (c->sock != INVALID_SOCKET)) {
        ws_send_text (c, web_last_network, web_last_network_len);
        c->net_ver = web_net_ver;
        }
    }
}

static void web_poll (void)
{
uint32 i;

if (web_master == INVALID_SOCKET)
    return;
for (;;) {                                                  /* accept new connections */
    SOCKET s = sim_accept_conn (web_master, NULL);
    WEB_CLIENT *c = NULL;

    if (s == INVALID_SOCKET)
        break;
    for (i = 0; i < WEB_MAX_CLIENTS; i++)
        if (web_clients[i].sock == INVALID_SOCKET) {
            c = &web_clients[i];
            break;
            }
    if (c == NULL) {
        sim_close_sock (s);
        continue;
        }
    memset (c, 0, sizeof (*c));
    c->sock = s;
    c->serial = ++web_serial;
    }
for (i = 0; i < WEB_MAX_CLIENTS; i++) {
    WEB_CLIENT *c = &web_clients[i];

    if (c->sock == INVALID_SOCKET)
        continue;
    client_read (c);
    if (c->sock != INVALID_SOCKET)
        client_flush (c);
    }
}

/* Answer the client that issued an injected command.  "started" means the
   command began a simulation run (the reply must not wait for it to end). */

static void web_finish_reply (t_bool started)
{
WEB_CLIENT *c;
SB sb = { NULL, 0, 0, FALSE };

if (web_reply_slot < 0)
    return;
c = &web_clients[web_reply_slot];
web_reply_slot = -1;
if ((c->sock == INVALID_SOCKET) || (c->serial != web_reply_serial) || c->close_after)
    return;
if (c->ws)
    sb_puts (&sb, "{\"type\":\"result\",\"op\":\"command\",\"command\":");
else
    sb_puts (&sb, "{\"command\":");
sb_json_str (&sb, web_cmd);
sb_puts (&sb, ",\"ok\":");
sb_puts (&sb, (started || (SCPE_BARE_STATUS (sim_last_cmd_stat) == SCPE_OK)) ? "true" : "false");
sb_puts (&sb, ",\"status\":");
sb_json_str (&sb, started ? "Running" : web_last_status ());
sb_printf (&sb, ",\"state\":\"%s\"}", web_run_state ());
if (c->ws)
    ws_send_text (c, sb.p, sb.len);
else
    respond_json (c, 200, &sb);
free (sb.p);
}

static t_stat web_svc (UNIT *uptr)
{
if (web_master == INVALID_SOCKET)
    return SCPE_OK;                                         /* detached */
web_finish_reply (TRUE);                                    /* a web command started the run */
web_poll ();
if (web_step_pending) {                                     /* single step done */
    web_step_pending = FALSE;
    web_paused = TRUE;
    }
if (web_paused)
    web_broadcast_state (TRUE);
while (web_paused && !web_halt_req) {
    t_stat c = sim_poll_kbd ();                             /* keep ^E working at the console */

    if (c == SCPE_STOP)
        web_halt_req = TRUE;
    web_poll ();
    web_broadcast_state (FALSE);
    sim_os_ms_sleep (5);
    }
if (web_halt_req) {                                         /* leave to the sim> prompt */
    web_halt_req = FALSE;
    web_paused = FALSE;
    web_step_pending = FALSE;
    sim_activate_after (uptr, WEB_POLL_USEC);
    return SCPE_STOP;
    }
web_broadcast_state (FALSE);
if (web_step_pending)
    sim_activate (uptr, 1);                                 /* come back after one instruction */
else
    sim_activate_after (uptr, WEB_POLL_USEC);
return SCPE_OK;
}

/* Called by the sim> command loop in place of a blocking read of stdin.
   While waiting for the operator, the web server keeps being serviced, so
   the simulator can be controlled from the web in any state.  Returns:

        0   web not attached; caller reads stdin as usual
        1   cbuf holds a command injected from the web
        2   stdin has input (or EOF); caller reads it as usual

   On POSIX systems stdin is polled non-blocking, which also copes with
   input already sitting in the stdio buffer. */

int sim_web_prompt_wait (const char *prompt, char *cbuf, size_t size)
{
#if !defined (_WIN32)
int flags = fcntl (0, F_GETFL, 0);
#endif
int ret = 2;

if (web_master == INVALID_SOCKET)
    return 0;
if (web_restart_pending) {                                  /* Restart: we have stopped, now boot */
    snprintf (web_cmd, sizeof (web_cmd), "BOOT %s", web_boot_default ());
    web_cmd_pending = TRUE;
    web_restart_pending = FALSE;
    }
else
    web_finish_reply (FALSE);                               /* previous web command has completed */
web_broadcast_state (TRUE);                                 /* show "stopped" at once */
printf ("%s", prompt);
fflush (stdout);
#if !defined (_WIN32)
if (flags != -1)
    fcntl (0, F_SETFL, flags | O_NONBLOCK);
#endif
for (;;) {
    if (web_master == INVALID_SOCKET)                       /* detached meanwhile */
        break;
    web_poll ();
    web_broadcast_state (FALSE);
    if (web_cmd_pending) {
        strlcpy (cbuf, web_cmd, size);
        web_cmd_pending = FALSE;
        printf ("%s\n", cbuf);                              /* echo as if typed */
        ret = 1;
        break;
        }
#if defined (_WIN32)
    if (WaitForSingleObject (GetStdHandle (STD_INPUT_HANDLE), WEB_POLL_USEC / 1000) == WAIT_OBJECT_0)
        break;
#else
    if (!web_stdin_eof) {
        int ch = getc (stdin);

        if (ch != EOF) {
            ungetc (ch, stdin);
            break;
            }
        if (feof (stdin)) {
            clearerr (stdin);
            if (sim_ttisatty ())                            /* ^D at the console: SCP ignores it */
                break;
            web_stdin_eof = TRUE;                           /* script/pipe ended: stay up for the web */
            }
        else
            clearerr (stdin);                               /* EAGAIN or EINTR: nothing yet */
        }
    sim_os_ms_sleep (WEB_POLL_USEC / 1000);
#endif
    }
#if !defined (_WIN32)
if (flags != -1)
    fcntl (0, F_SETFL, flags);
#endif
if (web_master == INVALID_SOCKET)
    return 0;
return ret;
}

/* ---------------------------------------------------------------------- */
/* Device                                                                  */
/* ---------------------------------------------------------------------- */

static t_stat web_reset (DEVICE *dptr)
{
web_paused = FALSE;
web_step_pending = FALSE;
web_halt_req = FALSE;
sim_cancel (&web_unit);                                     /* reset may follow a queue flush */
if (web_master != INVALID_SOCKET)
    sim_activate_after (&web_unit, WEB_POLL_USEC);
return SCPE_OK;
}

static t_stat web_attach (UNIT *uptr, CONST char *cptr)
{
char hostport[CBUFSIZE];
char *dup;
int parse_status = 0;
t_bool digits_only = TRUE;
const char *p;
SOCKET s;
uint32 i;

if ((cptr == NULL) || (*cptr == '\0'))
    return SCPE_2FARG;
if (web_master != INVALID_SOCKET)
    return sim_messagef (SCPE_ALATT, "Already attached\n");
for (p = cptr; *p; p++)
    if (!isdigit ((unsigned char)*p))
        digits_only = FALSE;
if (digits_only)                                            /* bare port: loopback only */
    snprintf (hostport, sizeof (hostport), "127.0.0.1:%s", cptr);
else
    strlcpy (hostport, cptr, sizeof (hostport));
s = sim_master_sock_ex (hostport, &parse_status, SIM_SOCK_OPT_REUSEADDR);
if (s == INVALID_SOCKET)
    return sim_messagef (SCPE_OPENERR, "Unable to listen on %s\n", hostport);
web_loopback = digits_only || (strncmp (hostport, "127.", 4) == 0) ||
               (strncasecmp (hostport, "localhost", 9) == 0) || (strncmp (hostport, "[::1]", 5) == 0);
web_master = s;
for (i = 0; i < WEB_MAX_CLIENTS; i++)
    web_clients[i].sock = INVALID_SOCKET;
dup = (char *)malloc (strlen (hostport) + 1);
if (dup == NULL) {
    sim_close_sock (s);
    web_master = INVALID_SOCKET;
    return SCPE_MEM;
    }
strcpy (dup, hostport);
uptr->filename = dup;
uptr->flags |= UNIT_ATT;
sim_activate_after (uptr, WEB_POLL_USEC);
sim_printf ("Web interface listening on http://%s/%s\n", hostport,
            web_loopback ? "" : "  (NOTE: not restricted to loopback; consider SET WEB TOKEN)");
if (!web_loopback && (web_token[0] == '\0'))
    sim_printf ("WARNING: anyone who can reach this port can control the simulator\n");
return SCPE_OK;
}

static t_stat web_detach (UNIT *uptr)
{
uint32 i;

if (!(uptr->flags & UNIT_ATT))
    return SCPE_OK;
for (i = 0; i < WEB_MAX_CLIENTS; i++)
    client_close (&web_clients[i]);
for (i = 0; i < WEB_MAX_CLIENTS; i++)
    web_clients[i].sock = INVALID_SOCKET;
if (web_master != INVALID_SOCKET)
    sim_close_sock (web_master);
web_master = INVALID_SOCKET;
web_paused = FALSE;
web_step_pending = FALSE;
web_cmd_pending = FALSE;
web_restart_pending = FALSE;
web_reply_slot = -1;
web_con_pend_len = 0;
web_con_in_head = web_con_in_tail = 0;
free (web_last_hist);
web_last_hist = NULL;
free (web_last_io);
web_last_io = NULL;
free (web_last_devices);
free (web_last_network);
web_last_devices = web_last_network = NULL;
sim_cancel (uptr);
free (uptr->filename);
uptr->filename = NULL;
uptr->flags &= ~UNIT_ATT;
return SCPE_OK;
}

static t_stat web_set_token (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
if ((cptr == NULL) || (*cptr == '\0')) {
    web_token[0] = '\0';
    return SCPE_OK;
    }
if (strlen (cptr) >= sizeof (web_token))
    return sim_messagef (SCPE_ARG, "Token too long (max %d characters)\n", (int)sizeof (web_token) - 1);
strlcpy (web_token, cptr, sizeof (web_token));
return SCPE_OK;
}

static t_stat web_set_media (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
if ((cptr == NULL) || (*cptr == '\0'))
    return SCPE_2FARG;
if (strlen (cptr) >= sizeof (web_media))
    return sim_messagef (SCPE_ARG, "Directory name too long\n");
strlcpy (web_media, cptr, sizeof (web_media));
return SCPE_OK;
}

static t_stat web_show_media (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf (st, "media directory %s\n", web_media);
return SCPE_OK;
}

static t_stat web_show_status (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
uint32 i, n = 0;

for (i = 0; i < WEB_MAX_CLIENTS; i++)
    if (web_clients[i].sock != INVALID_SOCKET)
        n++;
if (web_master == INVALID_SOCKET)
    fprintf (st, "not listening");
else
    fprintf (st, "listening on %s%s, %u client(s), %s", uptr->filename,
             web_loopback ? " (loopback only)" : "", (unsigned)n, web_run_state ());
fprintf (st, ", token %s, media %s\n", web_token[0] ? "required" : "not set", web_media);
return SCPE_OK;
}

static const char *web_description (DEVICE *dptr)
{
return "Web server, WebSocket and REST API with Blinkenlights panel";
}

static t_stat web_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
fprintf (st, "Web Interface (WEB)\n\n");
fprintf (st, "The WEB device serves a Blinkenlights front panel, a REST API and a\n");
fprintf (st, "WebSocket feed so the simulator can be watched and controlled from a\n");
fprintf (st, "browser or from scripts.\n\n");
fprintf (st, "  sim> ATTACH WEB 8080              listen on 127.0.0.1 port 8080\n");
fprintf (st, "  sim> ATTACH WEB 0.0.0.0:8080      listen on all interfaces\n");
fprintf (st, "  sim> SET WEB TOKEN=secret         require a token (Bearer or ?token=)\n");
fprintf (st, "  sim> SHOW WEB STATUS\n\n");
fprintf (st, "The interface is serviced only while the simulation is running or\n");
fprintf (st, "paused from the web.  HALT from the web returns to the sim> prompt.\n\n");
fprintf (st, "Endpoints: /api/state /api/registers /api/devices /api/memory\n");
fprintf (st, "           /api/pause /api/resume /api/step /api/halt  /ws\n");
fprintf (st, "Register and memory writes are accepted only while paused.\n");
return SCPE_OK;
}

void sim_web_init (void)
{
uint32 i;

for (i = 0; i < WEB_MAX_CLIENTS; i++)
    web_clients[i].sock = INVALID_SOCKET;
sim_register_internal_device (&web_dev);
}

#else /* !USE_WEBSERVER */

typedef int sim_web_not_enabled;                        /* keep the translation unit non-empty */

#endif
