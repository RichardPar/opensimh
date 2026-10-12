# My Open SIMH fork: VAX clock fixes and a web interface

This is my personal fork of Open SIMH ([github.com/RichardPar/opensimh](https://github.com/RichardPar/opensimh)).
It is **not** the official Open SIMH repository. If you want that, go to
[github.com/open-simh/simh](https://github.com/open-simh/simh).

I made this fork because of a thread about VAX clocks misbehaving under SIMH: clocks
racing or dying after a reboot, and a VMS system that always came up about 19 hours
out. I dug into the timekeeping code, tested against real VMS installs (VMS 5.5-2 on
a MicroVAX II and VMS 6.2 on a VAXstation 4000-60) and fixed what I found.

I've since also added an **optional web interface** to this fork (a front panel, guest
console, media and network view, all in the browser). It is a build-time option and is
left out unless you ask for it: see [section 4](#4-an-optional-web-interface-sim_webc-web) and
[web/README.md](web/README.md). Everything else in the tree is plain Open SIMH.

## What I changed

### 1. The clock goes wrong after BOOT / RUN / RESTORE (`sim_timer.c`)

If idling is on (`SET CPU IDLE`) and you restart the machine from the `sim>` prompt,
or restore a saved state, the clock calibration gets thrown off. The guest clock
then crawls or races, and the console can stop responding. The cause is a leftover
timestamp from the previous run: SIMH resets simulated time to zero on a restart but
kept the last idle time, and the first idle after that did its sums with a huge
negative number.

The fix is two lines that reset that timestamp when the timers restart. It is the
same change as open-simh issue #581 / pull request #582 (credit to its author for
spotting it). It hasn't been merged upstream yet, so I've carried it here.

**Applies to:** every simulator. It's framework code, not tied to any one clock chip,
but you'll mostly notice it on the PDP-11 (KW11-L line clock) and the VAX (interval
timer), where people run with idling enabled.

### 2. The Motorola MC146818 watch chip gave bad dates (`VAX/vax_watch.c`)

The time-of-year clock on the smaller VAXen is a **Motorola MC146818** real-time
clock (DEC calls it the "watch chip"; later boards used pin-compatible parts such as
the Dallas DS1287). SIMH simulates the MC146818.

VMS doesn't use the MC146818 as a normal calendar. It stores the time
since 1 January as a date in 1982 and keeps the real year on the system disk. SIMH
fakes this by patching the date for leap years, and that code had bugs:

- On 31 December of a leap year the month register read **13**.
- The year 2100 was treated as a leap year.
- The day-of-week register counted 0-6 with Sunday = 0. The real chip counts 1-7
  with Sunday = 1.

I replaced the leap-year patching with a simpler approach: take the time since
1 January and add it to 1982-01-01. There are no special cases left. On the last
day of a leap year the chip now reads 83-01-01, which is what a real chip set by VMS
would show.

To be fair, VMS 5.5-2 handled the old month-13 value fine, so this is a correctness
fix rather than something users were tripping over.

### 3. MC146818 clock settings now stick to the machine, not the disk (`VAX/vax_watch.c`)

This one explains the "19 hours out" report.

On real hardware, when VMS sets the time it writes it into the MC146818. SIMH used
to throw that write away and keep returning the host's time. VMS saves the time you
gave it together with the clock reading at that moment, so any difference got stored
**on the system disk**. That offset then showed up on every boot, on any VAX model,
until someone ran `SET TIME` with the correct time. I reproduced exactly that: a disk
set 19 hours out booted 19 hours out every time, while another disk on the same SIMH
was fine.

Now SIMH works like the real chip. When the guest sets the clock, SIMH remembers the
difference from the host clock (a delta, in Unix seconds, so daylight saving doesn't
confuse it) and adds it to every later read. It can save the delta straight back
into your `.ini` file so it survives a restart.

VMS rewrites the clock itself at every boot and shutdown, about a second off each
time. So small writes are ignored: anything within 10 seconds of the host counts as
"no offset", and anything within 10 seconds of the current offset leaves it alone.
Real changes, like someone setting the clock hours out, are still kept.

New commands:

```
SET WTC DELTA=seconds     offset added to the host time (default 0)
SET WTC DELTAFILE=%0      when the guest sets the clock, rewrite the
                          "set wtc delta=" line in this .ini file
SET WTC DEBUG=TIME        log every clock set/read against the host clock
                          (needs SET DEBUG <file>)
```

A typical `.ini`, put these before `boot`:

```
set wtc delta=0
set wtc deltafile=%0
set debug clock.log
set wtc debug=time
```

`%0` is SIMH's name for the `.ini` file being run, so the offset is kept in your
own config file. `HELP WTC` has the details.

**Changes 2 and 3 apply to** the VAX models with the MC146818 watch chip:

| Simulator | Machine |
|---|---|
| `microvax2` | MicroVAX II / VAXstation II (KA630) |
| `rtvax1000` | rtVAX 1000 (KA620) |
| `microvax2000` | MicroVAX 2000 / VAXstation 2000 (KA410) |
| `microvax3100`, `microvax3100e` | MicroVAX 3100 models 10/20/10e/20e |
| `vaxstation3100m30`, `vaxstation3100m38` | VAXstation 3100 model 30/38 |
| `vaxstation3100m76` | VAXstation 3100 model 76 (KA43) |
| `vaxstation4000m60` | VAXstation 4000-60 (KA46) |
| `microvax3100m80` | MicroVAX 3100 model 80 (KA47) |
| `vaxstation4000vlc` | VAXstation 4000 VLC (KA48) |
| `infoserver100`, `infoserver150vxt`, `infoserver1000` | InfoServers |
| `vax8200` | VAX 8200/8250 (KA820) |

They do **not** apply to the `vax` (MicroVAX 3900), `microvax1`, `vax730`, `vax750`,
`vax780` or `vax8600`. Those have no MC146818; they keep time in the TODR, a 32-bit
time-of-year counter register in the CPU (on the 3900's KA655 it lives in the CVAX
SSC support chip), and SIMH's TODR code already kept what VMS wrote.

### 4. An optional web interface (`sim_web.c`, `web/`)

This one is a new feature, not a fix, and it is **off unless you build it in**:

    make WEBSERVER=1 pdp11 vax          (also pdp15, pdp9, pdp7, pdp4 ...)

Then, at the `sim>` prompt (or in your `.ini` file):

    sim> ATTACH WEB 8080                (listens on 127.0.0.1 only)
    sim> SET WEB TOKEN=secret           (optional)

and open <http://127.0.0.1:8080/>. It is a single-threaded server inside the simulator, with a
REST API and a WebSocket that pushes events to the page, so it keeps working whether the machine
is stopped at `sim>`, running, or paused. In the browser you get:

- a **front panel**: address lamps (the PC), data lamps (the word at the PC) and status lamps
  (PSW/PSL, or the accumulator on the 18-bit PDPs), with Pause / Continue / Step / Halt / Reset / Run
  and a **Restart** button with a drive selector, plus the registers;
- the last 10/20/50 **instructions** (the CPU's own `SHOW CPU HISTORY`), with their operand data;
- a **terminal** for the guest's console, so you can log in to RSX or VMS from the browser;
- **devices and media**: load and unload disks and tapes (even while the guest runs), choose
  the boot drive, and see **media I/O** activity (read and write counts with blinking lamps);
- **network**: MAC address, host adapter and live counters for the Ethernet cards (XQ/XU);
- memory examine / deposit, and a box to send any SCP command.

**Applies to:** I've run it on the PDP-11 (11/70 and 11/73), the MicroVAX 3900 and the PDP-4/7/9/15.
It is not part of upstream Open SIMH. The core changes are small and are all inside
`#ifdef USE_WEBSERVER`: a few lines in `scp.c`, `sim_console.c`, `sim_disk.c` and `sim_tape.c`,
a build switch in the `makefile`, and the new files `sim_web.c`, `sim_web.h` and `sim_web_page.h`
(generated from `web/blinkenlights.html`). Without `WEBSERVER=1` none of it is compiled.

It has no TLS and `/api/command` can run any SCP command, so keep it on loopback (the default) or
behind an SSH tunnel or reverse proxy, and set a token if you open it up. The details (endpoints,
events, security, limits) are in [web/README.md](web/README.md).

## If your VMS clock is already out

If an existing system disk comes up a fixed amount off, the offset is saved on that
disk. Boot it once and set the correct time explicitly, then shut down cleanly:

```
$ SET TIME=28-SEP-2026:14:30:00
```

A bare `SET TIME` won't do it, because it recalculates from the same saved values.

---

The original Open SIMH README follows.

# Open SIMH machine simulator

This is the codebase of SIMH, a framework and collection of computer system simulators.

SIMH was created by Bob Supnik, originally at Digital Equipment Corporation, and extended by contributions of many other people.  It is now an open source project, licensed under an MIT open source license (see [LICENSE.txt](LICENSE.txt) for the specific wording).  The project gatekeepers are the members of the [SIMH Steering Group](SIMH-SG.md).  We welcome and encourage contributions from all.  Contributions will be covered by the project license.

The Open SIMH code base was taken from a code base maintained by Mark Pizzolato as of 12 May 2022.  From that point onward there is no connection between that source and the Open SIMH code base.  A detailed listing of features as of that point may be found in [SIMH-V4-status](SIMH-V4-status.md).

## PLEASE NOTE

**Do not** contribute material taken from `github.com/simh/simh` unless you are the author of the material in question.

<a href="https://scan.coverity.com/projects/open-simh-simh">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/29458/badge.svg"/>
</a>
