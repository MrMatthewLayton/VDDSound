/*
 * audio_out.c - one looping DirectSound secondary buffer, refilled behind the
 * play cursor by a dedicated render thread that pulls FM PCM from opl_render.
 * COM interfaces are used via lpVtbl-> (C calling convention).
 */
#include "common.h"
#include "audio_out.h"
#include "opl_module.h"

#include <dsound.h>
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
    (void)arg;

    while (InterlockedCompareExchange(&running, 1, 1)) {
        DWORD play = 0, write = 0, avail;
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

int audio_init(void)
{
    DWORD tid;

    if (FAILED(DirectSoundCreate(NULL, &ds, NULL))) {
        return 1;
    }
    if (FAILED(ds->lpVtbl->SetCooperativeLevel(ds, GetDesktopWindow(),
                                               DSSCL_PRIORITY))) {
        /* Fall back to a normal cooperative level if PRIORITY is refused. */
        ds->lpVtbl->SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_NORMAL);
    }
    if (create_buffer() != 0) {
        ds->lpVtbl->Release(ds);
        ds = NULL;
        return 1;
    }

    dsb->lpVtbl->Play(dsb, 0, 0, DSBPLAY_LOOPING);

    running = 1;
    render_thread = CreateThread(NULL, 0, render_proc, NULL, 0, &tid);
    if (render_thread == NULL) {
        running = 0;
        dsb->lpVtbl->Stop(dsb);
        dsb->lpVtbl->Release(dsb);
        ds->lpVtbl->Release(ds);
        dsb = NULL;
        ds = NULL;
        return 1;
    }
    SetThreadPriority(render_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return 0;
}

void audio_shutdown(void)
{
    if (render_thread != NULL) {
        InterlockedExchange(&running, 0);
        WaitForSingleObject(render_thread, 2000);
        CloseHandle(render_thread);
        render_thread = NULL;
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
