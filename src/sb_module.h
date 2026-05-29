/* sb_module.h - Sound Blaster DSP: detection + digital (DMA) PCM playback. */
#ifndef VDDSOUND_SB_MODULE_H
#define VDDSOUND_SB_MODULE_H

#include "common.h"
#include <stdint.h>

#define SB_PORT_FIRST 0x220
#define SB_PORT_LAST  0x22F
#define SB_BASE       0x220

/* Advertised DSP version (SB16 v4.05). */
#define SB_DSP_MAJOR 0x04
#define SB_DSP_MINOR 0x05

/* hVdd is needed for VDDRequestDMA; pass it in at init (called from DllMain). */
void sb_init(HANDLE hVdd);

/* I/O hook callbacks (run on the VDM CPU thread). */
VOID WINAPI sb_outb(WORD iport, BYTE data);
VOID WINAPI sb_inb(WORD iport, PBYTE data);

/* Render-thread mixer step: if a digital transfer is active, pull guest PCM via
 * DMA, resample to the output rate, and mix it into `buf` (interleaved stereo
 * int16, `frames` frames), posting the completion IRQ at block boundaries.
 * No-op when nothing is playing. Called by the audio render thread after the FM
 * block has been rendered into `buf`. */
void sb_mix(int16_t *buf, unsigned frames);

#endif /* VDDSOUND_SB_MODULE_H */
