# vddsound — Project Status & Rehydration Guide

**This is the single source of truth for the current state.** `docs/ARCHITECTURE.md` describes the design/internals; the root `README.md` is the front page. Where they disagree with reality, **this file wins.**

Last build tag: **`[b33-wavdump]`**. Target: Windows XP SP3 32-bit NTVDM. Built on macOS with Homebrew MinGW-w64 i686, CRT-free, no XP-incompatible deps.

---

## What it is

A statically-registered NTVDM Virtual Device Driver (`vddsound.dll`) that takes over the legacy sound ports and routes DOS audio to the host:
- **MIDI** (MPU-401, `0x330/0x331`) → `midiOutShortMsg`/`midiOutLongMsg` (winmm).
- **FM** (OPL2/3, `0x388-0x38B`) → **Nuked OPL3** software synth → **DirectSound**.
- **SB DSP** (`0x220-0x22F`) → detection (reset→`0xAA`, version `0xE1`→`04 05`) **+ digital DMA playback** (`[b22-dmadirect]`): DSP command FSM → read guest PCM directly from memory (`VDDQueryDMA` + `MGetVdmPointer`, since `VDDRequestDMA` returns 0 on this NTVDM) → snapshot buffer → resample → mix into the FM/DirectSound buffer → IRQ5 via `call_ica_hw_interrupt`. **8-bit plays the correct sounds on the VM (mild residual resampler crackle); 16-bit not yet.**
- **Forensic logger** with guest CS:IP, off by default (set `VDDSOUND_TRACE=1`).

## What WORKS (verified on real XP SP3)
- Reproducible macOS cross-build; DLL imports only ntvdm.exe/winmm/dsound/user32/kernel32.
- Loads into every NTVDM via the registry `VDD` list; `DllMain` runs; `VDDInstallUserHook` fires.
- **Owns all three port ranges** (SB+MPU+OPL) — handlers fire for `0x220`/`0x330`/`0x388` (proven with DOS `debug`).
- MIDI bytes reach winmm; FM register writes reach Nuked OPL3 → DirectSound; CS:IP capture works.
- **8-bit SB digital PCM plays the correct sounds** (`[b33-wavdump]`, Skyroads SFX): DSP FSM + direct guest-memory DMA read + per-transfer buffer snapshot + signedness auto-detect. Residual mild crackle = our crude linear resampler imaging the low-rate sources (6024/8000 Hz). 16-bit not yet.
- CPU-affinity pin to core 0 (in DllMain).

## What's BROKEN / unsolved
1. **Tempo/time distortion (the big one).** DOS audio plays time-distorted ("stretched cassette", in-tune, drifting). **This is NTVDM's emulated timebase, not our code** — it hits native NTVDM, VDMSound, and us identically. Root cause: NTVDM idle-detection + PIT/BIOS-tick inaccuracy. **Nothing fixed it:** affinity, BIOS SpeedStep off, PIF Idle-Sensitivity=Low, Compatible-Timer toggle, full-screen — all no effect. Overriding the BIOS tick (`0040:006C`) from a host thread made it **worse** (fights NTVDM's IRQ0). This is the genuine NTVDM floor; even VDMSound never fixed it (it's "smooth but slow").
2. **FM content** (Skyroads): some instruments missing / not quite right. **`[b18-oplbuf]` improved write timing** — drained register writes now go through Nuked's timestamped write buffer (`OPL3_WriteRegBuffered`), spread at hardware-accurate 2-sample spacing instead of all collapsing onto the block's first sample (worst on render-thread catch-up). **Not yet A/B'd on the VM.** OPL3 bank-1 (`0x38A/0x38B`) was already handled correctly in `opl_module.c` — the old "verify bank-1" suspicion was stale. Still open: true per-burst sample-accurate timing needs timestamps on the SPSC queue entries (the ~16 ms inter-burst quantization still stands); and SB Pro's OPL mirror at `0x220-0x223/0x228-9` isn't forwarded to the OPL core, so a game driving FM through those instead of `0x388` would lose it (Skyroads uses `0x388`, so not its cause).
3. **SB digital PCM/DMA**: **8-bit plays the correct sounds on the VM** (`[b33-wavdump]`, Skyroads). The path that got there (all verified by trace + ear):
   - `VDDRequestDMA` returns **0** on this NTVDM even on the VDM thread with the channel unmasked and a live count — abandoned it. Read guest PCM directly via **`VDDQueryDMA`** (layout `{addr,count,page,...}`, phys = `(page<<16)|addr`) + **`MGetVdmPointer`**.
   - **Snapshot** the whole buffer into private memory at `sb_start` (VDM thread) — the render thread reads ~384 ms ahead of the play cursor, and Skyroads re-fires the same buffer ~0.275 s apart, so reading guest memory live tore the audio. This was the gross crackle.
   - **Signedness auto-detect** from the buffer (`0x00`-dominated = signed): Skyroads sends *signed* data via the spec-*unsigned* `0x14`.
   - Underrun ruled out (`peak chunks/wake=1` after `timeBeginPeriod(1)` + TIME_CRITICAL render thread); clipping ruled out (halving PCM didn't help).
   - **ROOT CAUSE of the crackle (found b32-b33): we snapshot the buffer too early.** Resampling is fully exonerated — both our linear/cubic resampler AND DirectSound's own SRC (b32, a dedicated native-rate `IDirectSoundBuffer` per transfer via `audio_pcm_play` in `audio_out.c`) crackle identically. b33's `sb: wav` dump shows the buffer is **nearly empty at snapshot time**: only ~12 bytes written (around offset 458) of a 32100-byte transfer; the rest is zeros. The game **streams PCM into the buffer in real time and the DMA plays it just-in-time**; native NTVDM reads it **live**, so it always sees fresh data. Our `sb_start` snapshot captures an almost-empty buffer → ~12 bytes of sound then garbage → crackle.
   - **THE FIX (next session):** read the guest buffer **live at the play position with minimal look-ahead**, like the hardware DMA — not a one-shot snapshot at command time. Options: (a) a *streaming* looping DS buffer continuously refilled from the guest's current DMA region just behind its write position; or (b) a dedicated low-latency PCM mix that reads the guest buffer in real time (our 384 ms FM ring look-ahead is the thing that forced the snapshot and caused the original tearing, so PCM needs its own short-latency path). The b32 `audio_pcm_play` DS-buffer plumbing is a reusable starting point. Also: DOOM uses **auto-init** (`autoinit=1`, stereo, 11025 Hz) — needs per-block re-read, not snapshot replay.
   - Build state: `[b33-wavdump]`, uncommitted experiments include the DS-buffer path + a now-dead in-driver resampler in `sb_mix` (early-returns; remove on rework). `sb_module.c` carries diagnostic notes throughout.
   - **16-bit** (`sb_channel>=4` word-granular path) still unverified — needs a 16-bit trace.
4. **Glitchiness vs VDMSound smoothness**: was a small ~96 ms DirectSound ring buffer underrunning (VDMSound used ~1.5 s). **`[b17-bigbuf]` enlarged it to ~384 ms** (`audio_out.c`, `NUM_CHUNKS` 6→24) — applied in code, **not yet verified on the VM**. If glitches persist, bump `NUM_CHUNKS` further (32 ≈ 512 ms).

## Recommended next steps (ranked)
1. **Enlarge the DirectSound buffer** — **DONE in `[b17-bigbuf]`** (`audio_out.c`, `NUM_CHUNKS` 6→24 ≈ 384 ms). Pending on-VM confirmation it removes the FM glitching; headroom to ~512 ms (`NUM_CHUNKS` 32) if needed.
2. **Fix FM content** — **partly done in `[b18-oplbuf]`** (writes now via `OPL3_WriteRegBuffered`; bank-1 verified already-correct). Still open, needs on-VM A/B testing: per-burst sample-accurate timing (timestamp the SPSC queue entries + render in segments between writes) and optionally forwarding the SB Pro OPL mirror ports to the OPL core. (`opl_module.c`.)
3. **SB16 digital DSP+DMA** path — **8-bit plays correct sounds** (`[b33-wavdump]`). Remaining: (a) optional — better resampler to kill the residual imaging crackle (native-rate DirectSound buffer or a band-limited resampler), (b) 16-bit play (`VDDQueryDMA(ch5)` trace → fix word-granular addressing), (c) ADPCM / direct-DAC / mixer volumes. (Note: `sb_module.c` still carries per-transfer diagnostic notes — strip when stable.)
4. **Timebase takeover spike** (hook PIT `0x40-0x43` AND drive IRQ0, both from QPC, without fighting NTVDM). Only thing that might beat everyone on timing; high effort/fragile; may still lose. Treat as a research spike, not a commitment.

---

## Build & deploy loop
- **Build (macOS):** `./scripts/build.sh` — builds and PUBLISHES just two files, `vddsound.dll` + `install.bat`, into `./release/` (the VM shared folder), and prints the build tag.
- **Deploy (on the XP VM):** double-click `release/install.bat` (run as Administrator) — it kills the resident `ntvdm.exe` (which holds the old DLL open), copies the DLL beside it into `C:\vddsound\`, clears the log, and registers the VDD via `reg.exe`. **The first trace.log line shows the build tag** — if it's not the latest tag, you're running a stale DLL (this bit us repeatedly; always verify the tag).
- **VM setup:** none separate anymore — `install.bat` sets `HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers` value `VDD` (REG_MULTI_SZ) to `C:\vddsound\vddsound.dll` via `reg add` on every run (idempotent; needs Administrator). Registration takes effect on the next NTVDM launch; reboot only if the DLL still won't load. `autoexec.nt` currently has `SET BLASTER=A0` (no longer required — built-in VSB doesn't own the ports; real games want a valid `A220 I5 D1 P330 T6`).

## File / module map
```
docs/STATUS.md              THIS FILE — current truth + rehydration
docs/ARCHITECTURE.md        internals/design (design-level)
ntvdm/ntvdm.exe             real XP SP3 binary (5.1.2600.5512), git-ignored — RE offsets
ntvdm/details.txt           the VM's real autoexec.nt/config.nt
defs/ntvdm.def              import lib source (stdcall @N + dlltool -k → undecorated imports)
cmake/toolchain-mingw-i686.cmake
src/vdd_entry.c             DllMain, VDDInstallUserHook, single VDDInstallIOHook(3 ranges), unified dispatch, affinity
src/midi_module.c/.h        MPU-401 reassembly → winmm
src/opl_module.c/.h         OPL traps → Nuked OPL3 (SPSC queue), AdLib detect
src/audio_out.c/.h          DirectSound ring buffer + render thread
src/sb_module.c/.h          SB detection + first-pass digital DMA playback (DSP FSM, VDDRequestDMA, IRQ5)
src/logger.c/.h             CRT-free trace logger (VDDSOUND_TRACE gates per-access I/O)
src/crt0.c                  -nostdlib entry + mem* (XP has no UCRT)
src/opl3.c/.h               vendored Nuked OPL3 (LGPL-2.1+)
scripts/build.sh            build + publish to ./release
scripts/install.bat         on-VM register + swap (reg.exe VDD + self-copies DLL beside it)
release/                    shared-folder publish target (DLL + bat + user's test files)
```

## Hard-won technical findings (don't relearn these)
- **Call `VDDInstallIOHook` ONCE with an array of all ranges + a single unified handler** that dispatches by port. NTVDM registers an hVdd once (6-slot table); a 2nd call with the same hVdd returns `ERROR_ALREADY_EXISTS (183)`. Our long "the built-in owns OPL/MPU" saga was THIS bug (three separate calls), not real ownership. The built-in VSB does NOT exclusively hold the ports.
- **`hVdd` = the `DllMain` HINSTANCE.** Never NULL. Don't call `RegisterModule`.
- **DllMain runs under the loader lock** — do NOT init DirectSound/winmm/threads there (deadlocks NTVDM at startup). Init lazily on first trapped I/O (the `ensure_runtime()` pattern). I/O hooks themselves install fine from the `VDDInstallUserHook` create callback.
- **ntvdm.exe exports are undecorated**; our stdcall calls need `@N` in the `.def` + `dlltool -k` so the import name is undecorated but the link symbol is `name@N`.
- **CRT-free** (`-nostdlib` + `src/crt0.c`): XP has no UCRT `api-ms-win-crt-*.dll`.
- **Capture `GetLastError` immediately** after a Win32 call, before any logging call (logging resets LastError — this masked the real `183` for many rounds).
- Reverse-engineering ntvdm.exe: `pip3 install --break-system-packages capstone`; ImageBase `0x0F000000`; key RVAs found: VDDInstallIOHook `0x330A1`, hVdd-register `0x3300D` (the 183 source), io_disconnect_port `0x15DE5`, adapter tables `0x8A2C0`/`0x7A2A0` (NOT the real ownership; were a red herring).

## Memory files (auto-loaded next session) `project-vddsound`, `ntvdm-vsb-supersession`, `ntvdm-vdd-dllmain-loader-lock` in the session memory dir hold the condensed versions of the above.
