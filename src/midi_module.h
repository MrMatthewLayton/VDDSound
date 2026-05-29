/* midi_module.h - MPU-401 UART pass-through to winmm. */
#ifndef VDDSOUND_MIDI_MODULE_H
#define VDDSOUND_MIDI_MODULE_H

#include "common.h"

#define MPU_PORT_FIRST 0x330
#define MPU_PORT_LAST  0x331
#define MPU_DATA       0x330
#define MPU_STATUS     0x331

/* Status byte: both bits active-low. 0xBF = ready to accept a write, nothing
 * to read (DSR set/no-data, DRR clear/ready). */
#define MPU_STATUS_READY 0xBF

int  midi_init(void);   /* returns 0 on success */
void midi_close(void);

VOID WINAPI midi_outb(WORD iport, BYTE data);
VOID WINAPI midi_inb(WORD iport, PBYTE data);

#endif /* VDDSOUND_MIDI_MODULE_H */
