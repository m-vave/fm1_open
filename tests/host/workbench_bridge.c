/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Device side of the project-library protocol over stdin/stdout, for
 * workbench_test.mjs: SysEx frames in, replies out, on the open engine with a
 * fresh simulated Flash. Not a test by itself (see workbench_test.mjs).
 *
 * transfer.c is #included with its external symbols renamed so replies reach
 * stdout (the simulator's fm1_midi_send discards them). */
#include "internal.h"
#define fm1_app_sysex t_app_sysex
#define groove_transfer_init t_transfer_init
#define groove_transfer_poll t_transfer_poll
#define fm1_midi_send t_midi_send
int t_midi_send(const uint8_t *data, size_t bytes);
#include "../../app/transfer.c"
#include <stdio.h>

void sim_boot(unsigned clear);
const float *sim_render(void);

int t_midi_send(const uint8_t *data, size_t bytes)
{
    if (!data || !bytes || bytes > 128) return -1;
    fwrite(data, 1, bytes, stdout);
    fflush(stdout);
    return 0;
}
int main(void)
{
    static uint8_t frame[192];
    unsigned n = 0;
    int c;
    sim_boot(3);
    t_transfer_init();
    while ((c = getchar()) != EOF) {
        if (c == 0xf0) n = 0;
        if (n < sizeof frame) frame[n++] = (uint8_t)c;
        if (c != 0xf7) continue;
        t_app_sysex(frame, n);
        for (unsigned i = 0; i < 4; ++i) t_transfer_poll();
        n = 0;
    }
    return 0;
}
