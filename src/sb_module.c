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
static unsigned    sb_dma_len;     /* buffer length in bytes */
static unsigned    sb_dma_pos;     /* play cursor within the buffer */
static unsigned    sb_irq_accum;   /* bytes fed since the last block-completion IRQ */
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

/* ---- b35 feed-path diagnostics (render thread only) --------------------- */
static int      sb_diag_feeds;       /* fed-byte hex dumps emitted this transfer */
static unsigned sb_diag_min_lead;    /* min lead (bytes) seen this window */
static unsigned sb_diag_underruns;   /* ticks the lead hit ~0 mid-stream */
static unsigned sb_diag_tick_acc;    /* playing ticks since last health log */
static unsigned sb_diag_fed_total;   /* total bytes fed to DS this transfer */
static int      sb_diag_req;         /* VDDRequestDMA return dumps emitted */

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
    sb_irq_accum   = 0;
    InterlockedExchange(&sb_irq_posts, 0);   /* DIAG(b38) */
    InterlockedExchange(&sb_irq_acks, 0);
    sb_diag_pull_logged = 0;
    sb_diag_feeds     = 0;
    sb_diag_min_lead  = 0xFFFFFFFFu;
    sb_diag_underruns = 0;
    sb_diag_tick_acc  = 0;
    sb_diag_fed_total = 0;
    sb_diag_req       = 0;
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

/* DIAG(b35): dump up to 32 bytes as hex (the bytes we actually feed to
 * DirectSound, post signed->unsigned convert). Smooth ramps => the bug is in
 * the DS streaming-buffer mechanics; jagged garbage => the bug is upstream in
 * how we read/convert the guest buffer. CRT-free hand-rolled formatting. */
static void sb_diag_hex(const char *tag, const BYTE *p, unsigned n)
{
    static const char hexd[] = "0123456789ABCDEF";
    char line[160];
    int pos = 0;
    unsigned i;
    while (*tag && pos < 28) line[pos++] = *tag++;
    line[pos++] = ':';
    line[pos++] = ' ';
    if (n > 32u) n = 32u;
    for (i = 0; i < n; i++) {
        line[pos++] = hexd[(p[i] >> 4) & 0xFu];
        line[pos++] = hexd[p[i] & 0xFu];
        line[pos++] = ' ';
    }
    line[pos] = '\0';
    logger_note(line);
}

void sb_mix(int16_t *buf, unsigned frames)
{
    unsigned step, i;

    if (!InterlockedCompareExchange(&sb_cs_ready, 1, 1)) return;
    EnterCriticalSection(&sb_cs);

    /* Stream guest PCM to DirectSound LIVE: hold a ~20ms lead by reading the
     * guest buffer just behind the play cursor (like the hardware DMA), 8-bit
     * signed->unsigned for DS. The old in-driver resampler below is unreachable
     * (kept so its helpers still compile). */
    if (sb_play && !sb_paused && sb_dma_ptr) {
        unsigned frame  = (unsigned)(sb_bits / 8) * (sb_stereo ? 2u : 1u);
        unsigned target = frame ? (sb_rate * frame) / 50u : 0u;   /* ~20 ms */
        unsigned lead   = audio_pcm_lead();

        /* DIAG(b35): lead/pacing health. If the lead frequently hits ~0 the DS
         * play cursor is lapping our writes (underrun) - that would BE the
         * crackle. Logged once per ~1s of playing ticks. */
        if (lead < sb_diag_min_lead) sb_diag_min_lead = lead;
        if (frame && lead < frame && sb_diag_fed_total > 0u) sb_diag_underruns++;
        if (++sb_diag_tick_acc >= 1024u) {
            unsigned f = sb_dma_pos, zrun = 0, frontier = sb_dma_pos;
            logger_note_kv("sb: min lead bytes /1k ticks", (unsigned long)sb_diag_min_lead);
            logger_note_kv("sb: lead underruns /1k ticks", (unsigned long)sb_diag_underruns);
            logger_note_kv("sb: target lead bytes", (unsigned long)target);
            /* DIAG(b36): find the guest's fill frontier - the end of real data,
             * i.e. the start of a long run of source-silence (0x00, unwritten
             * DMA). If our read pos sits at/near the frontier the game has not
             * filled that far yet, so we feed unwritten silence into real audio
             * = tearing crackle. The frontier's advance rate per ~1s window is
             * the game's true (NTVDM-paced) DMA fill rate - what the fix paces
             * to. Pure guest-memory reads: no cross-thread VDD API. */
            while (f < sb_dma_len && zrun < 64u) {
                if (sb_dma_ptr[f] == 0x00u) zrun++;
                else { zrun = 0; frontier = f; }
                f++;
            }
            logger_note_kv("sb: read pos", (unsigned long)sb_dma_pos);
            logger_note_kv("sb: fill frontier", (unsigned long)frontier);
            logger_note_kv("sb: fill margin ahead", (unsigned long)(frontier - sb_dma_pos));
            logger_note_kv("sb: irq posts (cumulative)", (unsigned long)sb_irq_posts);
            logger_note_kv("sb: irq acks (cumulative)", (unsigned long)sb_irq_acks);
            logger_note_kv("sb: DS underruns (overtake)", (unsigned long)audio_pcm_underruns());
            /* DIAG(b39): does NTVDM's emulated DMA cursor advance? We never call
             * VDDRequestDMA, so the 8237 count may be frozen at its initial
             * value. If it DECREASES over windows, NTVDM is pacing the channel
             * and we can sync our read to it (the guest's own clock) to stop the
             * tearing; if frozen, we must drive the DMA to advance it. Safe from
             * the render thread - we already raise IRQs (call_ica_hw_interrupt)
             * from here. */
            {
                WORD qi[16];
                ZeroMemory(qi, sizeof(qi));
                VDDQueryDMA(sb_hvdd, sb_channel, qi);
                logger_note_kv("sb: live DMA count", (unsigned long)qi[1]);
                logger_note_kv("sb: live DMA addr", (unsigned long)qi[0]);
            }
            sb_diag_tick_acc  = 0;
            sb_diag_min_lead  = 0xFFFFFFFFu;
            sb_diag_underruns = 0;
        }

        if (frame && lead < target) {
            unsigned want  = target - lead;
            unsigned avail = sb_dma_len - sb_dma_pos;
            unsigned k, fed;
            if (want > avail)               want = avail;
            if (want > sizeof(sb_snapshot)) want = sizeof(sb_snapshot);
            want -= want % frame;
            if (want > 0) {
                CopyMemory(sb_snapshot, sb_dma_ptr + sb_dma_pos, want);
                /* DIAG(b35/b39): raw (pre-xor) then fed (post-xor) bytes. Raw
                 * centered on 0x80 = unsigned source; centered on 0x00 = signed
                 * source. Lets us read signedness off the data, not the guess. */
                if (sb_diag_feeds < 6 && want >= 16u) {
                    logger_note_kv("sb: feed want", (unsigned long)want);
                    logger_note_kv("sb: feed lead", (unsigned long)lead);
                    sb_diag_hex("sb raw pre-xor", sb_snapshot, want);
                }
                if (sb_bits == 8 && sb_signed)
                    for (k = 0; k < want; k++) sb_snapshot[k] ^= 0x80u;
                if (sb_diag_feeds < 6 && want >= 16u) {
                    sb_diag_hex("sb fed post-xor", sb_snapshot, want);
                    sb_diag_feeds++;
                }
                fed = audio_pcm_feed(sb_snapshot, want);
                sb_dma_pos        += fed;
                sb_diag_fed_total += fed;

                /* Advance NTVDM's emulated 8237 current addr/count so a guest
                 * that POLLS the DMA controller sees the play cursor move. DOOM/
                 * DMX reads ports 0x02/0x03 (current address) each interrupt to
                 * pick which block to mix next; b39 proved the count was frozen
                 * (VDDRequestDMA is inert here, b40: got=0), so DMX re-mixed one
                 * block forever = chk-a-chk-a. We drive the controller directly
                 * with VDDSetDMA, the way VDMSound does. The 8237 counts DOWN in
                 * transfers (bytes for 8-bit ch<4, words for ch>=4); sb_dma_pos
                 * is our byte cursor and wraps the ring on auto-init, which
                 * reloads addr/count to base (the correct auto-init behaviour).
                 * [b42-setdma] */
                {
                    VDD_DMA_INFO di;
                    unsigned xfers = (sb_channel >= 4u) ? (sb_dma_pos >> 1)
                                                        : sb_dma_pos;
                    BOOL ok;
                    di.addr   = (WORD)(sb_base_addr + xfers);
                    di.count  = (WORD)(sb_base_count - xfers);
                    di.page   = (WORD)sb_base_page;
                    di.status = 0;
                    di.mode   = 0;
                    di.mask   = 0;
                    ok = VDDSetDMA(sb_hvdd, sb_channel,
                                   VDD_DMA_ADDR | VDD_DMA_COUNT, &di);
                    if (sb_diag_req < 4) {
                        logger_note_kv("sb: VDDSetDMA ok",    (unsigned long)ok);
                        logger_note_kv("sb: VDDSetDMA count", (unsigned long)di.count);
                        sb_diag_req++;
                    }
                }

                /* Fire the completion IRQ every programmed block (sb_block_bytes),
                 * NOT once per DMA-buffer wrap: the SB raises one IRQ per block,
                 * and games pack several blocks into one auto-init ring (DOOM:
                 * 512-byte block inside a 4096-byte ring = 8 IRQs/loop). b36
                 * showed under-IRQing makes the guest refill late, so its fill
                 * frontier rode right at our read cursor -> tearing (DOOM fill
                 * margin ~0). [b37-irqblk] */
                sb_irq_accum += fed;
                while (sb_block_bytes && sb_irq_accum >= sb_block_bytes) {
                    sb_irq_accum -= sb_block_bytes;
                    sb_post_irq();
                    if (!sb_autoinit) {     /* single-cycle: one block then stop */
                        sb_play = 0;
                        audio_pcm_close();
                        break;
                    }
                }
                if (sb_autoinit && sb_dma_pos >= sb_dma_len)
                    sb_dma_pos = 0;         /* wrap the physical auto-init ring */
            }
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
