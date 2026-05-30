/*
 * audio_out.c - one looping DirectSound secondary buffer, refilled behind the
 * play cursor by a dedicated render thread that pulls FM PCM from opl_render.
 * COM interfaces are used via lpVtbl-> (C calling convention).
 */
#include "common.h"
#include "audio_out.h"
#include "opl_module.h"
#include "sb_module.h"
#include "logger.h"

#include <dsound.h>
#include <mmsystem.h>   /* timeBeginPeriod/timeEndPeriod (winmm) */
#include <stdint.h>

/* Ring buffer = NUM_CHUNKS x CHUNK_FRAMES. The render thread refills one chunk
 * at a time behind the play cursor, so the whole ring stays queued ahead of
 * playback: a deeper ring tolerates longer render-thread scheduling gaps (NTVDM
 * readily starves core 0) and removes the underruns behind the FM glitching, at
 * the cost of that much output latency. Tune via NUM_CHUNKS; keep CHUNK_FRAMES
 * modest since write_chunk() puts CHUNK_FRAMES*2 int16 on the stack and finer
 * chunks refill more smoothly. */
#define CHUNK_FRAMES 768u                         /* 16 ms/chunk @ 48 kHz */
#define NUM_CHUNKS   24u                          /* ~384 ms ring (was 6 = ~96 ms, underran) */
#define FRAME_BYTES  4u                           /* 16-bit stereo */
#define CHUNK_BYTES  (CHUNK_FRAMES * FRAME_BYTES)
#define BUF_BYTES    (CHUNK_BYTES * NUM_CHUNKS)

static LPDIRECTSOUND       ds;
static LPDIRECTSOUNDBUFFER dsb;
static HANDLE              render_thread;
static volatile LONG       running;

static int write_chunk(DWORD offset)
{
    void *p1 = NULL, *p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    HRESULT hr;
    int16_t tmp[CHUNK_FRAMES * 2];

    hr = dsb->lpVtbl->Lock(dsb, offset, CHUNK_BYTES, &p1, &b1, &p2, &b2, 0);
    if (hr == DSERR_BUFFERLOST) {
        dsb->lpVtbl->Restore(dsb);
        hr = dsb->lpVtbl->Lock(dsb, offset, CHUNK_BYTES, &p1, &b1, &p2, &b2, 0);
    }
    if (FAILED(hr)) {
        return 0;
    }

    opl_render(tmp, CHUNK_FRAMES);

    CopyMemory(p1, (const char *)tmp, b1);
    if (p2 != NULL && b2 != 0) {
        CopyMemory(p2, (const char *)tmp + b1, b2);
    }
    dsb->lpVtbl->Unlock(dsb, p1, b1, p2, b2);
    return 1;
}

static DWORD WINAPI render_proc(LPVOID arg)
{
    DWORD wr = 0;
    unsigned acc = 0, peak = 0;   /* fill-health meter */
    (void)arg;

    while (InterlockedCompareExchange(&running, 1, 1)) {
        DWORD play = 0, write = 0, avail;
        unsigned burst = 0;
        if (FAILED(dsb->lpVtbl->GetCurrentPosition(dsb, &play, &write))) {
            Sleep(5);
            continue;
        }
        avail = (play + BUF_BYTES - wr) % BUF_BYTES;
        while (avail >= CHUNK_BYTES) {
            if (!write_chunk(wr)) {
                break;
            }
            wr = (wr + CHUNK_BYTES) % BUF_BYTES;
            avail -= CHUNK_BYTES;
            burst++;
        }
        /* Having to fill most of the ring in one wake means the thread was
         * starved that long: peak near NUM_CHUNKS == underrun (audible crackle).
         * Logged every ~256 chunks (~4s) so we can confirm the cause. */
        sb_mix(NULL, 0);   /* PCM streaming feeder: top up the lead each ~1ms */

        if (burst > peak) peak = burst;
        if ((acc += burst) >= 256u) {
            logger_note_kv("audio: peak chunks/wake (max=NUM_CHUNKS)",
                           (unsigned long)peak);
            acc = 0;
            peak = 0;
        }
        Sleep(1);
    }
    return 0;
}

static int create_buffer(void)
{
    WAVEFORMATEX wfx;
    DSBUFFERDESC desc;
    void *p1 = NULL, *p2 = NULL;
    DWORD b1 = 0, b2 = 0;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize        = sizeof(desc);
    desc.dwFlags       = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    desc.dwBufferBytes = BUF_BYTES;
    desc.lpwfxFormat   = &wfx;

    if (FAILED(ds->lpVtbl->CreateSoundBuffer(ds, &desc, &dsb, NULL))) {
        return 1;
    }

    /* Pre-fill the whole ring with silence. */
    if (SUCCEEDED(dsb->lpVtbl->Lock(dsb, 0, BUF_BYTES, &p1, &b1, &p2, &b2, 0))) {
        if (p1 != NULL) ZeroMemory(p1, b1);
        if (p2 != NULL) ZeroMemory(p2, b2);
        dsb->lpVtbl->Unlock(dsb, p1, b1, p2, b2);
    }
    return 0;
}

/* ---- streaming PCM via a looping DirectSound buffer (DS does SRC + mixing) - */
static LPDIRECTSOUNDBUFFER pcm_buf;
static CRITICAL_SECTION    pcm_lock;
static volatile LONG       pcm_lock_ready;
static unsigned            pcm_bytes;   /* ring size */
static DWORD               pcm_write;   /* our write cursor */
static int                 pcm_on;
static volatile LONG       pcm_underruns; /* play cursor overtook write (b41) */

int audio_pcm_open(unsigned rate, int bits, int channels)
{
    WAVEFORMATEX wfx;
    DSBUFFERDESC desc;
    void *p1 = NULL, *p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    unsigned sz, align = (unsigned)(channels * bits / 8);

    if (ds == NULL || align == 0) return 1;
    if (!InterlockedCompareExchange(&pcm_lock_ready, 1, 1)) return 1;

    EnterCriticalSection(&pcm_lock);
    if (pcm_buf != NULL) {
        pcm_buf->lpVtbl->Stop(pcm_buf);
        pcm_buf->lpVtbl->Release(pcm_buf);
        pcm_buf = NULL;
    }
    sz = (rate * align) / 8u;            /* ~125 ms ring */
    if (sz < 4096u) sz = 4096u;
    sz -= sz % align;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = (WORD)channels;
    wfx.nSamplesPerSec  = rate;
    wfx.wBitsPerSample  = (WORD)bits;
    wfx.nBlockAlign     = (WORD)align;
    wfx.nAvgBytesPerSec = rate * align;

    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize        = sizeof(desc);
    desc.dwFlags       = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes = sz;
    desc.lpwfxFormat   = &wfx;

    if (FAILED(ds->lpVtbl->CreateSoundBuffer(ds, &desc, &pcm_buf, NULL))) {
        pcm_buf = NULL;
        LeaveCriticalSection(&pcm_lock);
        return 1;
    }
    if (SUCCEEDED(pcm_buf->lpVtbl->Lock(pcm_buf, 0, sz, &p1, &b1, &p2, &b2, 0))) {
        FillMemory(p1, b1, (bits == 8) ? 0x80 : 0x00);
        if (p2 != NULL && b2 != 0) FillMemory(p2, b2, (bits == 8) ? 0x80 : 0x00);
        pcm_buf->lpVtbl->Unlock(pcm_buf, p1, b1, p2, b2);
    }
    pcm_bytes = sz;
    pcm_write = 0;
    pcm_on    = 1;
    InterlockedExchange(&pcm_underruns, 0);
    pcm_buf->lpVtbl->SetCurrentPosition(pcm_buf, 0);
    pcm_buf->lpVtbl->Play(pcm_buf, 0, 0, DSBPLAY_LOOPING);
    LeaveCriticalSection(&pcm_lock);
    return 0;
}

unsigned audio_pcm_lead(void)
{
    DWORD play, write;
    unsigned lead = 0;
    if (!InterlockedCompareExchange(&pcm_lock_ready, 1, 1)) return 0;
    EnterCriticalSection(&pcm_lock);
    if (pcm_on && pcm_buf != NULL &&
        SUCCEEDED(pcm_buf->lpVtbl->GetCurrentPosition(pcm_buf, &play, &write))) {
        lead = (pcm_write + pcm_bytes - play) % pcm_bytes;
        /* If the play cursor has overtaken our write cursor the ring has
         * underrun: that shows up as a lead near the FULL ring, not ~0 (the
         * small-lead meter in sb_mix is blind to it). Count it and report 0 so
         * the feeder tops up immediately instead of coasting a whole ring of
         * stale data (~the periodic crackle / chk-a-chk-a). [b41] */
        if (pcm_bytes != 0 && lead > pcm_bytes / 2u) {
            InterlockedIncrement(&pcm_underruns);
            lead = 0;
        }
    }
    LeaveCriticalSection(&pcm_lock);
    return lead;
}

unsigned audio_pcm_underruns(void)
{
    return (unsigned)pcm_underruns;
}

unsigned audio_pcm_feed(const void *data, unsigned n)
{
    void *p1 = NULL, *p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    unsigned wrote = 0;
    if (!InterlockedCompareExchange(&pcm_lock_ready, 1, 1)) return 0;
    EnterCriticalSection(&pcm_lock);
    if (pcm_on && pcm_buf != NULL && n > 0 &&
        SUCCEEDED(pcm_buf->lpVtbl->Lock(pcm_buf, pcm_write, n, &p1, &b1, &p2, &b2, 0))) {
        CopyMemory(p1, data, b1);
        if (p2 != NULL && b2 != 0) CopyMemory(p2, (const char *)data + b1, b2);
        pcm_buf->lpVtbl->Unlock(pcm_buf, p1, b1, p2, b2);
        pcm_write = (pcm_write + n) % pcm_bytes;
        wrote = n;
    }
    LeaveCriticalSection(&pcm_lock);
    return wrote;
}

void audio_pcm_close(void)
{
    if (!InterlockedCompareExchange(&pcm_lock_ready, 1, 1)) return;
    EnterCriticalSection(&pcm_lock);
    if (pcm_buf != NULL) {
        pcm_buf->lpVtbl->Stop(pcm_buf);
        pcm_buf->lpVtbl->Release(pcm_buf);
        pcm_buf = NULL;
    }
    pcm_on = 0;
    LeaveCriticalSection(&pcm_lock);
}

int audio_init(void)
{
    DWORD tid;

    InitializeCriticalSection(&pcm_lock);
    InterlockedExchange(&pcm_lock_ready, 1);

    if (FAILED(DirectSoundCreate(NULL, &ds, NULL))) {
        return 1;
    }
    if (FAILED(ds->lpVtbl->SetCooperativeLevel(ds, GetDesktopWindow(),
                                               DSSCL_PRIORITY))) {
        /* Fall back to a normal cooperative level if PRIORITY is refused. */
        ds->lpVtbl->SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_NORMAL);
    }

    /* Set the primary buffer to 48 kHz/16/stereo so DirectSound mixes our
     * secondary buffers at full rate. The default primary is often 22050 Hz,
     * which forces an ugly resample of the 6024/11025 Hz PCM (a real crackle
     * source). Best-effort: needs DSSCL_PRIORITY (set above); ignore failure
     * and the SetFormat persists after we release the primary. [b41] */
    {
        LPDIRECTSOUNDBUFFER prim = NULL;
        DSBUFFERDESC pd;
        ZeroMemory(&pd, sizeof(pd));
        pd.dwSize  = sizeof(pd);
        pd.dwFlags = DSBCAPS_PRIMARYBUFFER;
        if (SUCCEEDED(ds->lpVtbl->CreateSoundBuffer(ds, &pd, &prim, NULL)) &&
            prim != NULL) {
            WAVEFORMATEX pf;
            ZeroMemory(&pf, sizeof(pf));
            pf.wFormatTag      = WAVE_FORMAT_PCM;
            pf.nChannels       = 2;
            pf.nSamplesPerSec  = AUDIO_SAMPLE_RATE;
            pf.wBitsPerSample  = 16;
            pf.nBlockAlign     = 4;
            pf.nAvgBytesPerSec = AUDIO_SAMPLE_RATE * 4u;
            prim->lpVtbl->SetFormat(prim, &pf);
            prim->lpVtbl->Release(prim);
        }
    }

    if (create_buffer() != 0) {
        ds->lpVtbl->Release(ds);
        ds = NULL;
        return 1;
    }

    dsb->lpVtbl->Play(dsb, 0, 0, DSBPLAY_LOOPING);

    timeBeginPeriod(1);   /* ~1ms scheduler tick for the render loop's Sleep(1) */
    running = 1;
    render_thread = CreateThread(NULL, 0, render_proc, NULL, 0, &tid);
    if (render_thread == NULL) {
        running = 0;
        timeEndPeriod(1);
        dsb->lpVtbl->Stop(dsb);
        dsb->lpVtbl->Release(dsb);
        ds->lpVtbl->Release(ds);
        dsb = NULL;
        ds = NULL;
        return 1;
    }
    SetThreadPriority(render_thread, THREAD_PRIORITY_TIME_CRITICAL);
    return 0;
}

void audio_shutdown(void)
{
    if (render_thread != NULL) {
        InterlockedExchange(&running, 0);
        WaitForSingleObject(render_thread, 2000);
        CloseHandle(render_thread);
        render_thread = NULL;
        timeEndPeriod(1);
    }
    if (dsb != NULL) {
        dsb->lpVtbl->Stop(dsb);
        dsb->lpVtbl->Release(dsb);
        dsb = NULL;
    }
    if (ds != NULL) {
        ds->lpVtbl->Release(ds);
        ds = NULL;
    }
}
