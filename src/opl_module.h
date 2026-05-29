/* opl_module.h - OPL2/OPL3 FM emulation (Nuked OPL3) + AdLib detection. */
#ifndef VDDSOUND_OPL_MODULE_H
#define VDDSOUND_OPL_MODULE_H

#include "common.h"
#include <stdint.h>

/* OPL/AdLib port range we hook: 0x388-0x38B (bank0 addr/data + OPL3 bank1). */
#define OPL_PORT_FIRST 0x388
#define OPL_PORT_LAST  0x38B

void opl_init(unsigned sample_rate);
void opl_shutdown(void);

/* I/O hook callbacks (run on the VDM CPU thread). */
VOID WINAPI opl_outb(WORD iport, BYTE data);
VOID WINAPI opl_inb(WORD iport, PBYTE data);

/* Pull rendered FM PCM (interleaved stereo). Called by the audio render
 * thread; drains queued register writes then synthesises `frames` frames. */
void opl_render(int16_t *buf, unsigned frames);

#endif /* VDDSOUND_OPL_MODULE_H */
