/* vax_watch.c: VAX watch chip

   Copyright (c) 2011-2012, Matt Burke

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
   THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of the author shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from the author.

   wtc          Watch chip
   
   08-Nov-2012  MB      First version

   This file covers the watch chip (MC146818) which is used by several VAX
   models including the KA620, KA630, KA410, KA420 and KA820.
*/

#include "vax_defs.h"

/* control/status registers */

#define WTC_CSRA_RS     0x0F                            /* Rate Select Bits (Not Used by VMS) */
#define WTC_CSRA_V_DV   4
#define WTC_CSRA_M_DV   0x7
#define WTC_CSRA_DV     (WTC_CSRA_M_DV << WTC_CSRA_V_DV)
#define WTC_CSRA_UIP    0x80                            /* update in progess (BUSY) */
#define WTC_CSRA_WR     (WTC_CSRA_RS | WTC_CSRA_DV)
const char *wtc_dv_modes[] = {"4.194304MHz", "1.048576MHz", "32.768KHz", "Any", "Any", "Test-Only", "Test-Only", "Test-Only"};
BITFIELD wtc_csra_bits[] = {
    BITNCF(4),                              /* Rate Select - unused MBZ for VMS */
    BITFNAM(DV,3,wtc_dv_modes),             /* Divider Select */
    BIT(UIP),                               /* Update In Progress */
    ENDBITS
};

#define WTC_CSRB_DSE    0x01                            /* daylight saving en */
#define WTC_CSRB_2412   0x02                            /* 24/12hr select (1 -> 24 hr) */
#define WTC_CSRB_DM     0x04                            /* data mode (1 -> binary, 0 -> BCD) */
#define WTC_CSRB_SET    0x80                            /* set time */
#define WTC_CSRB_PIE    0x40                            /* periodic interrupt enable (Not Used by VMS) */
#define WTC_CSRB_AIE    0x20                            /* alarm interrupt enable (Not Used by VMS) */
#define WTC_CSRB_UIE    0x10                            /* update ended interrupt enable (Not Used by VMS) */
#define WTC_CSRB_SQWE   0x08                            /* square wave enable (Not Used by VMS) */
#define WTC_CSRB_WR     (WTC_CSRB_DSE | WTC_CSRB_2412 | WTC_CSRB_DM | WTC_CSRB_SET)
const char *wtc_dse_modes[] = {"Disabled", "Enabled"};
const char *wtc_hr_modes[] = {"12Hr", "24Hr"};
const char *wtc_data_modes[] = {"BCD", "Binary"};
BITFIELD wtc_csrb_bits[] = {
    BITFNAM(DST,1,wtc_dse_modes),           /* Daylight Savings Time Enable */
    BITFNAM(24HR,1,wtc_hr_modes),           /* 24/12 Hour Mode */
    BITFNAM(DM,1,wtc_data_modes),           /* Data Mode */
    BITNCF(4),                              /* Unused SQWE, UIE, AIE, PIE */
    BIT(SET),                               /* Set In Progress */
    ENDBITS
};

BITFIELD wtc_csrc_bits[] = {
    BITF(VALUE,8),                          /* Should be unused */
    ENDBITS
};
#define WTC_CSRD_VRT    0x80                            /* valid time */
#define WTC_CSRD_RD     (WTC_CSRD_VRT)
#define WTC_CSRD_WR     (WTC_CSRD_VRT)
BITFIELD wtc_csrd_bits[] = {
    BITNCF(7),
    BIT(VALID),                             /* Valid RAM and Time (VRT) */
    ENDBITS
};

BITFIELD wtc_value_bits[] = {
    BITFFMT(VALUE,8,%d),                    /* Decimal Value */
    ENDBITS
};
BITFIELD* wtc_bitdefs[] = {wtc_value_bits, wtc_value_bits, wtc_value_bits, wtc_value_bits,
                           wtc_value_bits, wtc_value_bits, wtc_value_bits, wtc_value_bits, 
                           wtc_value_bits, wtc_value_bits, wtc_csra_bits,  wtc_csrb_bits, 
                           wtc_csrc_bits,  wtc_csrd_bits,  wtc_value_bits, wtc_value_bits};

#define WTC_MODE_STD    0
#define WTC_MODE_VMS    1
const char *wtc_modes[] = {"Std", "VMS"};
BITFIELD wtc_mode_bits[] = {
    BITFNAM(MODE,1,wtc_modes),              /* Watch Date/Time mode */
    ENDBITS
};

int32 wtc_csra = 0;
int32 wtc_csrb = 0;
int32 wtc_csrc = 0;
int32 wtc_csrd = 0;
int32 wtc_mode = WTC_MODE_VMS;
uint8 wtc_ram[64];
t_int64 wtc_delta_ms = 0;                               /* milliseconds added to host time */
static char wtc_delta_file[CBUFSIZE] = "";              /* where to save the delta */

t_stat wtc_set (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat wtc_show (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat wtc_set_delta (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat wtc_show_delta (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat wtc_set_delta_file (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat wtc_show_delta_file (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat wtc_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *wtc_description (DEVICE *dptr);
t_stat wtc_reset (DEVICE *dptr);
void wtc_set_valid (void);
void wtc_set_invalid (void);

UNIT wtc_unit = { UDATA (NULL, 0, 0) };

REG wtc_reg[] = {
    { HRDATADF (CSRA, wtc_csra, 8, "CSRA", wtc_csra_bits) },
    { HRDATADF (CSRB, wtc_csrb, 8, "CSRB", wtc_csrb_bits) },
    { HRDATADF (CSRC, wtc_csrc, 8, "CSRC", wtc_csrc_bits) },
    { HRDATADF (CSRD, wtc_csrd, 8, "CSRD", wtc_csrd_bits) },
    { HRDATADF (MODE, wtc_mode, 8, "Watch Mode", wtc_mode_bits) },
    { BRDATAD  (RAM,  wtc_ram,  8, 8, sizeof (wtc_ram), "RAM") },
    { NULL }
    };

MTAB wtc_mod[] = {
    { MTAB_XTD|MTAB_VDV, 0, "TIME", "TIME={VMS|STD}", &wtc_set, &wtc_show, NULL, "Display watch time mode" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "DELTA", "DELTA=seconds",
        &wtc_set_delta, &wtc_show_delta, NULL, "Seconds added to host time (set by guest clock writes)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR|MTAB_NC, 0, "DELTAFILE", "DELTAFILE=file",
        &wtc_set_delta_file, &wtc_show_delta_file, NULL, "File the delta is saved to when the guest sets the clock" },
    { 0 }
    };

/* debugging bitmaps */
#define DBG_REG  0x0001                                 /* trace read/write registers */
#define DBG_TIME 0x0002                                 /* clock set/read compared to host clock */

DEBTAB wtc_debug[] = {
  {"REG",    DBG_REG},
  {"TIME",   DBG_TIME,  "clock set/read compared to host clock"},
  {0}
};

DEVICE wtc_dev = {
    "WTC", &wtc_unit, wtc_reg, wtc_mod,
    1, 16, 16, 1, 16, 8,
    NULL, NULL, &wtc_reset,
    NULL, NULL, NULL,
    NULL, DEV_DEBUG, 0, wtc_debug, NULL, NULL, &wtc_help, NULL, NULL,
    &wtc_description
    };

/* Register names for Debug tracing */
static const char *wtc_regs[] =
    {"SEC ", "SECA", "MIN ", "MINA", 
     "HR  ", "HRA ", "DOW ", "DOM ", 
     "MON ", "YEAR", "CSRA", "CSRB", 
     "CSRC", "CSRD"};

static uint32 wtc_written = 0;                          /* time regs written since SET */

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian, any month 1..24) */

static t_int64 wtc_days (int32 y, int32 m, int32 d)
{
t_int64 era, yoe, doy, doe;

y += (m - 1) / 12;                                      /* month 13 = Jan of next year */
m = ((m - 1) % 12) + 1;
y -= (m <= 2);
era = (y >= 0 ? y : y - 399) / 400;
yoe = y - era * 400;
doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
return era * 146097 + doe - 719468;
}

/* Decode a time register value written by the guest (binary or BCD) */

static int32 wtc_decode (int32 v)
{
if (wtc_csrb & WTC_CSRB_DM)                             /* binary mode? */
    return v;
return ((v >> 4) & 0xF) * 10 + (v & 0xF);               /* BCD */
}

#define WTC_DELTA_MIN   10                              /* delta changes below this (s) are ignored */
#define WTC_ABS(x)      (((x) < 0) ? -(x) : (x))

/* Convert a chip date/time to Unix time.

   STD mode: the chip holds a 2 digit year.  VMS mode: the chip holds the
   time since Jan 1 as a 1982 date, which is placed in the host's year, or
   the year before or after if that is nearer to now (across New Year).
   Wall clock time goes through mktime, so daylight saving time is handled. */

static time_t wtc_chip_to_unix (int32 yr, int32 mon, int32 dom, int32 hr, int32 min, int32 sec, time_t now)
{
struct tm tm, *htm;
time_t best = (time_t)-1, t;
int32 yday, y, hyear;

memset (&tm, 0, sizeof (tm));
tm.tm_hour = hr;
tm.tm_min = min;
tm.tm_sec = sec;
tm.tm_isdst = -1;                                       /* let mktime decide DST */
if (wtc_mode != WTC_MODE_VMS) {
    tm.tm_year = yr + ((yr < 70) ? 100 : 0);
    tm.tm_mon = mon - 1;
    tm.tm_mday = dom;
    return mktime (&tm);
    }
yday = (int32)(wtc_days (1900 + yr, mon, dom) - wtc_days (1982, 1, 1));  /* days since 1-Jan-1982 */
htm = localtime (&now);
if (htm == NULL)
    return (time_t)-1;
hyear = htm->tm_year;
for (y = hyear - 1; y <= hyear + 1; y++) {
    tm.tm_year = y;
    tm.tm_mon = 0;
    tm.tm_mday = 1 + yday;                              /* mktime normalises the day */
    tm.tm_hour = hr;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    t = mktime (&tm);
    if ((t != (time_t)-1) &&
        ((best == (time_t)-1) || (WTC_ABS ((t_int64)t - now) < WTC_ABS ((t_int64)best - now))))
        best = t;
    }
return best;
}

/* Log a chip date/time and how it compares with the host clock (Unix time).
   Returns the difference in milliseconds. */

static t_int64 wtc_show_time (const char *what, int32 yr, int32 mon, int32 dom,
                              int32 hr, int32 min, int32 sec)
{
struct timespec now;
time_t chip;
struct tm htm, *tp;
t_int64 diff, adiff;

sim_rtcn_get_time (&now, TMR_CLK);
chip = wtc_chip_to_unix (yr, mon, dom, hr, min, sec, now.tv_sec);
tp = localtime (&now.tv_sec);
if ((chip == (time_t)-1) || (tp == NULL))
    return 0;
htm = *tp;
diff = (((t_int64)chip - now.tv_sec) * 1000) - (now.tv_nsec / 1000000);
adiff = WTC_ABS (diff);
sim_debug (DBG_TIME, &wtc_dev, "%s: chip %02d-%02d-%02d %02d:%02d:%02d, host %04d-%02d-%02d %02d:%02d:%02d.%03d, "
           "chip-host %c%dd %02d:%02d:%02d.%03d\n", what, yr, mon, dom, hr, min, sec,
           htm.tm_year + 1900, htm.tm_mon + 1, htm.tm_mday, htm.tm_hour, htm.tm_min, htm.tm_sec,
           (int)(now.tv_nsec / 1000000), (diff < 0) ? '-' : '+', (int)(adiff / 86400000),
           (int)((adiff % 86400000) / 3600000), (int)((adiff % 3600000) / 60000),
           (int)((adiff % 60000) / 1000), (int)(adiff % 1000));
return diff;
}

/* Is this line "set wtc delta=..." (any case/spacing, but not deltafile)? */

static t_bool wtc_is_delta_line (const char *l)
{
static const char *words[] = {"SET", "WTC", "DELTA"};
int32 i;

for (i = 0; i < 3; i++) {
    size_t n = strlen (words[i]);

    while ((*l == ' ') || (*l == '\t'))
        l++;
    if (strncasecmp (l, words[i], n) != 0)
        return FALSE;
    l += n;
    if ((i < 2) && (*l != ' ') && (*l != '\t'))         /* word must end here */
        return FALSE;
    }
while ((*l == ' ') || (*l == '\t'))
    l++;
return (*l == '=');
}

/* Save the delta by rewriting the "set wtc delta=" line in the delta file,
   which is normally the configuration file itself (SET WTC DELTAFILE=%0).
   If there is no such line one is added after "set wtc deltafile=".  The
   rest of the file is kept as is; the new file replaces it by rename. */

static void wtc_save_delta (void)
{
FILE *f;
char *buf = NULL, *p, *eol, tmp[CBUFSIZE + 8], line[64];
long len = 0;
t_bool done = FALSE;
const char *nl = "\n";

if (wtc_delta_file[0] == '\0')
    return;
snprintf (line, sizeof (line), "set wtc delta=%.3f", (double)wtc_delta_ms / 1000.0);
f = fopen (wtc_delta_file, "rb");                       /* read the current file */
if (f != NULL) {
    fseek (f, 0, SEEK_END);
    len = ftell (f);
    fseek (f, 0, SEEK_SET);
    buf = (char *)calloc (len + 1, 1);
    if ((buf == NULL) || (fread (buf, 1, len, f) != (size_t)len)) {
        fclose (f);
        free (buf);
        sim_printf ("WTC: can't read delta file %s\n", wtc_delta_file);
        return;
        }
    fclose (f);
    if (strstr (buf, "\r\n") != NULL)
        nl = "\r\n";
    }
snprintf (tmp, sizeof (tmp), "%s.tmp", wtc_delta_file);
f = fopen (tmp, "wb");
if (f == NULL) {
    free (buf);
    sim_printf ("WTC: can't write %s\n", tmp);
    return;
    }
for (p = buf; (p != NULL) && (*p != '\0'); p = eol) {    /* copy line by line */
    size_t n;

    eol = strchr (p, '\n');
    eol = (eol != NULL) ? eol + 1 : p + strlen (p);
    n = (size_t)(eol - p);
    if (wtc_is_delta_line (p)) {                        /* replace the delta line */
        if (!done)
            fprintf (f, "%s%s", line, nl);
        done = TRUE;
        continue;
        }
    fwrite (p, 1, n, f);
    if ((!done) && (strncasecmp (p, "set wtc deltafile", 17) == 0)) {
        if (p[n - 1] != '\n')
            fputs (nl, f);
        fprintf (f, "%s%s", line, nl);                  /* none yet: add after deltafile */
        done = TRUE;
        }
    }
if (!done)                                              /* empty/new file: just the line */
    fprintf (f, "%s%s", line, nl);
fclose (f);
free (buf);
if (rename (tmp, wtc_delta_file) != 0) {
    remove (wtc_delta_file);                            /* Windows: rename won't replace */
    if (rename (tmp, wtc_delta_file) != 0)
        sim_printf ("WTC: can't update delta file %s\n", wtc_delta_file);
    }
}

/* Guest finished setting the clock (SET cleared): update the delta.

   The guest only writes whole seconds and times its writes for the chip's
   once-a-second update, which is not simulated, so a write is only accurate
   to a second or so (VMS rewrites the clock this way at every boot and
   shutdown).  Writes within WTC_DELTA_MIN seconds of the host clock mean
   "host time" (delta 0), and writes within WTC_DELTA_MIN seconds of the
   current delta leave it unchanged, so those rewrites cannot make it drift. */

static void wtc_report_set (void)
{
int32 hr = wtc_ram[4];
t_int64 diff, new_ms;

if ((wtc_csrb & WTC_CSRB_2412) == 0) {                  /* 12 hour mode? */
    int32 pm = hr & 0x80;

    hr = wtc_decode (hr & 0x7F) % 12;
    if (pm)
        hr = hr + 12;
    }
else
    hr = wtc_decode (hr);
diff = wtc_show_time ("guest set clock", wtc_decode (wtc_ram[9]), wtc_decode (wtc_ram[8]),
                      wtc_decode (wtc_ram[7]), hr, wtc_decode (wtc_ram[2]), wtc_decode (wtc_ram[0]));
new_ms = ((diff + ((diff < 0) ? -500 : 500)) / 1000) * 1000;   /* nearest second */
if (WTC_ABS (new_ms) < (WTC_DELTA_MIN * 1000))          /* close to host time? */
    new_ms = 0;
if (WTC_ABS (new_ms - wtc_delta_ms) < (WTC_DELTA_MIN * 1000)) {
    sim_debug (DBG_TIME, &wtc_dev, "guest set clock: within %d s of the current delta (%+.3f s), unchanged\n",
               WTC_DELTA_MIN, (double)wtc_delta_ms / 1000.0);
    return;
    }
wtc_delta_ms = new_ms;                                  /* reads now return host + delta */
wtc_save_delta ();
sim_debug (DBG_TIME, &wtc_dev, "guest set clock: delta is now %+.3f seconds%s%s\n", (double)wtc_delta_ms / 1000.0,
           wtc_delta_file[0] ? ", saved to " : "", wtc_delta_file);
}

/* VMS mode rebases the current time of year onto 1982

   VMS uses the watch chip to hold the time since Jan 1 of the current
   year, written as a date in 1982 (the real year is kept on the system
   disk).  So take the local wall clock time since Jan 1, add it to
   1982-01-01 00:00:00 and let gmtime() produce the 1982 date.  Leap
   years need no special handling: day 366 of a leap year comes out as
   1983-01-01, which is what a real chip set by VMS would show after
   running past the end of 1982.
*/

#define WTC_VMS_BASE    ((time_t)378691200)             /* 1982-01-01 00:00:00 UTC */

static struct tm *wtc_vms_time (const struct tm *ctm)
{
time_t t;

t = WTC_VMS_BASE + ((((((time_t)ctm->tm_yday * 24) +   /* local sec since Jan 1 */
                       ctm->tm_hour) * 60) +
                       ctm->tm_min) * 60) +
                       ctm->tm_sec;
return gmtime (&t);                                     /* decompose as 1982 date */
}

int32 wtc_rd (int32 rg)
{
int32 val = 0;
time_t curr;
struct timespec now;
struct tm *ctm = NULL;

if (rg < 10) {                                          /* time reg? */
    sim_rtcn_get_time (&now, TMR_CLK);
    curr = now.tv_sec;                                  /* get curr time */
    if (curr == (time_t) -1)                            /* error? */
        return 0;
    curr = (time_t)((((t_int64)now.tv_sec * 1000) + (now.tv_nsec / 1000000) + wtc_delta_ms) / 1000);
    ctm = localtime (&curr);                            /* decompose */
    if (ctm == NULL)                                    /* error? */
        return 0;
    if (wtc_mode == WTC_MODE_VMS) {
        ctm = wtc_vms_time (ctm);                       /* same time of year in 1982 */
        if (ctm == NULL)                                /* error? */
            return 0;
        }
    }

val = wtc_ram[rg];                                      /* start with RAM data */

if (rg < 14) {
    switch(rg) {

        case 0:                                         /* seconds */
            val = ctm->tm_sec;
            break;

        case 2:                                         /* minutes */
            val = ctm->tm_min;
            break;

        case 4:                                         /* hours */
            val = ctm->tm_hour;
            break;

        case 6:                                         /* day of week */
            val = ctm->tm_wday + 1;                     /* chip: 1-7, Sunday = 1 */
            break;

        case 7:                                         /* day of month */
            val = ctm->tm_mday;
            break;

        case 8:                                         /* month */
            val = ctm->tm_mon + 1;
            break;

        case 9:                                         /* year */
            val = (int32)(ctm->tm_year % 100);          /* VMS: 82 (83 on day 366) */
            wtc_show_time ("guest read clock", val, ctm->tm_mon + 1, ctm->tm_mday,
                           ctm->tm_hour, ctm->tm_min, ctm->tm_sec);
            break;

        case 10:                                        /* CSR A */
            val = wtc_csra;
            break;

        case 11:                                        /* CSR B */
            val = wtc_csrb;
            break;

        case 12:                                        /* CSR C */
            val = wtc_csrc;
            break;

        case 13:                                        /* CSR D */
            val = wtc_csrd & WTC_CSRD_RD;
            wtc_set_valid ();
            break;
            }
    sim_debug(DBG_REG, &wtc_dev, "wtc_rd(rg=%d [%s], data=0x%X) ", rg, wtc_regs[rg], val);
    sim_debug_bits(DBG_REG, &wtc_dev, wtc_bitdefs[rg], (uint32)val, (uint32)val, TRUE);
    }
else
    sim_debug(DBG_REG, &wtc_dev, "wtc_rd(rg=%d [RAM], data=0x%X)\n", rg, val);

return val;
}

int32 wtc_rd_pa (int32 pa)
{
return wtc_rd ((pa & (sizeof (wtc_ram) - 1)) >> 1);
}

void wtc_wr (int32 rg, int32 val)
{
int32 new_val = val;

val = val & 0xFF;

wtc_ram[rg] = val;                                      /* Save data in RAM */
if (rg < 10)                                            /* time register? */
    wtc_written |= (1u << rg);
switch(rg) {                                            /* register behaviors */

    case 10:                                            /* CSR A */
        val = val & WTC_CSRA_WR;
        new_val = wtc_csra = (wtc_csra & ~WTC_CSRA_WR) | val;
        break;

    case 11:                                            /* CSR B */
        val = val & WTC_CSRB_WR;
        if (((wtc_csrb & WTC_CSRB_SET) == 0) && (val & WTC_CSRB_SET)) {
            wtc_written = 0;                            /* guest starts setting the clock */
            sim_debug (DBG_TIME, &wtc_dev, "guest started setting clock (SET=1)\n");
            }
        new_val = wtc_csrb = (wtc_csrb & ~WTC_CSRB_WR) | val;
        if (((val & WTC_CSRB_SET) == 0) && wtc_written) {
            wtc_report_set ();                          /* SET cleared: clock was set */
            wtc_written = 0;
            }
        break;

    case 12:                                            /* CSR C */
        break;

    case 13:                                            /* CSR D */
        val = val & WTC_CSRD_WR;
        new_val = wtc_csrd = (wtc_csrd & ~WTC_CSRD_WR) | val;
        break;
        }

if (rg < 14) {
    sim_debug(DBG_REG, &wtc_dev, "wtc_wr(rg=%d [%s], data=0x%X) ", rg, wtc_regs[rg], val);
    sim_debug_bits(DBG_REG, &wtc_dev, wtc_bitdefs[rg], (uint32)new_val, (uint32)new_val, TRUE);
    }
else
    sim_debug(DBG_REG, &wtc_dev, "wtc_wr(rg=%d [RAM], data=0x%X)\n", rg, val);
}

void wtc_wr_pa (int32 pa, int32 val, int32 lnt)
{
wtc_wr ((pa & (sizeof (wtc_ram) - 1)) >> 1, val);
if (lnt == 4)
    wtc_wr (((pa + 2) & (sizeof (wtc_ram) - 1)) >> 1, val);
}

t_stat wtc_reset (DEVICE *dptr)
{
if (sim_switches & SWMASK ('P')) {                      /* powerup? */
    wtc_csra = 0;
    wtc_csrb = 0;
    wtc_csrc = 0;
    wtc_csrd = 0;
    wtc_mode = WTC_MODE_VMS;
    }
return SCPE_OK;
}

t_stat wtc_set (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
char gbuf[CBUFSIZE];

if (cptr != NULL) {
    cptr = get_glyph (cptr, gbuf, 0);
    if (*cptr)
        return SCPE_2MARG;
    if (strcmp (gbuf, "STD") == 0)
        wtc_mode = WTC_MODE_STD;
    else {
        if (strcmp (gbuf, "VMS") == 0)
            wtc_mode = WTC_MODE_VMS;
        else
            return SCPE_ARG;
        }
    }
return SCPE_OK;
}

t_stat wtc_show (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf(st, "time=%s", (wtc_mode ? "vms" :"std"));
return SCPE_OK;
}

t_stat wtc_set_delta (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
char *end;
double v;

if ((cptr == NULL) || (*cptr == '\0'))
    return SCPE_ARG;
v = strtod (cptr, &end);
if ((*end != '\0') || (end == cptr))
    return SCPE_ARG;
wtc_delta_ms = (t_int64)((v * 1000.0) + ((v < 0) ? -0.5 : 0.5));
return SCPE_OK;
}

t_stat wtc_show_delta (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf (st, "delta=%+.3fs", (double)wtc_delta_ms / 1000.0);
return SCPE_OK;
}

t_stat wtc_set_delta_file (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
if ((cptr == NULL) || (*cptr == '\0'))
    return SCPE_ARG;
snprintf (wtc_delta_file, sizeof (wtc_delta_file), "%s", cptr);
return SCPE_OK;
}

t_stat wtc_show_delta_file (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf (st, "deltafile=%s", wtc_delta_file[0] ? wtc_delta_file : "none");
return SCPE_OK;
}

void wtc_set_valid (void)
{
wtc_csra |= (2 << WTC_CSRA_V_DV);
wtc_csrb |= (WTC_CSRB_DM | WTC_CSRB_2412);
wtc_csrd |= WTC_CSRD_VRT;
}

void wtc_set_invalid (void)
{
wtc_csrd &= ~WTC_CSRD_VRT;
}

t_stat wtc_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
fprintf (st, "Watch Chip (WTC)\n\n");
fprintf (st, "The WTC simulates the MC146818 watch chip.  It recognizes the following options:\n\n");
fprintf (st, "  SET WTC TIME=STD            standard time mode\n");
fprintf (st, "  SET WTC TIME=VMS            VMS time mode\n\n");
fprintf (st, "When running in standard mode the current year reported by the watch chip is\n");
fprintf (st, "determined by the date/time of the host system.  When running in VMS mode the\n");
fprintf (st, "chip holds the time since Jan 1 as a date in 1982, as VMS itself writes it\n");
fprintf (st, "(the year reads 82, or 83 on Dec 31 of a leap year).  The default mode is VMS.\n\n");
fprintf (st, "When the guest sets the clock, SIMH keeps the difference from the host clock\n");
fprintf (st, "(the delta, in Unix seconds) and adds it to every later read, as a battery-backed\n");
fprintf (st, "chip would.  Writes within 10 seconds of the host clock count as delta 0, and\n");
fprintf (st, "writes within 10 seconds of the current delta leave it unchanged:\n\n");
fprintf (st, "  SET WTC DELTA=seconds       seconds (to 1 ms) added to the host time (default 0)\n");
fprintf (st, "  SET WTC DELTAFILE=file      when the guest sets the clock, rewrite the\n");
fprintf (st, "                              'set wtc delta=n' line in file.  In a .ini use\n");
fprintf (st, "                              SET WTC DELTAFILE=%%0 to keep it in that .ini\n");
fprintf (st, "  SET WTC DEBUG=TIME          log each time the guest sets or reads the clock,\n");
fprintf (st, "                              compared with the host clock (needs SET DEBUG <file>)\n\n");
return SCPE_OK;
}

const char *wtc_description (DEVICE *dptr)
{
return "watch chip";
}
