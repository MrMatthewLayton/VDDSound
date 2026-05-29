/* sb_module.h - Sound Blaster DSP detection stub (MVP: own the ports + detect). */
#ifndef VDDSOUND_SB_MODULE_H
#define VDDSOUND_SB_MODULE_H

#include "common.h"

#define SB_PORT_FIRST 0x220
#define SB_PORT_LAST  0x22F
#define SB_BASE       0x220

/* Advertised DSP version (SB16 v4.05). */
#define SB_DSP_MAJOR 0x04
#define SB_DSP_MINOR 0x05

void sb_init(void);

VOID WINAPI sb_outb(WORD iport, BYTE data);
VOID WINAPI sb_inb(WORD iport, PBYTE data);

#endif /* VDDSOUND_SB_MODULE_H */
