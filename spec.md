# vddsound — A Replacement Virtual Sound Blaster for NTVDM

> ⚠️ **This is the original design brief (aspirational). For the ACTUAL current
> state — what's built, what works, what's blocked, the build/deploy loop, and
> hard-won findings — read [`docs/STATUS.md`](docs/STATUS.md) first. Where this
> brief and STATUS.md disagree, STATUS.md wins.** In particular: the MVP was
> re-scoped to a *replacement* VSB (FM+MIDI to host); port ownership and routing
> work; the remaining blocker is NTVDM's emulated timebase (tempo distortion),
> which is an NTVDM-wide limitation, not our code.

You are being asked to build the project described below. Read this brief in full before touching any code. Where it makes a decision, the decision is final; where it leaves a choice open, propose one and proceed.

> **Authoritative references used to harden this spec.** VDD struct layouts, function signatures, and calling conventions are quoted from the ReactOS DDK headers `sdk/include/ddk/nt_vdd.h` and `sdk/include/ddk/vddsvc.h`. The `.def` import list is validated against the **actual target binary** in `vdm/ntvdm.exe` (file version 5.1.2600.5512, XP SP3). The built-in-VSB behaviour and the VDD-vs-VSB init order are verified against leecher1337's `ntvdmx64` reconstruction of the NT4/XP NTVDM sound source. MPU-401 and Sound Blaster byte-level semantics come from VDMSound's own source (`volkertb/vdmsound`). The OPL core, audio output path, and macOS toolchain are verified against Nuked-OPL3, MSDN DirectSound docs, and the Homebrew `mingw-w64` formula. URLs are cited inline where a decision depends on them.

## Goal

Build a 32-bit Win32 Virtual Device Driver (VDD) DLL — `vddsound.dll` — that loads into NTVDM on Windows XP SP3 (32-bit) and **replaces NTVDM's broken built-in Virtual Sound Blaster (VSB)** with a sound bridge that actually works. It traps the guest's FM (OPL) and MIDI (MPU-401) port I/O and routes audio to the host's Windows sound stack — OPL synthesised in software and streamed to DirectSound, MIDI passed through to `winmm`. It installs once via the registry and is active automatically in **every** NTVDM session (native feel — no launcher, no per-game shortcut). Forensic-grade trace logging is a first-class instrumentation feature. Cross-built from **macOS** with MinGW-w64, with no dependency on the Windows DDK.

## Why this exists: NTVDM's built-in VSB is inadequate

Static analysis of the target `vdm/ntvdm.exe` and the `ntvdmx64` source reconstruction confirm that NTVDM on XP ships a **built-in Virtual Sound Blaster (VSB)** — its own diagnostic strings enumerate `VSB: SINGLE CYCLE mode`, `AUTO-INIT mode`, `HIGH SPEED…`, `ADLIB/FM mode`, `MIDI mode`, `MIXER mode`, and it loads `WINMM.DLL` to bridge to host `waveOut`/`midiOut`. The machine's own `vdm/details.txt` (its real `autoexec.nt`) states the limitation in prose: **"NTVDM supports Sound Blaster 2.0 only. The T switch must be set to 3."**

That VSB is widely regarded as half-working: SB 2.0 only (8-bit mono, single low DMA, no SB16, no stereo, no high DMA), poor/limited FM, glitchy MIDI timing, and zero observability. This is the same gap VDMSound and SoundFX 2000 filled. We supersede it cleanly (see next section) and do FM + MIDI properly.

The useful mental model is **control plane vs data plane**:

* **MIDI (MPU-401)** is pure pass-through — the bytes are already host-consumable; reassemble them and forward to `midiOutShortMsg`/`midiOutLongMsg`. Control plane only.
* **FM (OPL2/OPL3)** has **no data plane at all** — the chip synthesises waveforms from register writes, so the host has nothing to forward. We must software-emulate the chip (Nuked OPL3), render PCM continuously, and stream it to DirectSound. This is the hard part of the MVP.
* **Sound Blaster digital (DSP + DMA)** is hybrid — interpret the DSP command stream, pull PCM via `VDDRequestDMA`, push to DirectSound. **Post-MVP.**

## How we supersede the built-in VSB (verified from source)

This was the project's biggest risk; it is now resolved. From the `ntvdmx64` reconstruction of the NTVDM startup path (`nt_msscs.c`), the order at process start is **verified**:

1. `SetupInstallableVDD()` runs first — it reads `HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers` value `VDD` (REG_MULTI_SZ) and `LoadLibrary`s each registered VDD, running its `DllMain(DLL_PROCESS_ATTACH)`.
2. **Then** `SbInitialize()` runs and installs the built-in VSB's I/O hooks.

And `SbInitialize()` is coded to **lose gracefully**: if its hook install fails (because a port is already owned), it rolls back and attaches to nothing —

```c
if (!InstallIoHook()) { dprintf2(("*** failed to install IO Hooks!!!")); return FALSE; }
/* ConnectPorts: on a failed io_connect_port it unwinds every port it took and returns FALSE */
```

**Therefore: a statically-registered VDD that calls `VDDInstallIOHook` on the sound ports in its `DllMain` runs *before* the VSB and takes ownership; the VSB then finds the ports taken and disables itself.** No `SET BLASTER=A0`, no launcher, no separate VDM session. This is exactly the "load a 16-bit app → you get our VSB" experience.

Implementation requirements that follow:

* **Hook the full ranges**, not just the sub-ports the VSB uses. The VSB only claims DSP/mixer sub-ports (`0x224–0x22E`), OPL (`0x388–0x389`), and MPU (`0x330–0x331`). To guarantee the VSB fully backs off and to own everything a game probes, hook `0x220–0x22F`, `0x330–0x331`, `0x388–0x389` in their entirety.
* **The game's `BLASTER` detection still works.** Because we own the ports and answer detection, and `BLASTER` is read later by the DOS side (in `command.com`, after VDD `DllMain`), the default `autoexec.nt` `BLASTER` line can stay — it now describes *our* card.
* **Fallback if a future XP revision changes the order:** the deterministic escape hatch is still `SET BLASTER=A0`, which makes the VSB call `SbTerminate()` and remove its hooks. Document it in the README troubleshooting, but we do **not** depend on it.
* **WOW note:** under WOW (16-bit *Windows* apps) `SbInitialize` returns early and never hooks — irrelevant for our DOS target but good to know.

## MVP scope (what "done" means)

1. A working cross-build producing `vddsound.dll` from a **macOS host** with MinGW-w64 (i686), driven by one script, with **no non-system DLL dependencies**.
2. **Automatic supersession:** registered once in the registry, the VDD loads into every NTVDM, hooks the sound ports in `DllMain`, and the built-in VSB backs off. Verified by the trace log showing our handlers firing.
3. **FM/OPL audio:** trap OPL2/OPL3 register writes (`0x388`/`0x389` AdLib, and the SB-aliased `0x220`/`0x221`/`0x228`/`0x229`), drive a **Nuked OPL3** core, and stream its PCM to the host via a **DirectSound** looping ring buffer on a dedicated render thread. Pass the standard AdLib timer-based detection so games find FM.
4. **MIDI pass-through** for MPU-401 UART mode (`0x330` data, `0x331` command/status) → `midiOutShortMsg`/`midiOutLongMsg` via `winmm`. The `0x331` status register returns `0xBF` (ready; active-low — see "MPU-401 register semantics").
5. **Sound Blaster detection stub** so autodetecting games see a card and to firmly own the SB ports (push `0xAA` on reset; answer the `0xE1` version query). **No digital PCM/DMA playback in MVP.**
6. A trace logger timestamping every trapped port I/O on `0x220–0x22F`, `0x330–0x331`, `0x388–0x389` with the guest's `CS:IP` and the byte/word value, to a configurable file.
7. README + ARCHITECTURE docs: install, smoke test, troubleshooting, internals.

Out of scope for MVP, post-MVP in this order: SB16 DSP digital playback (DSP command FSM + `VDDRequestDMA` + IRQ via `call_ica_hw_interrupt`); the mixer chip; stereo/high-speed modes; CMS/Game Blaster; a **Control Panel applet (`.cpl`)** for configuration (device selection, OPL core, log path, latency).

## Hard constraints

* Target: **Windows XP SP3, 32-bit only.** NTVDM does not exist on x64 Windows.
* Architecture: **i686** (32-bit x86).
* Toolchain: **MinGW-w64 i686** on **macOS** via Homebrew (`brew install mingw-w64`, formula 14.0.0). No Windows DDK / WDK. No MSVC. Prefix `i686-w64-mingw32-` (the formula builds both `i686` and `x86_64` toolchains and symlinks all binaries into `$(brew --prefix)/bin`). Works on Apple Silicon (`/opt/homebrew`) and Intel (`/usr/local`); only the prefix path differs.
* The VDD links its NTVDM imports against `ntvdm.exe` directly. Generate the import library from a hand-written `.def` using `i686-w64-mingw32-dlltool`. The `.def` `LIBRARY` name must be exactly `ntvdm.exe`, lowercase, with the extension.
* Minimal CRT. Static-link what's needed (`-static-libgcc`; `-static`/`-static-libstdc++` if the OPL core's TU pulls C++ in) so the DLL has **no non-system DLL dependencies**. Verify with `i686-w64-mingw32-objdump -p build/vddsound.dll | grep "DLL Name"` — expect only `ntvdm.exe`, `WINMM.DLL`, `dsound.dll`, `ole32.dll`, `KERNEL32.dll` (+ `ADVAPI32.dll` if reading the registry/env).
* **You cannot test on XP from the build host.** Validation is the user's responsibility on a disposable, snapshotted XP VM. Code defensively, instrument heavily, write the README so the user can isolate failures.

## `.def` for the ntvdm.exe import (VALIDATED against `vdm/ntvdm.exe`)

`VDDSimulateInterrupt` and `GetVDMPointer` are **macros** in `vddsvc.h` (→ `call_ica_hw_interrupt` and `MGetVdmPointer` respectively), so the *imported* symbols are the macro targets. The register accessors are real `WINAPI` functions exported **undecorated**.

```
LIBRARY ntvdm.exe
EXPORTS
  ; --- I/O hook + lifecycle (MVP) ---
  VDDInstallIOHook
  VDDDeInstallIOHook
  ; --- guest register accessors for CS:IP logging (MVP) ---
  getCS
  getIP
  getEIP
  getDS
  getES
  getSS
  getAX
  getBX
  getCX
  getDX
  ; --- VM services (macros in vddsvc.h resolve to these real symbols) ---
  MGetVdmPointer         ; GetVDMPointer / Sim32pGetVDMPointer -> MGetVdmPointer
  call_ica_hw_interrupt  ; VDDSimulateInterrupt -> call_ica_hw_interrupt (post-MVP IRQ)
  ; --- DMA + IRQ (post-MVP SB digital path; declared now to freeze the import lib) ---
  VDDRequestDMA
  VDDQueryDMA
  VDDSetDMA
  VDDReserveIrqLine
  VDDReleaseIrqLine
```

Generate at build time (use `-k` to strip any `@N` stdcall decoration so bare names match the binary's undecorated exports), then sanity-check:

```
i686-w64-mingw32-dlltool -k -d defs/ntvdm.def -l libntvdm.a
i686-w64-mingw32-objdump -t libntvdm.a | head
```

**Validation status:** every symbol above is confirmed present and exported **undecorated** in `vdm/ntvdm.exe` (162 exports, ImageBase `0x0F000000`). Notes from the dump: `VDDSimulateInterrupt` is not an export (it's the macro → `call_ica_hw_interrupt`, present); the pointer helper is exported as `Sim32pGetVDMPointer` (we import `MGetVdmPointer` directly, so this is moot); `VDDReserveIrqLine`/`VDDReleaseIrqLine`/`VDDInstallMemoryHook` are available for the post-MVP IRQ/DMA path. No further on-target export verification is required — the binary is in `vdm/`.

## VDD ABI (get this exactly right or the VM dies)

Quoted from ReactOS `nt_vdd.h` / `vddsvc.h`, which mirror the Microsoft DDK. There is no DDK on the build host, so **hand-declare these in `common.h`**.

### `VDDInstallIOHook` and the struct layouts

```c
BOOL  WINAPI VDDInstallIOHook(HANDLE hVdd, WORD cPortRange,
                              PVDD_IO_PORTRANGE pPortRange,
                              PVDD_IO_HANDLERS  IoHandlers);
VOID  WINAPI VDDDeInstallIOHook(HANDLE hVdd, WORD cPortRange,
                                PVDD_IO_PORTRANGE pPortRange);

typedef struct _VDD_IO_PORTRANGE {        /* ranges are INCLUSIVE: First..Last */
  WORD First;
  WORD Last;
} VDD_IO_PORTRANGE, *PVDD_IO_PORTRANGE;

typedef struct _VDD_IO_HANDLERS {         /* field ORDER is load-bearing */
  PFNVDD_INB   inb_handler;
  PFNVDD_INW   inw_handler;
  PFNVDD_INSB  insb_handler;
  PFNVDD_INSW  insw_handler;
  PFNVDD_OUTB  outb_handler;
  PFNVDD_OUTW  outw_handler;
  PFNVDD_OUTSB outsb_handler;
  PFNVDD_OUTSW outsw_handler;
} VDD_IO_HANDLERS, *PVDD_IO_HANDLERS;
```

### Handler prototypes — **all `WINAPI` (`__stdcall`), return `VOID`**

A wrong calling convention corrupts the stack and takes the whole VM down on the first port access. Non-negotiable.

```c
typedef VOID (WINAPI *PFNVDD_INB)  (WORD iport, PBYTE data);          /* write read-value into *data */
typedef VOID (WINAPI *PFNVDD_INW)  (WORD iport, PWORD data);
typedef VOID (WINAPI *PFNVDD_INSB) (WORD iport, PBYTE data, WORD count);
typedef VOID (WINAPI *PFNVDD_INSW) (WORD iport, PWORD data, WORD count);
typedef VOID (WINAPI *PFNVDD_OUTB) (WORD iport, BYTE data);           /* data is the value written */
typedef VOID (WINAPI *PFNVDD_OUTW) (WORD iport, WORD data);
typedef VOID (WINAPI *PFNVDD_OUTSB)(WORD iport, PBYTE data, WORD count);
typedef VOID (WINAPI *PFNVDD_OUTSW)(WORD iport, PWORD data, WORD count);
```

* `iport` is the **absolute** port number (not an offset) — switch on it.
* IN handlers receive a pointer to write the result into; OUT handlers receive the value.
* At minimum supply byte-in and byte-out. **You may pass `NULL` for the word/string slots — NTVDM auto-emulates them via the byte handlers.** For MVP, supply `inb`/`outb` and `NULL` the other six.

### `hVdd` for a statically-registered VDD

**Pass the `HINSTANCE` from `DllMain`'s first parameter (`hinstDLL`).** ReactOS's static sample stores `hVdd = hInstanceDll` and passes it; `io.c` treats `hVdd` as an opaque token, rejecting only `NULL`/`INVALID_HANDLE_VALUE`. **Do not call `RegisterModule`** (that is the dynamic DOS-app BOP path). Use the same handle for `VDDDeInstallIOHook` on detach.

### Reading guest registers (CS:IP capture)

Real functions exported by `ntvdm.exe`, `WINAPI`/`__stdcall`, undecorated names. Inside any handler:

```c
USHORT WINAPI getCS(VOID);
USHORT WINAPI getIP(VOID);
ULONG  WINAPI getEIP(VOID);
/* full family present in the binary: getEAX/getAX/getAH/getAL, getDS/getES/getSS,
   getSP/getBP/getSI/getDI, getMSW, getEFLAGS, plus set* — declare what you log. */
```

Capture `getCS()`/`getIP()` for the log line.

## OPL/FM emulation (the MVP's hard part)

The host has no FM chip, so we synthesise. **Use the Nuked OPL3 core** (`nukeykt/Nuked-OPL3`): pure C (no C++ wrapper, no `-static-libstdc++`), two files (`opl3.c`/`opl3.h`), cycle-accurate, built-in resampler, outputs **stereo interleaved int16**, and has a proven Windows-DLL precedent in `WinOPL3Driver` (incl. 32-bit MinGW XP builds). One OPL3 core covers OPL2 games too (YMF262 is backward-compatible with YM3812).

* **License:** Nuked OPL3 is **LGPL-2.1+**. Keep it in a separable object/static-lib and document relinkability (or, if a fully permissive license is preferred for distribution, swap in **ymfm**, BSD-3, behind an `extern "C"` boundary with `-static-libstdc++`). **Do not use dbopl/MAME** (GPL would force the whole DLL to GPL).

API (verbatim from `opl3.h`):

```c
void OPL3_Reset(opl3_chip *chip, uint32_t samplerate);         /* e.g. 48000; resamples from 49716 internally */
void OPL3_WriteReg(opl3_chip *chip, uint16_t reg, uint8_t v);  /* reg>=0x100 selects OPL3 bank 1 */
void OPL3_WriteRegBuffered(opl3_chip *chip, uint16_t reg, uint8_t v);
void OPL3_GenerateStream(opl3_chip *chip, int16_t *sndptr, uint32_t numsamples); /* interleaved stereo */
```

Port → register mapping (two-step address/data protocol; latch the address write, apply on the data write):

* AdLib / OPL2: `0x388` = address, `0x389` = data → bank 0 (`reg < 0x100`).
* OPL3 second bank: `0x38A` = address, `0x38B` = data → bank 1 (`reg |= 0x100`).
* SB-aliased OPL: `0x220`/`0x228` = address, `0x221`/`0x229` = data (Pro/16 map FM into the SB range).

Rendering / clocking: the chip is stateful — render a continuous stream at the output rate; guest register writes just mutate state. **Sample-accurate timestamping is not required for music.** Push each guest OPL write `(reg, value)` into a **lock-free single-producer/single-consumer queue** from the I/O hook (VDM CPU thread); the render thread drains the queue and applies writes at the start of each block (optionally flush samples-so-far on a data-port write if timing artifacts appear, à la DOSBox `FillUp`). Block size 512–1024 frames (~10–20 ms at 48 kHz).

AdLib **detection** the emulation must pass (reads of status port `0x388` return: bit7 = IRQ, bit6 = Timer1 expired, bit5 = Timer2 expired; mask `0xE0`, other bits undefined):

1. write `0x60`→reg `0x04` (mask timers); 2. write `0x80`→reg `0x04` (reset IRQ); 3. read `0x388` → expect `0x00`; 4. write `0xFF`→reg `0x02` (Timer1 preset); 5. write `0x21`→reg `0x04` (start Timer1); 6. guest delays ~80 µs; 7. read `0x388` → expect `0xC0`. Drive the status byte from the core's timer state (or fake the `0x00`→`0xC0` transition keyed on this write sequence). Some SB-compatibles don't pass this method, so games have fallbacks; getting the canonical transition right covers AdLib detect.

## Audio output (DirectSound ring buffer + render thread)

XP has no WASAPI; use **DirectSound** (VDMSound's choice). Stream PCM through one looping secondary buffer, refreshed behind the play cursor by a dedicated render thread. VDMSound used a conservative ~1.5 s buffer; we target ~40–80 ms.

* **Format:** PCM 48000 Hz, 16-bit, stereo (`nBlockAlign=4`).
* **Setup:** `CoInitialize(NULL)` → `DirectSoundCreate(NULL,&lpDS,NULL)` → `SetCooperativeLevel(hWnd, DSSCL_PRIORITY)` → `CreateSoundBuffer` with `DSBUFFERDESC{ dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS, dwBufferBytes = NUM_CHUNKS*CHUNK_BYTES }` (e.g. 10 ms chunk = 1920 B at 48k/16/stereo, 4–8 chunks) → lock-whole/write-silence/unlock → `Play(0,0,DSBPLAY_LOOPING)`. `DSBCAPS_GLOBALFOCUS` keeps audio playing when the DOS box loses focus.
* **Render loop (dedicated `CreateThread`/`_beginthreadex`, raised priority):** `GetCurrentPosition(&play,&write)`; when a chunk frees up, drain the OPL write queue, `OPL3_GenerateStream` into a scratch buffer (later: mix SB PCM), `Lock`/`CopyMemory`(handle the 2-pointer wrap)/`Unlock` ahead of the play cursor, advance your own write pointer; `Sleep(1)` between passes (polling, like VDMSound and SDL2). On `DSERR_BUFFERLOST`, call `Restore()` and re-lock.
* **Threading rules:** plain Win32 thread (VDMSound does this). **The render thread must stay host-side only — never call `VDDSimulate16` or other engine APIs that assume the VDM CPU-thread context.** Do **not** use the internal `host_CreateThread`/`cpu_createthread` exports. Hand-off from I/O hook (CPU thread) to render thread is SPSC; keep any hook-side critical section to a couple of instructions so guest execution never stalls.
* **Mixing (future SB PCM + FM):** sum into `int32` (or float), clamp to `[-32768, 32767]`, store int16 — never sum directly into int16 (wrap-around is the worst distortion).
* **MinGW link:** `-ldsound -ldxguid -luuid -lole32 -lwinmm`. In C, call COM via the header macros (`IDirectSound_CreateSoundBuffer(p,…)` → `(p)->lpVtbl->...`). `dsound.h`/`libdsound.a` are present in the Homebrew i686 sysroot; GUIDs (`IID_IDirectSoundNotify`, etc.) live in `libdxguid`.

## MPU-401 register semantics (verified against VDMSound)

Ports: `0x330` = data (read inbound / write outbound MIDI), `0x331` = command on write / status on read.

**Status byte (read of 0x331) — both bits ACTIVE LOW** (a clear bit means satisfied):

* bit 7 (`0x80`) DSR — clear ⇒ a byte is waiting for the guest to read from 0x330.
* bit 6 (`0x40`) DRR — clear ⇒ the MPU is ready to accept a write.
* bits 0–5 pad, always set (`0x3F`).

`MK_MPU_STATUS(dsr,drr) = (dsr?0:0x80) | (drr?0:0x40) | 0x3F`. For MVP (always ready, nothing inbound) **return `0xBF`** on IN from 0x331. Games spin waiting for bit `0x40` to read 0 before each write — return it set and the game hangs.

**UART reset/enter** (writes to command port 0x331): `0xFF` (reset) and `0x3F` (enter UART) each **queue `0xFE` (ACK)** to be read from data port 0x330. VDMSound also fakes `0xFE` for unrecognised commands.

## MIDI byte-stream reassembly (0x330 → winmm)

Per incoming `OUT 0x330` byte, in this order:

1. **Real-time (`0xF8`–`0xFF`):** dispatch immediately and return — do **not** touch running status or the partial buffer (these interleave mid-message).
2. **Status byte (top bit set):** if inside SysEx, terminate/flush it. Otherwise set `currentStatus = byte` and clear the partial buffer.
3. **Data byte:** append; if no `currentStatus`, it's running status reusing the previous one.
4. **Completion:** when `buffered + 1 == length[currentStatus]`, dispatch and clear the buffer but **keep `currentStatus`** (running status).

Total message lengths including the status byte (the `length[]` table):

| Status range | Message | Total len |
|---|---|---|
| `0x80`–`0xBF` | Note Off / Note On / Aftertouch / Control Change | 3 |
| `0xC0`–`0xCF` | Program Change | **2** |
| `0xD0`–`0xDF` | Channel Aftertouch | **2** |
| `0xE0`–`0xEF` | Pitch Bend | 3 |
| `0xF1` MTC, `0xF3` Song Select | | 2 |
| `0xF2` Song Position | | 3 |
| `0xF6` Tune, `0xF7` EOX, `0xF8`–`0xFF` | | 1 |

* Channel-voice / system-common → `midiOutShortMsg`: `dwData = status | (data1<<8) | (data2<<16)`.
* SysEx (`0xF0`…`0xF7`) → `midiOutLongMsg`: accumulate the whole block (framing bytes included), `midiOutPrepareHeader`, then `midiOutLongMsg`. **The buffer + `MIDIHDR` must outlive the async playback — free only after the `MOM_DONE` callback** (VDMSound runs a small GC thread). Freeing early is a VM-killing use-after-free.

## Sound Blaster detection (MVP stub — own the ports + answer detection, no PCM)

We hook the full SB range so the built-in VSB backs off and probing games find a card. Port map at base `0x220` (offsets switched on `iport - base`):

| Port | Read | Write |
|---|---|---|
| `0x226` | — | DSP **reset** |
| `0x22A` | DSP **read data** | — |
| `0x22C` | DSP **write-buffer status** | DSP **write command/data** |
| `0x22E` | DSP **read-buffer status** | — |

Status polarity (both use bit 7 = `0x80`, opposite meanings): read-buffer (0x22E) bit7 set ⇒ data available (idle `0x7F`, `0xFF` when queued); write-buffer (0x22C) bit7 set ⇒ busy (always-ready `0x7F`). Minimum handshake: on reset (`1` then `0` to `0x226`) **queue `0xAA`** for the next read of `0x22A`; on `0xE1` (write to `0x22C`) return two bytes from `0x22A` = major then minor. **No DMA / no DSP playback commands in MVP.**

## Proposed file layout

```
vddsound/
  CMakeLists.txt
  cmake/toolchain-mingw-i686.cmake
  src/
    vdd_entry.c       DllMain, hVdd capture, VDDInstallIOHook for all ranges, thread start/stop
    midi_module.c/.h  0x330/0x331 handlers, winmm bridge, running-status reassembly, SysEx GC
    opl_module.c/.h   0x388/0x389 + SB-aliased FM handlers, register latch, AdLib detect, write queue
    sb_module.c/.h    SB detection stub (MVP); DSP/DMA state machine (post-MVP)
    audio_out.c/.h    DirectSound ring buffer + render thread, OPL render, mixer
    opl3.c/opl3.h     vendored Nuked OPL3 core (LGPL-2.1+; keep separable for relinkability)
    logger.c/.h       trace file writer with CS:IP capture
    common.h          hand-declared VDD ABI (structs, WINAPI handler typedefs, getXX protos)
  defs/ntvdm.def
  docs/README.md
  docs/ARCHITECTURE.md
  scripts/build.sh
  (post-MVP) cpl/vddsound.cpl   Control Panel applet
```

## Build on macOS (verified toolchain)

```
brew install mingw-w64      # formula 14.0.0; ships i686-w64-mingw32-* and x86_64-w64-mingw32-*
```

`cmake/toolchain-mingw-i686.cmake`:

```cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(TARGET_TRIPLE i686-w64-mingw32)
execute_process(COMMAND brew --prefix mingw-w64
                OUTPUT_VARIABLE MINGW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_C_COMPILER   ${TARGET_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${TARGET_TRIPLE}-g++)
set(CMAKE_RC_COMPILER  ${TARGET_TRIPLE}-windres)
set(CMAKE_DLLTOOL      ${TARGET_TRIPLE}-dlltool)
set(CMAKE_FIND_ROOT_PATH "${MINGW_PREFIX}/toolchain-i686/${TARGET_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

> Verify the sysroot once with `i686-w64-mingw32-gcc -print-sysroot`.

DLL specifics:

* `add_library(vddsound SHARED <sources>)`; `set_target_properties(vddsound PROPERTIES PREFIX "" SUFFIX ".dll")`. No `EXPORTS` needed — NTVDM loads by registered path and calls `DllMain`; hooks register at runtime. So `defs/` holds only `ntvdm.def`.
* `WINDOWS_EXPORT_ALL_SYMBOLS` does **not** work under MinGW — irrelevant here since we export nothing.
* Link: generated `libntvdm.a` + `winmm dsound dxguid uuid ole32`.
* Static-link the runtime: `target_link_options(vddsound PRIVATE -static-libgcc -static)` (Nuked is C, so `-static-libstdc++` only needed if you switch to a C++ OPL core). Confirm zero stray deps with the `objdump -p` check.

`scripts/build.sh`: ensure toolchain → `dlltool -k` the import lib → `cmake -B build -DCMAKE_TOOLCHAIN_FILE=…` → `cmake --build build` → `objdump -p` dependency check. Output: `build/vddsound.dll`.

## VDD lifecycle

* Registered statically via `HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers`, value `VDD` (REG_MULTI_SZ), one full DLL path per line. NTVDM loads every listed DLL at startup (before the built-in VSB inits — see supersession).
* `DllMain` on `DLL_PROCESS_ATTACH`: **save `hinstDLL` to a module-global `HANDLE hVdd`**; open the log; `VDDInstallIOHook` for `0x220–0x22F`, `0x330–0x331`, `0x388–0x389` (this is what makes the VSB back off — do it here, early); `OPL3_Reset`; `CoInitialize` + DirectSound setup; `midiOutOpen` the default device; start the render thread. Keep init lean and **defensive — a crash here takes the whole VM down**; if DirectSound or winmm fail, log and degrade (e.g. MIDI still works without FM), don't abort the process. Do **not** call `RegisterModule`.
* `DLL_PROCESS_DETACH`: stop the render thread, `Stop`/release the DirectSound buffer, `CoUninitialize`, `midiOutReset`+`midiOutClose`, `VDDDeInstallIOHook(hVdd,…)` for each range, flush+close the log.
* Do not assume the CRT is fully initialised as in a desktop app. Beyond the documented VDD APIs, the only sanctioned IRQ-injection path (post-MVP) is `call_ica_hw_interrupt` (the `VDDSimulateInterrupt` macro).

## DOS-side environment

`autoexec.nt` (at `%SystemRoot%\system32\autoexec.nt`) advertises the card to games. Because we own the ports and the VSB backs off, the existing default works; for an OPL3-class card advertise SB Pro/16:

```
SET BLASTER=A220 I5 D1 H5 P330 T6
SET MIDI=SYNTH:1 MAP:E MODE:0
```

(`T6` = SB16/AWE; `H5` = high DMA, used only by the post-MVP digital path. `T4` would advertise SB Pro/OPL3 if you prefer an 8-bit-only profile. The MVP only needs the FM/MPU bases to be correct.) The README must note that the kill-switch `SET BLASTER=A0` exists as a fallback to force the built-in VSB off, but is not normally required.

## Code style (non-negotiable)

* Plain C for everything except modules embedding C++ libraries (only if a C++ OPL core is chosen); use `extern "C"` at the boundary. Nuked OPL3 is C, so the MVP stays pure C.
* **Explicit types everywhere.** No `auto` in any C++; no `__auto_type` in C.
* **No leading-underscore prefixes** on fields/locals/globals (reserved by the C standard in many contexts; also house style). The vendored `opl3.c`/`opl3.h` are exempt as third-party code.
* Default to `static` linkage and `const` data. TU-private functions are `static`; headers expose only what other modules need.
* American English in code/comments; British English in user-facing README prose.
* No global mutable state outside an explicit, documented per-module state struct.
* Clean under `-Wall -Wextra -Wpedantic` for our code (the vendored OPL core may be built with relaxed warnings). Warnings are errors in CI.

## Logger format

Plain text, one event per line, microsecond-resolution timestamp from `QueryPerformanceCounter`. Example:

```
1716000000.123456  IN  port=0x331 size=1 value=0xC0 cs:ip=0xF000:0x1234
1716000000.123512  OUT port=0x388 size=1 value=0x20 cs:ip=0xF000:0x1238
```

Log path configurable via env var `VDDSOUND_LOG`, default `C:\vddsound\trace.log`. The logger is the primary forensic deliverable; treat it accordingly. Logging on the I/O hook path must be cheap (format into a fixed buffer; avoid heap churn) so it doesn't perturb guest timing.

## Smoke test (document verbatim in README)

1. Build on macOS: `./scripts/build.sh`. Output: `build/vddsound.dll`.
2. Copy to `C:\vddsound\vddsound.dll` on the XP VM. **Snapshot the VM first.**
3. `regedit` → `HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers` → edit `VDD` (REG_MULTI_SZ) → add a line `C:\vddsound\vddsound.dll` (keep existing lines).
4. Ensure `C:\WINDOWS\system32\autoexec.nt` has the `SET BLASTER=` and `SET MIDI=` lines above.
5. Reboot the VM.
6. **Supersession check:** run any DOS app that uses sound; inspect `C:\vddsound\trace.log` for our handlers firing on `0x388`/`0x330` with sensible `CS:IP`. (Confirms we won the ports and the VSB backed off.)
7. **FM check:** run a known AdLib/OPL game (or `TESTOPL`-style tool). Expected: FM music audible via the host, no stuck/garbled notes.
8. **MIDI check:** run a DOS MPU-401 MIDI player against a `.mid`. Expected: MIDI audible via the host's default MIDI output; trace shows 0x330 writes.
9. Troubleshooting in README: DLL not loading (registry path exact, no typos); no log (check `VDDSOUND_LOG` and folder permissions); no FM audio (check default DirectSound device, buffer-lost loop); no MIDI (check default MIDI device in Control Panel → Sounds); game says "no sound card" (check `BLASTER`/`MIDI` env via `set` inside `command.com`); **still hearing the old crackly VSB** (our hooks didn't install first — confirm registry entry, and as a fallback set `SET BLASTER=A0` then re-add a valid per-app `BLASTER`).

## Deliverables checklist

* [ ] `vddsound.dll` from `scripts/build.sh` on a clean macOS + Homebrew MinGW-w64 environment, reproducible from a fresh checkout, **no non-system DLL dependencies** (`objdump -p`).
* [ ] Automatic supersession of the built-in VSB (trace log proves our handlers fire).
* [ ] FM/OPL audible via DirectSound; passes AdLib detection.
* [ ] MPU-401 MIDI pass-through audible via winmm.
* [ ] `docs/README.md` (install, smoke test, troubleshooting) and `docs/ARCHITECTURE.md` (VDD API surface, control/data plane, supersession mechanism, threading model, post-MVP roadmap incl. the `.cpl` applet).
* [ ] Clean build under `-Wall -Wextra -Wpedantic` (our code).
* [ ] Third-party license compliance documented (Nuked OPL3 LGPL relinkability, or ymfm BSD notice).

## Known landmines

* **Calling convention.** Every I/O handler and every `getXX`/VDD API is `WINAPI` (`__stdcall`). One `__cdecl` handler corrupts the stack on the first port hit and the VM dies. Declare them all `WINAPI` in `common.h`.
* **`hVdd` must be the DllMain `HINSTANCE`, never `NULL`.** `VDDInstallIOHook` rejects `NULL`; passing it means hooks silently never install (and the VSB keeps the ports). Do not route through `RegisterModule`.
* **Hook in `DllMain`, and hook the full ranges.** Supersession depends on owning the ports *before* `SbInitialize` runs. Install during `DLL_PROCESS_ATTACH`; cover `0x220–0x22F` entirely (not just the VSB's sub-ports) so the VSB's `ConnectPorts` fails and it fully backs off.
* **Symbol decoration.** `ntvdm.exe` exports `getXX`/VDD functions **undecorated**. If `dlltool` emits `_getCS@0`-style names the link fails — use `-k`, verify with `objdump -t`. (Already validated against `vdm/ntvdm.exe`.)
* **Thread safety.** OPL register writes arrive on the VDM CPU thread (I/O hook); the render thread reads synth state. Use a lock-free SPSC queue; never block in the hook; the render thread must never call VM/engine APIs.
* **DirectSound buffer-lost.** Always handle `DSERR_BUFFERLOST` (`Restore()` + re-lock) or audio dies on focus changes. Create with `DSBCAPS_GLOBALFOCUS` so the DOS box keeps playing in the background.
* **SysEx lifetime.** Buffers passed to `midiOutLongMsg` must survive until `MOM_DONE` — freeing early is a VM-killing use-after-free.
* **MPU status polarity.** Bits are active-low; return `0xBF`. A value with bit `0x40` set hangs the game.
* **DllMain runs under the loader lock — do NOT init DirectSound/winmm/threads there.** `DirectSoundCreate` and `midiOutOpen` `LoadLibrary` their own dependencies; calling them in `DLL_PROCESS_ATTACH` deadlocks the loader lock and hangs NTVDM at startup (blank window, must force-close, empty log because no I/O ever runs). `DllMain` must do only loader-lock-safe work: capture `hVdd`, open the log, pure-memory init, and `VDDInstallIOHook`. Start the audio/MIDI runtime lazily on the first trapped port access (which runs on the VDM thread after the loader lock is released). *(Hit and fixed during bring-up — see [[ntvdm-vdd-dllmain-loader-lock]].)*
* **In-process crash = dead VM.** Validate everything from `GetVDMPointer`; treat the VM as hostile input. The VDM I/O path is the same kernel transition CVE-2010-0232 exploited — bugs in hooks have history. Snapshot the XP VM before every test.
* **Licensing.** A statically linked DLL is a derivative of its OPL core: dbopl/MAME (GPL) would force the whole DLL GPL — excluded. Nuked OPL3 (LGPL) requires relinkability; ymfm (BSD) just needs the notice.

## When to stop

MVP is done when: the build is reproducible from a clean checkout with one script and has no non-system DLL deps; registering the DLL makes every NTVDM use our VSB (the built-in one backs off, proven by the trace log); a known AdLib game's FM music plays cleanly via DirectSound; an MPU-401 MIDI player is audible via winmm; and the README's smoke test passes end-to-end on a clean XP SP3 VM. **Do not start the SB16 digital DSP/DMA path, the mixer, or the Control Panel applet until MVP is signed off.** The SB DMA + IRQ timing path in particular needs real-hardware iteration against real DOS games, not speculative coding.
