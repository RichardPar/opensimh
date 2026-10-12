# Web interface (optional, build-time)

> This is a feature of **my fork** of Open SIMH ([github.com/RichardPar/opensimh](https://github.com/RichardPar/opensimh)),
> not of the official Open SIMH. See the [main README](../README.md) for the fork overview.

An embedded HTTP server, WebSocket event feed and REST API with a web
console: front-panel lamps and controls, device/media management (load and
unload disks and tapes), live Ethernet statistics, memory and an SCP command
box. It is compiled in only when `USE_WEBSERVER` is defined:

    make WEBSERVER=1 pdp11 vax

Without `WEBSERVER=1` nothing is added to the simulators (`sim_web.c` compiles
to an empty unit). CMake / Visual Studio builds are not wired up yet.

## Simulators

Verified on the PDP-11 (11/70, 11/73), VAX (MicroVAX 3900) and the 18-bit PDP-4/7/9/15
(`make WEBSERVER=1 pdp15 pdp9 pdp7 pdp4`). The panel adapts to the machine: the address lamps
follow the PC's width (13/15/17/22/32 bits), the data lamps show the memory word at the PC (up to 32 bits), and
the status row is the PSW/PSL, or the accumulator (AC) on the 18-bit machines, which also get AC/L/MQ/...
in the register card. Simulators without a CPU instruction history simply show the trace switch as off.

## Use

    sim> SET WEB TOKEN=secret       (optional)
    sim> ATTACH WEB 8080            (127.0.0.1 only; use 0.0.0.0:8080 to expose)
    sim> BOOT ...

then open <http://127.0.0.1:8080/?token=secret>.

The server runs on the simulator's main thread and is serviced in **every**
state: while running (a 20 ms event), while paused, and while stopped at the
`sim>` prompt (the prompt loop polls the web in place of a blocking read).
So you can start, pause, step, halt, reset and boot entirely from the web,
and `sim>` commands from the web are run exactly as if typed.

While WEB is attached: the prompt has no line editing/history, and EOF on a
non-tty stdin does not exit, so `pdp11 foo.ini </dev/null` runs headless
(send `{"command":"exit"}` to `/api/command`, or kill it, to quit). The
stdin handling is POSIX; on Windows it is a best-effort poll and untested.

## The page is event driven

`blinkenlights.html` makes no HTTP calls other than loading itself: everything
runs over `/ws`. The server **pushes**

| event | when |
|---|---|
| `state` | ~10/s: run state, PC, status word, registers, lights, last status |
| `devices` | on connect and whenever a unit is attached/detached/enabled |
| `network` | on connect and whenever an Ethernet adapter's counters or link change |
| `mediaio` | read/write operation counts of every loaded disk and tape unit, when they change |
| `history` | the CPU's last N instructions, while the **Instructions** switch on the Panel is on |
| `console` | guest console output as it is written (plus ~32 KB of history on connect) |

and the page sends requests that are answered by events: `{"cmd":"media","path":..}`
→ `media`; `memory` / `deposit` → `memory`; `attach` / `detach` /
`command` / `run` / `boot` / `reset` → `result` (or `error`). Packet rates on the
Network tab are computed in the browser from the pushed counters.

## Instruction history

The **Instructions** card on the Panel tab shows the last 10/20/50 instructions
with PC, status and operand data, using the CPU's own `SHOW CPU HISTORY` text
(so it works for the PDP-11 and VAX). Switching it on runs `SET CPU HISTORY=128`;
switching it off runs `SET CPU HISTORY=0`, because recording costs a little speed.
Over the WebSocket: `{"cmd":"history","count":20,"enable":true}` (count 0 =
unsubscribe). If you enabled history yourself (`SET CPU HISTORY=…` in your ini),
the page picks that up and shows it.

## Terminal (guest console)

The **Terminal** tab is the emulated machine's own console, so you can log in
to RSX/VMS from the browser. Output written through `sim_putchar` is pushed as
`console` events; keystrokes go back as `{"cmd":"input","data":"…"}` and are
fed to the guest through `sim_poll_kbd`. The page has a small built-in
VT100/VT220-style emulator (80×24, scrollback, cursor addressing, bold /
underline / reverse; no external libraries). Input only works while the
simulator is running. As at a real console, SIMH's stop character (**Ctrl-E**)
halts the simulation. Two small hooks in `sim_console.c` (inside
`#ifdef USE_WEBSERVER`) make this work. `SCP` output (the `sim>` prompt and
messages) still goes to the real console, not the Terminal tab.

## Media and network

    sim> SET WEB MEDIA=/srv/simh/media    (directory the media browser may list; default .)

* **Load / unload** works in any state, including while the guest is running
  (SCP's own `ATTACH`/`DETACH`, so every device's usual rules apply). It is like
  swapping a pack on a live drive: the guest sees the unit go offline/online.
* The browser is confined to the MEDIA directory (relative paths, no `..`),
  but the "File" box accepts any path, like `ATTACH` does at the console.
* Network cards (`XQ`, `XU`) show MAC, type, link/host adapter and every counter
  the device's own `SHOW … STATS` reports. "Connect…" attaches a host adapter
  (`nat:`, `eth0`, `tap:tap0` …); the card must be enabled first
  (`SET XQ ENABLE`).

## REST API

| Method | Path | Notes |
|---|---|---|
| GET | `/api/state` | `state` = `running`/`paused`/`stopped`, PC, status word, R0-R15, lights, `last_status` |
| GET | `/api/registers[?device=D]` | all registers of a device (default: CPU) |
| PUT | `/api/registers/NAME` | `{"value":..,"index":..}` — stopped or paused |
| GET | `/api/devices` | devices (kind, enabled), units, attached files, read-only, size |
| GET | `/api/network` | Ethernet adapters: MAC, type, host adapter, counters |
| GET | `/api/media[?path=sub]` | files and folders under the MEDIA directory |
| POST | `/api/attach` | `{"unit":"RQ0","file":"x.dsk","readonly":false,"new":false}` — any state |
| POST | `/api/detach` | `{"unit":"RQ0"}` — any state |
| GET | `/api/memory?addr=A&count=N` | physical RAM only (never the I/O page) |
| PUT | `/api/memory` | `{"addr":..,"value":..}` — stopped or paused |
| POST | `/api/pause` | running → paused |
| POST | `/api/resume` (`/continue`) | paused → running; stopped → `CONTINUE` |
| POST | `/api/step` | paused: one instruction; stopped: `STEP [count]` (`{"count":n}`) |
| POST | `/api/halt` | running/paused → stopped (`sim>`) |
| POST | `/api/go`, `/api/run` | stopped: `GO`/`RUN [addr]` (`{"addr":..}`). Resumes at the PC; does **not** reboot the OS (use `restart`/`boot`) |
| POST | `/api/boot` | stopped: `{"device":"RQ0"}` |
| POST | `/api/reset` | stopped: `RESET` |
| POST | `/api/restart` | any state: halt if running, then `BOOT` the last boot device (or first attached disk). Reboots the guest |
| POST | `/api/command` | stopped: `{"command":"attach rq0 vms.dsk"}`; `!`/`SPAWN` refused |
| GET | `/ws` | WebSocket; pushes state ~10/s, accepts `{"cmd":"pause"}`, `{"cmd":"boot","device":"RQ0"}`… |

Commands that start a run reply as soon as it is running
(`{"status":"Running"}`); others reply when SCP finishes them
(`{"status":"OK"|<error>,"state":..}`). Command output itself appears on the
simulator console / log.

Numbers: JSON numbers are decimal; strings are `0x…` hex, otherwise in the
radix of the target (octal on a PDP-11).

    curl -X POST localhost:8080/api/pause
    curl 'localhost:8080/api/memory?addr=1000&count=4'
    curl -X PUT localhost:8080/api/registers/R0 -d '{"value":"0x1234"}'

## Security

Loopback by default; cross-origin requests and non-loopback `Host` headers
(DNS rebinding) are refused on loopback binds; optional token via
`Authorization: Bearer` or `?token=`. There is no TLS — put it behind a
reverse proxy or SSH tunnel if you expose it.

## Notes

* `/api/command` is full control of the simulator (attach any file, etc.): use a token, and keep it off non-loopback interfaces unless you trust the network.
* Pausing holds the simulation, but wall-clock time keeps moving; guests such
  as VMS may see a clock jump on resume.
* `web/blinkenlights.html` is embedded as `sim_web_page.h`; after editing it run
  `python3 web/html2h.py` (it also touches `sim_web.c` so `make` rebuilds).
