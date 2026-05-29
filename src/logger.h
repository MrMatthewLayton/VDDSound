/* logger.h - forensic trace logger for trapped port I/O. */
#ifndef VDDSOUND_LOGGER_H
#define VDDSOUND_LOGGER_H

#include "common.h"

void logger_init(void);
void logger_close(void);

/* Log one trapped access. is_in != 0 for IN, 0 for OUT. Captures the guest
 * CS:IP internally via getCS()/getIP(). Cheap: formats into a stack buffer. */
void logger_log(int is_in, WORD port, int size, DWORD value);

/* Write a free-text diagnostic line (no CS:IP). Safe to call from DllMain. */
void logger_note(const char *msg);

/* Write "NOTE <msg>=<value>" (value in decimal and hex). For diagnostics. */
void logger_note_kv(const char *msg, unsigned long value);

#endif /* VDDSOUND_LOGGER_H */
