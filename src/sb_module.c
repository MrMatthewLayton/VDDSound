/*
 * sb_module.c - minimal Sound Blaster DSP so autodetecting games see a card,
 * and so we own the SB port range (which makes NTVDM's built-in VSB back off).
 * MVP: reset handshake (0xAA) and the DSP version query (0xE1). No PCM/DMA.
 * All handlers run on the VDM CPU thread; state is single-threaded here.
 */
#include "sb_module.h"

#define SB_FIFO_SIZE 16u
#define SB_FIFO_MASK (SB_FIFO_SIZE - 1u)

static BYTE  sb_fifo[SB_FIFO_SIZE]; /* DSP -> host read-data bytes */
static unsigned sb_fifo_head;
static unsigned sb_fifo_tail;
static int   sb_reset_pending;

static void sb_fifo_clear(void)
{
    sb_fifo_head = sb_fifo_tail = 0;
}

static void sb_fifo_push(BYTE b)
{
    unsigned next = (sb_fifo_head + 1u) & SB_FIFO_MASK;
    if (next == sb_fifo_tail) {
        return; /* full */
    }
    sb_fifo[sb_fifo_head] = b;
    sb_fifo_head = next;
}

static int sb_fifo_empty(void)
{
    return sb_fifo_head == sb_fifo_tail;
}

static BYTE sb_fifo_pop(void)
{
    BYTE b;
    if (sb_fifo_empty()) {
        return 0xFF;
    }
    b = sb_fifo[sb_fifo_tail];
    sb_fifo_tail = (sb_fifo_tail + 1u) & SB_FIFO_MASK;
    return b;
}

void sb_init(void)
{
    sb_fifo_clear();
    sb_reset_pending = 0;
}

VOID WINAPI sb_outb(WORD iport, BYTE data)
{
    switch (iport - SB_BASE) {
    case 0x06: /* 0x226 DSP reset: 1 then 0 triggers the 0xAA ready byte */
        if (data & 0x01) {
            sb_reset_pending = 1;
        } else if (sb_reset_pending) {
            sb_reset_pending = 0;
            sb_fifo_clear();
            sb_fifo_push(0xAA);
        }
        break;
    case 0x0C: /* 0x22C DSP write command/data */
        if (data == 0xE1) { /* DSP version query */
            sb_fifo_push(SB_DSP_MAJOR);
            sb_fifo_push(SB_DSP_MINOR);
        }
        /* Other commands are accepted and ignored in the MVP. */
        break;
    default:
        break;
    }
}

VOID WINAPI sb_inb(WORD iport, PBYTE data)
{
    switch (iport - SB_BASE) {
    case 0x0A: /* 0x22A DSP read data */
        *data = sb_fifo_pop();
        break;
    case 0x0C: /* 0x22C write-buffer status: bit7 set = busy. Always ready. */
        *data = 0x7F;
        break;
    case 0x0E: /* 0x22E read-buffer status: bit7 set = data available */
        *data = sb_fifo_empty() ? (BYTE)0x7F : (BYTE)0xFF;
        break;
    default:
        *data = 0xFF;
        break;
    }
}
