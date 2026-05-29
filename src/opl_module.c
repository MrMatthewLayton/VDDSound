/*
 * opl_module.c - traps OPL register writes and synthesises FM via Nuked OPL3.
 *
 * Threading: opl_outb/opl_inb run on the VDM CPU thread; opl_render runs on
 * the audio thread. They communicate through a lock-free single-producer/
 * single-consumer ring of packed register writes. The Nuked chip is only ever
 * touched by the render thread. The AdLib timer-status used for detection is
 * only ever touched by the VDM thread (both writes and status reads), so it
 * needs no synchronisation.
 */
#include "opl_module.h"
#include "opl3.h"

#define OPL_QUEUE_SIZE 8192u           /* power of two */
#define OPL_QUEUE_MASK (OPL_QUEUE_SIZE - 1u)

static opl3_chip chip;
static int opl_active = 0;

/* SPSC ring of packed writes: (reg << 8) | value, reg up to 0x1FF. */
static volatile uint32_t opl_queue[OPL_QUEUE_SIZE];
static volatile unsigned opl_head; /* written by producer (VDM thread) */
static volatile unsigned opl_tail; /* written by consumer (render thread) */

/* AdLib detection state (VDM thread only). */
static BYTE opl_addr0;   /* selected register, bank 0 (port 0x388 write) */
static BYTE opl_addr1;   /* selected register, bank 1 (port 0x38A write) */
static BYTE opl_status;  /* value returned by reads of 0x388 */

static void opl_enqueue(uint16_t reg, uint8_t val)
{
    unsigned head = __atomic_load_n(&opl_head, __ATOMIC_RELAXED);
    unsigned next = (head + 1u) & OPL_QUEUE_MASK;
    unsigned tail = __atomic_load_n(&opl_tail, __ATOMIC_ACQUIRE);

    if (next == tail) {
        return; /* full: drop (only happens under pathological write floods) */
    }
    opl_queue[head] = ((uint32_t)reg << 8) | val;
    __atomic_store_n(&opl_head, next, __ATOMIC_RELEASE);
}

/* Apply detection side effects for bank-0 timer registers, then queue. */
static void opl_write_reg(uint16_t reg, uint8_t val)
{
    if (reg == 0x04) {
        if (val & 0x80) {
            opl_status = 0x00;        /* IRQ reset clears timer flags */
        } else {
            BYTE st = 0;
            if (val & 0x01) st |= 0xC0; /* Timer1 started -> IRQ + T1 expired */
            if (val & 0x02) st |= 0xA0; /* Timer2 started -> IRQ + T2 expired */
            opl_status = st;
        }
    }
    opl_enqueue(reg, val);
}

void opl_init(unsigned sample_rate)
{
    OPL3_Reset(&chip, sample_rate);
    opl_head = 0;
    opl_tail = 0;
    opl_addr0 = 0;
    opl_addr1 = 0;
    opl_status = 0;
    opl_active = 1;
}

void opl_shutdown(void)
{
    opl_active = 0;
}

VOID WINAPI opl_outb(WORD iport, BYTE data)
{
    switch (iport) {
    case 0x388: opl_addr0 = data; break;                 /* address (bank 0) */
    case 0x389: opl_write_reg(opl_addr0, data); break;   /* data    (bank 0) */
    case 0x38A: opl_addr1 = data; break;                 /* address (bank 1) */
    case 0x38B: opl_write_reg((uint16_t)(0x100 | opl_addr1), data); break;
    default: break;
    }
}

VOID WINAPI opl_inb(WORD iport, PBYTE data)
{
    /* Status read is on the address port (0x388). Other reads are undefined. */
    *data = (iport == 0x388) ? opl_status : (BYTE)0xFF;
}

void opl_render(int16_t *buf, unsigned frames)
{
    unsigned head, tail;

    if (!opl_active) {
        unsigned i;
        for (i = 0; i < frames * 2u; i++) {
            buf[i] = 0;
        }
        return;
    }

    /* Drain pending register writes into Nuked's own timestamped write buffer.
     * OPL3_WriteRegBuffered schedules each write OPL_WRITEBUF_DELAY (2) samples
     * after the previous, and OPL3_GenerateStream applies them at those sample
     * offsets as it renders. So a burst of writes is spread across the block at
     * hardware-accurate spacing and takes effect mid-block, instead of
     * OPL3_WriteReg collapsing the whole burst onto the block's first sample
     * (the old block-granularity smear, worst on render-thread catch-up). */
    tail = __atomic_load_n(&opl_tail, __ATOMIC_RELAXED);
    head = __atomic_load_n(&opl_head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        uint32_t packed = opl_queue[tail];
        OPL3_WriteRegBuffered(&chip, (uint16_t)(packed >> 8), (uint8_t)(packed & 0xFF));
        tail = (tail + 1u) & OPL_QUEUE_MASK;
    }
    __atomic_store_n(&opl_tail, tail, __ATOMIC_RELEASE);

    OPL3_GenerateStream(&chip, buf, frames);
}
