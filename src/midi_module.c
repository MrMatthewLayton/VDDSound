/*
 * midi_module.c - MPU-401 UART mode reassembled to winmm short/long messages.
 * Runs on the VDM CPU thread. winmm callbacks are CALLBACK_NULL so there is no
 * reentrancy from the MIDI device.
 */
#include "midi_module.h"

#include <mmsystem.h>

static HMIDIOUT hmo;
static int midi_open;

/* Reassembler state. */
static BYTE status;          /* current/running status byte (0 = none) */
static BYTE data[2];
static int  dcount;
static int  dneed;
static int  in_sysex;
static BYTE sysex_buf[2048];
static unsigned sysex_len;

/* ACK bytes the guest reads back from the data port (0x330). */
#define MIDI_INQ_SIZE 8u
#define MIDI_INQ_MASK (MIDI_INQ_SIZE - 1u)
static BYTE inq[MIDI_INQ_SIZE];
static unsigned inq_head, inq_tail;

/* Outstanding SysEx headers, freed at close (avoids freeing before MOM_DONE). */
#define MIDI_HDR_MAX 64
static MIDIHDR *hdrs[MIDI_HDR_MAX];
static int hdr_count;

static void inq_push(BYTE b)
{
    unsigned next = (inq_head + 1u) & MIDI_INQ_MASK;
    if (next == inq_tail) {
        return;
    }
    inq[inq_head] = b;
    inq_head = next;
}

static int inq_empty(void)
{
    return inq_head == inq_tail;
}

static BYTE inq_pop(void)
{
    BYTE b;
    if (inq_empty()) {
        return 0x00;
    }
    b = inq[inq_tail];
    inq_tail = (inq_tail + 1u) & MIDI_INQ_MASK;
    return b;
}

static int need_for(BYTE s)
{
    if (s >= 0x80 && s <= 0xBF) return 2;
    if (s >= 0xC0 && s <= 0xDF) return 1;
    if (s >= 0xE0 && s <= 0xEF) return 2;
    if (s == 0xF1 || s == 0xF3) return 1;
    if (s == 0xF2)              return 2;
    return 0;
}

static void send_short(BYTE s, BYTE d1, BYTE d2)
{
    DWORD msg;
    if (!midi_open) {
        return;
    }
    msg = (DWORD)s | ((DWORD)d1 << 8) | ((DWORD)d2 << 16);
    midiOutShortMsg(hmo, msg);
}

static void flush_sysex(void)
{
    MIDIHDR *h;

    if (!midi_open || sysex_len == 0 || hdr_count >= MIDI_HDR_MAX) {
        return;
    }
    h = (MIDIHDR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                             sizeof(MIDIHDR) + sysex_len);
    if (h == NULL) {
        return;
    }
    h->lpData = (LPSTR)(h + 1);
    CopyMemory(h->lpData, sysex_buf, sysex_len);
    h->dwBufferLength = sysex_len;

    if (midiOutPrepareHeader(hmo, h, sizeof(MIDIHDR)) != MMSYSERR_NOERROR) {
        HeapFree(GetProcessHeap(), 0, h);
        return;
    }
    midiOutLongMsg(hmo, h, sizeof(MIDIHDR));
    hdrs[hdr_count++] = h; /* freed at midi_close, after MOM_DONE is safe */
}

static void feed(BYTE b)
{
    if (b >= 0xF8) {            /* real-time: interleaves, never disturbs state */
        send_short(b, 0, 0);
        return;
    }
    if (b >= 0x80) {           /* status byte */
        if (b == 0xF0) {       /* SysEx start */
            in_sysex = 1;
            sysex_len = 0;
            sysex_buf[sysex_len++] = 0xF0;
            status = 0;
            dcount = 0;
            return;
        }
        if (b == 0xF7) {       /* EOX */
            if (in_sysex) {
                if (sysex_len < sizeof(sysex_buf)) {
                    sysex_buf[sysex_len++] = 0xF7;
                }
                flush_sysex();
                in_sysex = 0;
            }
            return;
        }
        if (in_sysex) {        /* any other status terminates SysEx */
            flush_sysex();
            in_sysex = 0;
        }
        status = b;
        dcount = 0;
        dneed = need_for(b);
        if (b >= 0xF0 && dneed == 0) { /* system common with no data */
            send_short(b, 0, 0);
            status = 0;
        }
        return;
    }
    /* data byte */
    if (in_sysex) {
        if (sysex_len < sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = b;
        }
        return;
    }
    if (status == 0) {
        return;                /* stray data, no running status yet */
    }
    data[dcount++] = b;
    if (dcount >= dneed) {
        send_short(status, data[0], dneed > 1 ? data[1] : 0);
        dcount = 0;
        if (status >= 0xF0) {
            status = 0;        /* system common: no running status */
        }
        /* channel voice: keep `status` for running status */
    }
}

int midi_init(void)
{
    status = 0;
    dcount = 0;
    dneed = 0;
    in_sysex = 0;
    sysex_len = 0;
    inq_head = inq_tail = 0;
    hdr_count = 0;
    midi_open = 0;

    if (midiOutGetNumDevs() == 0) {
        return 1;
    }
    if (midiOutOpen(&hmo, (UINT)-1 /* MIDI_MAPPER */, 0, 0, CALLBACK_NULL)
            != MMSYSERR_NOERROR) {
        return 1;
    }
    midi_open = 1;
    return 0;
}

void midi_close(void)
{
    int i;
    if (!midi_open) {
        return;
    }
    midiOutReset(hmo); /* completes outstanding long messages */
    for (i = 0; i < hdr_count; i++) {
        midiOutUnprepareHeader(hmo, hdrs[i], sizeof(MIDIHDR));
        HeapFree(GetProcessHeap(), 0, hdrs[i]);
    }
    hdr_count = 0;
    midiOutClose(hmo);
    midi_open = 0;
}

VOID WINAPI midi_outb(WORD iport, BYTE data_byte)
{
    if (iport == MPU_DATA) {
        feed(data_byte);
    } else { /* MPU_STATUS (0x331) write = command */
        switch (data_byte) {
        case 0xFF: /* reset */
            status = 0;
            dcount = 0;
            in_sysex = 0;
            inq_push(0xFE);
            break;
        case 0x3F: /* enter UART mode */
            inq_push(0xFE);
            break;
        default:   /* fake-acknowledge unknown commands (VDMSound behaviour) */
            inq_push(0xFE);
            break;
        }
    }
}

VOID WINAPI midi_inb(WORD iport, PBYTE data_out)
{
    if (iport == MPU_STATUS) {
        /* active-low: DRR clear = ready to write; DSR clear = data to read. */
        *data_out = inq_empty() ? (BYTE)MPU_STATUS_READY : (BYTE)0x3F;
    } else { /* MPU_DATA */
        *data_out = inq_pop();
    }
}
