/* sim_web.h: optional embedded web server, WebSocket and REST API

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

   This feature is compiled only when USE_WEBSERVER is defined at build time
   (for the makefile: "make WEBSERVER=1 pdp11 vax").  Without it, this header
   and sim_web.c contribute nothing to the simulator.
*/

#ifndef SIM_WEB_H_
#define SIM_WEB_H_    0

#ifdef USE_WEBSERVER

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the internal WEB device with SCP.  Called once from main(). */
void sim_web_init (void);

/* Used by the sim> command loop instead of a blocking stdin read; keeps the
   web server serviced while the simulator is stopped.  See sim_web.c. */
int sim_web_prompt_wait (const char *prompt, char *cbuf, size_t size);

/* Guest console hooks (called from sim_console.c): output is copied to the
   web terminal; sim_web_console_in returns the next keystroke typed in the
   web terminal, or -1. */
/* Called with every command line the sim> loop executes (to learn the boot device). */
void sim_web_note_command (const char *line);

void sim_web_console_out (int c);
int sim_web_console_in (void);

#ifdef __cplusplus
}
#endif

#endif /* USE_WEBSERVER */

#endif
