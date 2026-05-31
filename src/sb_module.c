/*
 * sb_module.c - Sound Blaster DSP: detection + digital (DMA) PCM playback.
 *
 * Detection (reset->0xAA, DSP version 0xE1->4.05) plus owning the port range
 * makes autodetecting games see an SB16 and makes NTVDM's built-in VSB back off.
 *
 * Digital path (FIRST PASS - needs on-VM tuning, esp. IRQ pacing):
 *   - A DSP command FSM (VDM thread) parses the multi-byte DMA commands: classic
 *     8-bit (0x14/0x1C/0x90/0x91 + 0x40 time-constant + 0x48 block size) and
 *     SB16 (0xB0-0xCF + mode byte + 0x41 sample rate), plus speaker/pause/
 *     resume/exit-auto-init. It fills a small playback control block.
 *   - sb_mix() (audio render thread) pulls guest PCM via VDDRequestDMA, decodes
 *     8/16-bit mono/stereo, linear-resamples to the output rate, and mixes into
 *     the FM buffer. Pacing DMA consumption off the audio clock makes the block
 *     completion IRQ (call_ica_hw_interrupt, IRQ5) land at about the right time;
 *     auto-init reloads, single-cycle stops.
 *
 * Threading: sb_outb/sb_inb run on the VDM CPU thread; sb_mix runs on the audio
 * render thread. The only state shared across the two threads is the digital
 * playback control block, guarded by sb_cs (held briefly, including across the
 * VDDRequestDMA pull, which only copies a few KB). IRQ-pending flags are atomic.
 * The detection FIFO, mixer registers and DSP-FSM scratch are VDM-thread-only.
 *
 * KNOWN FIRST-PASS ASSUMPTIONS to verify on the VM (search "TUNE"):
 *   - DMA channels hard-wired to 1 (8-bit) / 5 (16-bit) per the SB16 default.
 *   - SB16 command length treated as (samples-1) per channel; block_bytes =
 *     samples * (bits/8) * channels. Some drivers count differently.
 *   - IRQ posted from the render thread, which runs AHEAD of the play cursor by
 *     the DirectSound buffer latency; on a big catch-up burst this can post a
 *     run of IRQs. If that floods a guest ISR, pace off the play cursor instead.
 *   - call_ica_hw_interrupt(0, 5, 1) for IRQ5 on the master 8259.
 */
#include "sb_module.h"
#include "audio_out.h"   /* AUDIO_SAMPLE_RATE */
#include "logger.h"

#define SB_FIFO_SIZE 16u
#define SB_FIFO_MASK (SB_FIFO_SIZE - 1u)

#define SB_DMA8   1u     /* TUNE: 8-bit DMA channel (BLASTER D1) */
#define SB_DMA16  5u     /* TUNE: 16-bit DMA channel (BLASTER H5) */
#define SB_IRQ    5      /* TUNE: BLASTER I5 */

#define SB_STAGE_RAW_BYTES 2048u   /* per-pull DMA staging (whole frames) */

/* DIAG(b46): when 1, ignore the guest buffer and feed a pure tone instead, to
 * test whether the DirectSound PCM OUTPUT path (8-bit, odd rate, resampled,
 * looping) is itself clean. RESULT: tone is clean in both games => the output
 * path is fine; the garbage is the guest data / our read. Now 0. */
#define SB_TONE_TEST 0

/* DIAG(b47): capture the exact bytes we feed to DirectSound (post-convert) to
 * C:\vddsound\dump.wav so it can be played back to SEE what we're reading -
 * noise = wrong memory, glitchy/repeating = tearing, wrong speed = stride. */
#define SB_WAV_DUMP 0

/* ---- detection FIFO (VDM thread only) ----------------------------------- */
static BYTE     sb_fifo[SB_FIFO_SIZE];
static unsigned sb_fifo_head, sb_fifo_tail;
static int      sb_reset_pending;

/* ---- mixer chip registers (VDM thread only) ----------------------------- */
static BYTE mixer_regs[256];
static BYTE mixer_addr;

/* ---- DSP command FSM scratch (VDM thread only) -------------------------- */
static BYTE     dsp_cmd;
static BYTE     dsp_args[3];   /* SB16 DMA commands take 3: mode + len lo/hi */
static int      dsp_argc, dsp_argn;
static unsigned dsp_rate = 11025;      /* last programmed sample rate (Hz) */
static unsigned dsp_block_bytes = 1;   /* last 0x48 block size (bytes) */

/* ---- shared digital playback control block (guard with sb_cs) ----------- */
static HANDLE           sb_hvdd;
static CRITICAL_SECTION sb_cs;
static volatile LONG    sb_cs_ready;

static volatile int sb_play;       /* a transfer is active */
static int      sb_paused;         /* DMA paused (0xD0/0xD5) */
static int      sb_speaker;        /* speaker on (0xD1) - gates audible output */
static unsigned sb_rate;           /* active playback rate (Hz) */
static int      sb_bits;           /* 8 or 16 */
static int      sb_stereo;         /* 0/1 */
static int      sb_signed;         /* sample encoding signed? */
static int      sb_autoinit;       /* 0/1 */
static unsigned sb_channel;        /* DMA channel in use */
static unsigned sb_block_bytes;    /* DSP-command block length (cross-check) */
/* Guest DMA buffer mapped directly: VDDRequestDMA returns 0 on this NTVDM, so
 * we read PCM straight from guest memory via VDDQueryDMA + MGetVdmPointer. */
static const BYTE *sb_dma_ptr;     /* host pointer to the guest DMA buffer */
static ULONG       sb_dma_lin;     /* guest linear addr of the DMA buffer (re-map each read) */
static unsigned    sb_dma_len;     /* buffer length in bytes */
static unsigned    sb_dma_pos;     /* play cursor within the buffer */
static unsigned    sb_gate_wait;   /* render ticks spent waiting for a block-IRQ ack */
static unsigned    sb_byterate;    /* programmed playback bytes/sec (rate*frame) */
static unsigned    sb_xfer_total;  /* bytes transferred this transfer (IRQ accounting) */
static unsigned    sb_load_fp;     /* DS playback load feedback, 8.8 fp (256 = on-target) */
static unsigned long long sb_qpc_freq;   /* QueryPerformanceFrequency (ticks/sec) */
static LARGE_INTEGER      sb_last_qpc;    /* QPC at the last metering tick */
static unsigned    sb_base_addr;   /* 8237 base address register at transfer start */
static unsigned    sb_base_count;  /* 8237 base count (transfers-1) at transfer start */
static unsigned    sb_base_page;   /* 8237 page register at transfer start */

/* ---- IRQ flags (atomic; render sets, VDM acks) -------------------------- */
static volatile LONG sb_irq8_pending;
static volatile LONG sb_irq16_pending;
static volatile LONG sb_irq_posts;   /* DIAG(b38): IRQs we raised this transfer */
static volatile LONG sb_irq_acks;    /* DIAG(b38): guest reads of 0x22E (8-bit ack) */

/* ---- resampler + decode staging (render thread only) -------------------- */
static BYTE     sb_raw[SB_STAGE_RAW_BYTES];
static BYTE     sb_snapshot[131072];   /* private copy of the guest DMA buffer */
static int16_t  sb_stage[SB_STAGE_RAW_BYTES * 2]; /* decoded stereo frames */
static unsigned sb_stage_head, sb_stage_pos;
static int      sb_have_cur;
static int      sb_p0l, sb_p0r, sb_p1l, sb_p1r, sb_p2l, sb_p2r, sb_p3l, sb_p3r;
static unsigned sb_phase;          /* 16.16 fractional position p1->p2 */
static int      sb_diag_pull_logged;   /* DIAG(b20): one-shot render-pull trace */

/* ---- health (render thread only) ---------------------------------------- */
static unsigned sb_diag_tick_acc;    /* playing ticks since last health log */

#if SB_WAV_DUMP
static BYTE     sb_dump[262144];     /* captured fed bytes -> dump.wav */
static unsigned sb_dump_pos, sb_dump_rate, sb_dump_bits, sb_dump_ch;
static int      sb_dump_done;
#endif

/* ------------------------------------------------------------------------- */

static void sb_fifo_clear(void) { sb_fifo_head = sb_fifo_tail = 0; }

static void sb_fifo_push(BYTE b)
{
    unsigned next = (sb_fifo_head + 1u) & SB_FIFO_MASK;
    if (next == sb_fifo_tail) return; /* full */
    sb_fifo[sb_fifo_head] = b;
    sb_fifo_head = next;
}

static int  sb_fifo_empty(void) { return sb_fifo_head == sb_fifo_tail; }

static BYTE sb_fifo_pop(void)
{
    BYTE b;
    if (sb_fifo_empty()) return 0xFF;
    b = sb_fifo[sb_fifo_tail];
    sb_fifo_tail = (sb_fifo_tail + 1u) & SB_FIFO_MASK;
    return b;
}

void sb_init(HANDLE hVdd)
{
    sb_hvdd = hVdd;
    sb_fifo_clear();
    sb_reset_pending = 0;
    dsp_argc = dsp_argn = 0;
    sb_play = 0;
    sb_speaker = 1;           /* SB16: speaker is always on (0xD1/0xD3 are no-ops) */
    mixer_regs[0x80] = 0x02;  /* IRQ select: IRQ5 */
    mixer_regs[0x81] = 0x22;  /* DMA select: DMA1 (8-bit) + DMA5 (16-bit) */
    {
        LARGE_INTEGER f;
        sb_qpc_freq = (QueryPerformanceFrequency(&f) && f.QuadPart)
                      ? (unsigned long long)f.QuadPart : 1ull;
    }
    InitializeCriticalSection(&sb_cs);
    InterlockedExchange(&sb_cs_ready, 1);
}

/* ---- digital playback start/stop (called under sb_cs) ------------------- */

static void sb_post_irq(void)
{
    if (sb_bits == 16) InterlockedExchange(&sb_irq16_pending, 1);
    else               InterlockedExchange(&sb_irq8_pending, 1);
    call_ica_hw_interrupt(0, SB_IRQ, 1);
    InterlockedIncrement(&sb_irq_posts);   /* DIAG(b38) */
}

/* SB 8-bit PCM signedness is not conveyed by the classic DMA commands and games
 * disagree (Skyroads sends signed data via the spec-unsigned 0x14). Detect it
 * from the buffer: unsigned silence is 0x80, signed silence is 0x00 - whichever
 * dominates a sample of the data wins, defaulting to unsigned (the SB spec)
 * when neither is clearly present. */
static int sb_detect_signed(const BYTE *p, unsigned len)
{
    unsigned i, n = (len < 2048u) ? len : 2048u, zeros = 0, mids = 0;
    if (!p) return 0;
    for (i = 0; i < n; i++) {
        if (p[i] == 0x00)      zeros++;
        else if (p[i] == 0x80) mids++;
    }
    return (zeros > mids) ? 1 : 0;
}

static void sb_start(int bits, int stereo, int sgnd, unsigned channel,
                     int autoinit, unsigned block_bytes)
{
    sb_bits        = bits;
    sb_stereo      = stereo;
    sb_signed      = sgnd;
    sb_channel     = channel;
    sb_autoinit    = autoinit;
    sb_block_bytes = block_bytes ? block_bytes : 1u;
    sb_rate        = dsp_rate ? dsp_rate : 11025u;
    sb_paused      = 0;
    /* reset the resampler/staging so we don't reuse the previous stream. */
    sb_have_cur    = 0;
    sb_phase       = 0;
    sb_stage_head  = sb_stage_pos = 0;
    sb_play        = 1;
    sb_gate_wait   = 0;
    /* metering state: byte-rate = rate * frame; reset transfer + load + clock. */
    sb_byterate    = sb_rate * ((unsigned)(bits / 8) * (stereo ? 2u : 1u));
    sb_xfer_total  = 0;
    sb_load_fp     = 256u;     /* assume on-target until the sink reports back */
    QueryPerformanceCounter(&sb_last_qpc);
    InterlockedExchange(&sb_irq_posts, 0);   /* DIAG(b38) */
    InterlockedExchange(&sb_irq_acks, 0);
    sb_diag_pull_logged = 0;
    sb_diag_tick_acc  = 0;
    logger_note_kv("sb: dma start, rate", (unsigned long)sb_rate);
    logger_note_kv("sb: dma start, bytes", (unsigned long)sb_block_bytes);
    logger_note_kv("sb: start bits", (unsigned long)sb_bits);
    logger_note_kv("sb: start stereo", (unsigned long)sb_stereo);
    logger_note_kv("sb: start signed", (unsigned long)sb_signed);
    logger_note_kv("sb: start chan", (unsigned long)sb_channel);
    logger_note_kv("sb: start autoinit", (unsigned long)sb_autoinit);
    /* Map the guest DMA buffer directly. VDDRequestDMA returns 0 on this NTVDM,
     * but VDDQueryDMA reports the channel state and MGetVdmPointer maps the
     * buffer, so we read PCM straight from guest memory (the VDMSound way). */
    {
        WORD  info[16];
        ULONG lin, cnt;
        ZeroMemory(info, sizeof(info));
        VDDQueryDMA(sb_hvdd, sb_channel, info);
        logger_note_kv("sb: q addr", (unsigned long)info[0]);
        logger_note_kv("sb: q count", (unsigned long)info[1]);
        logger_note_kv("sb: q page", (unsigned long)info[2]);
        /* Latch the 8237 base regs; we advance current addr/count from these
         * each tick via VDDSetDMA so a polling guest (DOOM/DMX) tracks the play
         * cursor. [b42-setdma] */
        sb_base_addr  = info[0];
        sb_base_count = info[1];
        sb_base_page  = info[2];
        /* VDD_DMA_INFO {addr, count, page, ...}; phys = (page<<16)|addr. 8-bit
         * DMA is byte-granular; 16-bit DMA (ch >= 4) is word-granular. TUNE. */
        if (sb_channel >= 4u) {
            lin = ((ULONG)(info[2] & 0xFFu) << 16) | ((ULONG)info[0] << 1);
            cnt = ((ULONG)info[1] + 1u) * 2u;
        } else {
            lin = ((ULONG)(info[2] & 0xFFu) << 16) | info[0];
            cnt = (ULONG)info[1] + 1u;
        }
        if (cnt > sizeof(sb_snapshot)) cnt = sizeof(sb_snapshot);   /* cap */
        sb_dma_len = cnt;
        sb_dma_pos = 0;
        /* LIVE pointer: read the guest buffer as it streams (do NOT snapshot -
         * the guest fills it just-in-time; a command-time snapshot is empty). */
        sb_dma_lin = lin;
        sb_dma_ptr = (cnt > 1u) ? (const BYTE *)MGetVdmPointer(lin, cnt, FALSE)
                                : NULL;
        logger_note_kv("sb: dma lin", (unsigned long)lin);
        logger_note_kv("sb: dma len", (unsigned long)cnt);
        logger_note_kv("sb: dma ptr ok", (unsigned long)(sb_dma_ptr != NULL));
    }
    if (sb_bits == 8 && sb_dma_ptr) {   /* content-detect signed vs unsigned */
        sb_signed = sb_detect_signed(sb_dma_ptr, sb_dma_len);
        logger_note_kv("sb: detected signed", (unsigned long)sb_signed);
    }
    if (!sb_dma_ptr) {
        sb_play = 0;
        logger_note("sb: no DMA pointer - not playing");
    } else if (audio_pcm_open(sb_rate, sb_bits, sb_stereo ? 2 : 1) != 0) {
        sb_play = 0;
        logger_note("sb: audio_pcm_open failed");
    }
}

static void sb_stop(void)
{
    sb_play    = 0;
    sb_paused  = 0;
    sb_dma_ptr = NULL;
}

/* ---- DSP command FSM (VDM thread, under sb_cs via sb_outb) --------------- */

static int dsp_arg_count(BYTE c)
{
    if (c >= 0xB0 && c <= 0xCF) return 3;   /* SB16 DMA: mode + len lo/hi */
    switch (c) {
    case 0x14: case 0x91:                   /* 8-bit single-cycle: len lo/hi */
    case 0x41: case 0x42:                   /* sample rate: hi/lo */
    case 0x48:                              /* block size: lo/hi */
    case 0x80:                              /* silence: len lo/hi (ignored) */
        return 2;
    case 0x40:                              /* time constant */
    case 0x10:                              /* direct DAC (ignored) */
        return 1;
    default:
        return 0;
    }
}

static void dsp_execute(BYTE cmd, const BYTE *a)
{
    /* DIAG(b38): log the raw command + arg bytes for every DMA-start command so
     * we can see EXACTLY what the guest programmed - the SB16 mode byte (arg0,
     * bit5=stereo bit4=signed) and the SB Pro mixer-0x0E stereo bit. DOOM's
     * chk-a-chk-a is most likely a stereo misdetect: decoding mono as stereo
     * makes us read the guest buffer at 2x its fill rate (perpetual tear). */
    if (cmd == 0x14 || cmd == 0x91 || cmd == 0x1C || cmd == 0x90 ||
        (cmd >= 0xB0 && cmd <= 0xCF)) {
        logger_note_kv("sb: DSP cmd", (unsigned long)cmd);
        logger_note_kv("sb:   arg0 (SB16 mode)", (unsigned long)a[0]);
        logger_note_kv("sb:   arg1", (unsigned long)a[1]);
        logger_note_kv("sb:   arg2", (unsigned long)a[2]);
        logger_note_kv("sb:   mixer 0x0E", (unsigned long)mixer_regs[0x0E]);
    }
    switch (cmd) {
    case 0xE1:                              /* DSP version */
        sb_fifo_push(SB_DSP_MAJOR);
        sb_fifo_push(SB_DSP_MINOR);
        break;
    case 0x40: {                            /* set time constant */
        unsigned tc = a[0];
        if (tc < 256u) dsp_rate = 1000000u / (256u - tc);
        break;
    }
    case 0x41: case 0x42:                   /* set sample rate (big-endian) */
        dsp_rate = ((unsigned)a[0] << 8) | a[1];
        break;
    case 0x48:                              /* set DMA block size (bytes-1) */
        dsp_block_bytes = (((unsigned)a[1] << 8) | a[0]) + 1u;
        break;
    case 0x14: case 0x91: {                 /* 8-bit single-cycle output */
        unsigned len = (((unsigned)a[1] << 8) | a[0]) + 1u;
        if (len < 32u) {   /* tiny transfer = SB detection blip (VDMSound quick-
                            * DMA): ack it so detection completes, but don't churn
                            * the audio buffer playing garbage. */
            InterlockedExchange(&sb_irq8_pending, 1);
            call_ica_hw_interrupt(0, SB_IRQ, 1);
            break;
        }
        sb_start(8, (mixer_regs[0x0E] & 0x02) ? 1 : 0, 0, SB_DMA8, 0, len);
        break;
    }
    case 0x1C: case 0x90:                   /* 8-bit auto-init output */
        sb_start(8, (mixer_regs[0x0E] & 0x02) ? 1 : 0, 0, SB_DMA8, 1,
                 dsp_block_bytes);
        break;
    case 0xD0: case 0xD5: sb_paused = 1; break;  /* pause 8/16-bit DMA */
    case 0xD4: case 0xD6: sb_paused = 0; break;  /* resume 8/16-bit DMA */
    case 0xD9: case 0xDA: sb_autoinit = 0; break;/* exit auto-init after block */
    case 0xD1: sb_speaker = 1; break;            /* speaker on */
    case 0xD3: sb_speaker = 0; break;            /* speaker off */
    default:
        if (cmd >= 0xB0 && cmd <= 0xCF) {        /* SB16 DMA */
            unsigned mode = a[0];
            unsigned len  = (((unsigned)a[2] << 8) | a[1]) + 1u; /* samples */
            int bits, stereo, sgnd, autoinit;
            unsigned ch, frame, bytes;
            if (cmd & 0x08) break;               /* bit3 set = A/D input: skip */
            bits     = (cmd < 0xC0) ? 16 : 8;    /* 0xBx=16-bit, 0xCx=8-bit */
            autoinit = (cmd & 0x04) ? 1 : 0;
            stereo   = (mode & 0x20) ? 1 : 0;
            sgnd     = (mode & 0x10) ? 1 : 0;
            ch       = (bits == 16) ? SB_DMA16 : SB_DMA8;
            frame    = (unsigned)(bits / 8) * (stereo ? 2u : 1u);
            bytes    = len * frame;              /* TUNE: length convention */
            if (!autoinit && bytes < 32u) {      /* detection blip (quick-DMA) */
                InterlockedExchange(bits == 16 ? &sb_irq16_pending
                                               : &sb_irq8_pending, 1);
                call_ica_hw_interrupt(0, SB_IRQ, 1);
                break;
            }
            sb_start(bits, stereo, sgnd, ch, autoinit, bytes);
        }
        /* other commands: accepted and ignored. */
        break;
    }
}

static void dsp_write(BYTE b)
{
    if (dsp_argn > 0) {
        dsp_args[dsp_argc++] = b;
        if (dsp_argc >= dsp_argn) {
            dsp_argn = 0;
            dsp_execute(dsp_cmd, dsp_args);
        }
        return;
    }
    dsp_cmd  = b;
    dsp_argc = 0;
    dsp_argn = dsp_arg_count(b);
    if (dsp_argn == 0) dsp_execute(b, dsp_args);
}

/* ------------------------------------------------------------------------- */

VOID WINAPI sb_outb(WORD iport, BYTE data)
{
    if (InterlockedCompareExchange(&sb_cs_ready, 1, 1)) EnterCriticalSection(&sb_cs);

    switch (iport - SB_BASE) {
    case 0x04: /* 0x224 mixer address latch */
        mixer_addr = data;
        break;
    case 0x05: /* 0x225 mixer data */
        mixer_regs[mixer_addr] = data;
        break;
    case 0x06: /* 0x226 DSP reset: 1 then 0 readies the card and stops audio */
        if (data & 0x01) {
            sb_reset_pending = 1;
        } else if (sb_reset_pending) {
            sb_reset_pending = 0;
            sb_fifo_clear();
            sb_fifo_push(0xAA);
            sb_stop();
            sb_speaker = 1;
            dsp_argc = dsp_argn = 0;
            InterlockedExchange(&sb_irq8_pending, 0);
            InterlockedExchange(&sb_irq16_pending, 0);
        }
        break;
    case 0x0C: /* 0x22C DSP write command/data */
        dsp_write(data);
        break;
    default:
        break;
    }

    if (InterlockedCompareExchange(&sb_cs_ready, 1, 1)) LeaveCriticalSection(&sb_cs);
}

VOID WINAPI sb_inb(WORD iport, PBYTE data)
{
    switch (iport - SB_BASE) {
    case 0x05: /* 0x225 mixer data read */
        switch (mixer_addr) {
        case 0x80: *data = 0x02; break;                 /* IRQ select: IRQ5 */
        case 0x81: *data = 0x22; break;                 /* DMA: DMA1 + DMA5 */
        case 0x82: {                                    /* IRQ status */
            BYTE s = 0;
            if (sb_irq8_pending)  s |= 0x01;
            if (sb_irq16_pending) s |= 0x02;
            *data = s;
            break;
        }
        default: *data = mixer_regs[mixer_addr]; break;
        }
        break;
    case 0x0A: /* 0x22A DSP read data */
        *data = sb_fifo_pop();
        break;
    case 0x0C: /* 0x22C write-buffer status: bit7 = busy. Always ready. */
        *data = 0x7F;
        break;
    case 0x0E: /* 0x22E read-buffer status (bit7 = data avail) + 8-bit IRQ ack */
        InterlockedExchange(&sb_irq8_pending, 0);
        InterlockedIncrement(&sb_irq_acks);   /* DIAG(b38) */
        *data = sb_fifo_empty() ? (BYTE)0x7F : (BYTE)0xFF;
        break;
    case 0x0F: /* 0x22F 16-bit IRQ acknowledge */
        InterlockedExchange(&sb_irq16_pending, 0);
        *data = 0xFF;
        break;
    default:
        *data = 0xFF;
        break;
    }
}

/* ---- render thread: pull DMA, resample, mix (under sb_cs) ---------------- */

static void sb_decode_frame(const BYTE *p, int16_t *l, int16_t *r)
{
    if (sb_bits == 16) {
        int s0 = (int)(int16_t)(p[0] | (p[1] << 8));
        if (!sb_signed) s0 = (int)((p[0] | (p[1] << 8))) - 32768;
        if (sb_stereo) {
            int s1 = (int)(int16_t)(p[2] | (p[3] << 8));
            if (!sb_signed) s1 = (int)((p[2] | (p[3] << 8))) - 32768;
            *l = (int16_t)s0; *r = (int16_t)s1;
        } else {
            *l = *r = (int16_t)s0;
        }
    } else { /* 8-bit */
        int s0 = sb_signed ? (int)(signed char)p[0] : ((int)p[0] - 128);
        s0 <<= 8;
        if (sb_stereo) {
            int s1 = sb_signed ? (int)(signed char)p[1] : ((int)p[1] - 128);
            s1 <<= 8;
            *l = (int16_t)s0; *r = (int16_t)s1;
        } else {
            *l = *r = (int16_t)s0;
        }
    }
}

/* Refill the decoded staging buffer from guest DMA. Returns frames produced;
 * 0 on block end (single-cycle: stops) or guest underrun. */
static unsigned sb_refill_stage(void)
{
    unsigned frame, want, avail, frames, i;

    if (!sb_play || !sb_dma_ptr) return 0;

    if (sb_dma_pos >= sb_dma_len) {      /* block fully played */
        if (!sb_diag_pull_logged)        /* DIAG(b24): never saw any data */
            logger_note("sb: WHOLE TRANSFER READ AS ZERO (bad addr / unfilled buffer)");
        sb_post_irq();
        if (sb_autoinit) {
            sb_dma_pos = 0;             /* replay; the guest keeps refilling it */
        } else {
            sb_play = 0;
            return 0;
        }
    }

    frame = (unsigned)(sb_bits / 8) * (sb_stereo ? 2u : 1u);
    if (frame == 0) { sb_play = 0; return 0; }

    avail = sb_dma_len - sb_dma_pos;
    want = SB_STAGE_RAW_BYTES;
    if (want > avail) want = avail;
    want -= want % frame;
    if (want == 0) { sb_dma_pos = sb_dma_len; return 0; }

    CopyMemory(sb_raw, sb_dma_ptr + sb_dma_pos, want);
    if (!sb_diag_pull_logged) {   /* DIAG(b24): scan this read for ANY non-zero PCM */
        unsigned z;
        for (z = 0; z < want; z++) {
            if (sb_raw[z] != 0) {
                sb_diag_pull_logged = 1;
                logger_note_kv("sb: DATA found at bufpos",
                               (unsigned long)(sb_dma_pos + z));
                break;
            }
        }
    }
    sb_dma_pos += want;

    frames = want / frame;
    for (i = 0; i < frames; i++) {
        sb_decode_frame(sb_raw + i * frame, &sb_stage[2 * i], &sb_stage[2 * i + 1]);
    }
    sb_stage_head = frames;
    sb_stage_pos  = 0;
    return frames;
}

static int sb_next_frame(int *l, int *r)
{
    if (sb_stage_pos >= sb_stage_head) {
        if (sb_refill_stage() == 0) return 0;
    }
    *l = sb_stage[2 * sb_stage_pos];
    *r = sb_stage[2 * sb_stage_pos + 1];
    sb_stage_pos++;
    return 1;
}

/* Catmull-Rom cubic between p1 and p2 (p0,p3 the neighbours), f = 16-bit frac.
 * out = p1 + 0.5*t*( (p2-p0) + t*( (2p0-5p1+4p2-p3) + t*(3(p1-p2)+p3-p0) ) ). */
static int sb_cubic(int p0, int p1, int p2, int p3, int f)
{
    int64_t a = 3 * (p1 - p2) + p3 - p0;
    int64_t b = 2 * p0 - 5 * p1 + 4 * p2 - p3;
    int64_t inner = b + ((f * a) >> 16);
    inner = (int64_t)(p2 - p0) + ((f * inner) >> 16);
    return p1 + (int)((f * inner) >> 17);
}

#if SB_WAV_DUMP
/* Write the captured fed bytes to C:\vddsound\dump.wav (8-bit PCM is unsigned,
 * which is exactly what we feed DirectSound). One-shot. */
static void sb_write_wav(void)
{
    HANDLE h;
    DWORD  wr;
    BYTE   hdr[44];
    unsigned dlen = sb_dump_pos;
    unsigned flen = 36u + dlen;
    unsigned brate = sb_dump_rate * sb_dump_ch * (sb_dump_bits / 8u);
    unsigned balign = sb_dump_ch * (sb_dump_bits / 8u);

    hdr[0]='R'; hdr[1]='I'; hdr[2]='F'; hdr[3]='F';
    hdr[4]=(BYTE)flen; hdr[5]=(BYTE)(flen>>8); hdr[6]=(BYTE)(flen>>16); hdr[7]=(BYTE)(flen>>24);
    hdr[8]='W'; hdr[9]='A'; hdr[10]='V'; hdr[11]='E';
    hdr[12]='f'; hdr[13]='m'; hdr[14]='t'; hdr[15]=' ';
    hdr[16]=16; hdr[17]=0; hdr[18]=0; hdr[19]=0;
    hdr[20]=1; hdr[21]=0;                                  /* PCM */
    hdr[22]=(BYTE)sb_dump_ch; hdr[23]=0;
    hdr[24]=(BYTE)sb_dump_rate; hdr[25]=(BYTE)(sb_dump_rate>>8);
    hdr[26]=(BYTE)(sb_dump_rate>>16); hdr[27]=(BYTE)(sb_dump_rate>>24);
    hdr[28]=(BYTE)brate; hdr[29]=(BYTE)(brate>>8); hdr[30]=(BYTE)(brate>>16); hdr[31]=(BYTE)(brate>>24);
    hdr[32]=(BYTE)balign; hdr[33]=(BYTE)(balign>>8);
    hdr[34]=(BYTE)sb_dump_bits; hdr[35]=0;
    hdr[36]='d'; hdr[37]='a'; hdr[38]='t'; hdr[39]='a';
    hdr[40]=(BYTE)dlen; hdr[41]=(BYTE)(dlen>>8); hdr[42]=(BYTE)(dlen>>16); hdr[43]=(BYTE)(dlen>>24);

    h = CreateFileA("C:\\vddsound\\dump.wav", GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, hdr, 44, &wr, NULL);
        WriteFile(h, sb_dump, dlen, &wr, NULL);
        CloseHandle(h);
    }
    logger_note_kv("sb: wrote dump.wav bytes", (unsigned long)dlen);
}
#endif

void sb_mix(int16_t *buf, unsigned frames)
{
    unsigned step, i;

    if (!InterlockedCompareExchange(&sb_cs_ready, 1, 1)) return;
    EnterCriticalSection(&sb_cs);

    /* Meter guest PCM to DirectSound the VDMSound way: consume bytes by
     * wall-clock elapsed x the guest's programmed byte-rate x a feedback scale
     * (1/load) from how full the playback buffer is, capped to one DSP block
     * (<=1 IRQ) and the DMA terminal count; push to a big looping DS buffer that
     * returns the load factor. ACK-gated on SB16 so we stay locked to the guest.
     * The old in-driver resampler below is unreachable (kept so its helpers
     * still compile). */
    if (sb_play && !sb_paused && sb_dma_ptr) {
        unsigned frame = (unsigned)(sb_bits / 8) * (sb_stereo ? 2u : 1u);

        /* SB16 ACK-gate: don't consume the next block until the guest has acked
         * the previous block's IRQ (it clears the pending flag by reading
         * 0x22E/0x22F), so our read stays ~1 block ahead of the guest's refill
         * instead of lapping its small DMA ring. The safety threshold must be a
         * genuine last-resort (only a multi-second guest stall), NOT a normal
         * path: at 64 ticks it fired constantly and let us read 21 blocks ahead
         * (b44 trace, posts-acks=21), lapping DOOM's 8-block ring = garbage. */
        if (!(sb_bits < 16 ? sb_irq8_pending : sb_irq16_pending)) sb_gate_wait = 0;
        else sb_gate_wait++;

        if (frame && sb_block_bytes &&
            (sb_gate_wait == 0 || sb_gate_wait >= 2000u)) {
            LARGE_INTEGER now;
            unsigned long long elapsed, base64;
            unsigned base, scale_fp, want, cap, avail, tonext, overshoot, k;

            QueryPerformanceCounter(&now);
            elapsed = (unsigned long long)(now.QuadPart - sb_last_qpc.QuadPart);
            sb_last_qpc = now;

            /* bytes earned this tick = byte-rate * elapsed-seconds, scaled by the
             * playback feedback (1/load in 8.8 fp, clamped to [0, 2]). */
            base64   = (unsigned long long)sb_byterate * elapsed / sb_qpc_freq;
            base     = (base64 > sizeof(sb_snapshot)) ? (unsigned)sizeof(sb_snapshot)
                                                      : (unsigned)base64;
            scale_fp = 65536u / (sb_load_fp ? sb_load_fp : 1u);
            if (scale_fp > 512u) scale_fp = 512u;
            want = (base * scale_fp) / 256u;

            /* cap to the next DSP-block boundary (so <=1 IRQ) and the DMA
             * terminal count; round up tiny leftovers to the boundary. */
            avail  = sb_dma_len - sb_dma_pos;
            tonext = sb_block_bytes - (sb_xfer_total % sb_block_bytes);
            cap    = (tonext < avail) ? tonext : avail;
            want  -= want % frame;
            if (want > cap)             want = cap;
            else if (cap - want < 128u) want = cap;
            want -= want % frame;

            if (want > 0) {
#if SB_TONE_TEST
                /* Feed a pure ~344 Hz (DOOM) / ~188 Hz (Skyroads) tone instead
                 * of the guest, to hear the DS PCM output path in isolation.
                 * 8-bit unsigned (DS native), centred on 0x80 - no XOR. */
                {
                    static const BYTE tone[32] = {
                        128,151,174,195,213,228,239,246,248,246,239,228,
                        213,195,174,151,128,105, 82, 61, 43, 28, 17, 10,
                          8, 10, 17, 28, 43, 61, 82,105 };
                    static unsigned ph;
                    if (frame >= 2u) {
                        for (k = 0; k + 1u < want; k += 2u) {
                            BYTE s = tone[ph++ & 31u];
                            sb_snapshot[k] = s; sb_snapshot[k + 1u] = s;
                        }
                    } else {
                        for (k = 0; k < want; k++) sb_snapshot[k] = tone[ph++ & 31u];
                    }
                }
#else
                /* Re-resolve the guest pointer FRESH each read (VDMSound maps on
                 * every transfer; we used to cache it once at sb_start - the
                 * clearest deviation). [b48] */
                {
                    const BYTE *gp = (const BYTE *)MGetVdmPointer(sb_dma_lin,
                                                                  sb_dma_len, FALSE);
                    if (gp != NULL) CopyMemory(sb_snapshot, gp + sb_dma_pos, want);
                    else            ZeroMemory(sb_snapshot, want);
                }
                if (sb_bits == 8 && sb_signed)
                    for (k = 0; k < want; k++) sb_snapshot[k] ^= 0x80u;
#endif
#if SB_WAV_DUMP
                if (!sb_dump_done) {            /* capture the fed stream to a WAV */
                    unsigned room, cpy;
                    if (sb_dump_pos == 0u) {
                        sb_dump_rate = sb_rate;
                        sb_dump_bits = (unsigned)sb_bits;
                        sb_dump_ch   = sb_stereo ? 2u : 1u;
                    }
                    room = (unsigned)sizeof(sb_dump) - sb_dump_pos;
                    cpy  = (want < room) ? want : room;
                    CopyMemory(sb_dump + sb_dump_pos, sb_snapshot, cpy);
                    sb_dump_pos += cpy;
                    if (sb_dump_pos >= (unsigned)sizeof(sb_dump)) {
                        sb_write_wav();
                        sb_dump_done = 1;
                    }
                }
#endif
                sb_load_fp     = audio_pcm_play(sb_snapshot, want);
                sb_dma_pos    += want;
                sb_xfer_total += want;

                /* advance NTVDM's emulated 8237 so a polling guest (DOOM/DMX
                 * reads ports 0x02/0x03) tracks the play cursor [b42-setdma] */
                {
                    VDD_DMA_INFO di;
                    unsigned xfers = (sb_channel >= 4u) ? (sb_dma_pos >> 1)
                                                        : sb_dma_pos;
                    di.addr   = (WORD)(sb_base_addr + xfers);
                    di.count  = (WORD)(sb_base_count - xfers);
                    di.page   = (WORD)sb_base_page;
                    di.status = 0; di.mode = 0; di.mask = 0;
                    VDDSetDMA(sb_hvdd, sb_channel,
                              VDD_DMA_ADDR | VDD_DMA_COUNT, &di);
                }

                /* one completion IRQ per DSP block crossed; single-cycle stops
                 * at the block end (VDMSound HandleAfterTransfer). */
                overshoot = sb_xfer_total % sb_block_bytes;
                if (want > overshoot) {
                    sb_post_irq();
                    if (!sb_autoinit) { sb_play = 0; audio_pcm_close(); }
                }
                if (sb_autoinit && sb_dma_pos >= sb_dma_len)
                    sb_dma_pos = 0;          /* wrap the auto-init ring */
            }
        }

        /* health log (~1 s of playing ticks) */
        if (++sb_diag_tick_acc >= 1024u) {
            WORD qi[16];
            ZeroMemory(qi, sizeof(qi));
            VDDQueryDMA(sb_hvdd, sb_channel, qi);
            logger_note_kv("sb: load fp (256=on target)", (unsigned long)sb_load_fp);
            logger_note_kv("sb: bytes transferred", (unsigned long)sb_xfer_total);
            logger_note_kv("sb: irq posts", (unsigned long)sb_irq_posts);
            logger_note_kv("sb: irq acks", (unsigned long)sb_irq_acks);
            logger_note_kv("sb: gate wait now", (unsigned long)sb_gate_wait);
            logger_note_kv("sb: live DMA count", (unsigned long)qi[1]);
            /* DIAG(b48): hash the whole guest DMA buffer. If this CHANGES across
             * windows the guest is refilling it (and we were reading stale);
             * if CONSTANT the guest never refills (its mixer/timer is stuck). */
            {
                const BYTE *gp = (const BYTE *)MGetVdmPointer(sb_dma_lin, sb_dma_len, FALSE);
                unsigned h = 2166136261u, z;
                if (gp) for (z = 0; z < sb_dma_len; z++) h = (h ^ gp[z]) * 16777619u;
                logger_note_kv("sb: guest buf hash", (unsigned long)h);
            }
            sb_diag_tick_acc = 0;
        }
    }
    LeaveCriticalSection(&sb_cs);
    return;

    if (!sb_play || sb_paused) { LeaveCriticalSection(&sb_cs); return; }

    step = (sb_rate << 16) / AUDIO_SAMPLE_RATE;
    if (step == 0) step = 1;

    if (!sb_have_cur) {
        /* prime the 4-sample window; interpolation is between p1 and p2. */
        if (!sb_next_frame(&sb_p1l, &sb_p1r)) { LeaveCriticalSection(&sb_cs); return; }
        sb_p0l = sb_p1l; sb_p0r = sb_p1r;
        if (!sb_next_frame(&sb_p2l, &sb_p2r)) { sb_p2l = sb_p1l; sb_p2r = sb_p1r; }
        if (!sb_next_frame(&sb_p3l, &sb_p3r)) { sb_p3l = sb_p2l; sb_p3r = sb_p2r; }
        sb_have_cur = 1;
        sb_phase = 0;
    }

    for (i = 0; i < frames; i++) {
        int f = (int)(sb_phase & 0xFFFFu);
        int l = sb_cubic(sb_p0l, sb_p1l, sb_p2l, sb_p3l, f);
        int r = sb_cubic(sb_p0r, sb_p1r, sb_p2r, sb_p3r, f);

        if (sb_speaker) {
            int ml = (int)buf[2 * i]     + l;
            int mr = (int)buf[2 * i + 1] + r;
            buf[2 * i]     = (int16_t)(ml < -32768 ? -32768 : ml > 32767 ? 32767 : ml);
            buf[2 * i + 1] = (int16_t)(mr < -32768 ? -32768 : mr > 32767 ? 32767 : mr);
        }

        sb_phase += step;
        while (sb_phase >= 0x10000u) {
            sb_phase -= 0x10000u;
            sb_p0l = sb_p1l; sb_p1l = sb_p2l; sb_p2l = sb_p3l;
            sb_p0r = sb_p1r; sb_p1r = sb_p2r; sb_p2r = sb_p3r;
            if (!sb_next_frame(&sb_p3l, &sb_p3r)) {
                sb_p3l = sb_p2l; sb_p3r = sb_p2r;
                if (!sb_play) { LeaveCriticalSection(&sb_cs); return; }
                /* else: transient guest underrun - hold last sample. */
            }
        }
    }

    LeaveCriticalSection(&sb_cs);
}
