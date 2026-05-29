# vddsound — Project Status & Rehydration Guide

**This is the single source of truth for the current state.** `docs/ARCHITECTURE.md` describes the design/internals; the root `README.md` is the front page. Where they disagree with reality, **this file wins.**

Last build tag: **`[b16-baseline]`**. Target: Windows XP SP3 32-bit NTVDM. Built on macOS with Homebrew MinGW-w64 i686, CRT-free, no XP-incompatible deps.

---

## What it is

A statically-registered NTVDM Virtual Device Driver (`vddsound.dll`) that takes over the legacy sound ports and routes DOS audio to the host:
- **MIDI** (MPU-401, `0x330/0x331`) → `midiOutShortMsg`/`midiOutLongMsg` (winmm).
- **FM** (OPL2/3, `0x388-0x38B`) → **Nuked OPL3** software synth → **DirectSound**.
- **SB DSP** (`0x220-0x22F`) → detection stub only (reset→`0xAA`, version `0xE1`→`04 05`).
- **Forensic logger** with guest CS:IP, off by default (set `VDDSOUND_TRACE=1`).

## What WORKS (verified on real XP SP3)
- Reproducible macOS cross-build; DLL imports only ntvdm.exe/winmm/dsound/user32/kernel32.
- Loads into every NTVDM via the registry `VDD` list; `DllMain` runs; `VDDInstallUserHook` fires.
- **Owns all three port ranges** (SB+MPU+OPL) — handlers fire for `0x220`/`0x330`/`0x388` (proven with DOS `debug`).
- MIDI bytes reach winmm; FM register writes reach Nuked OPL3 → DirectSound; CS:IP capture works.
- CPU-affinity pin to core 0 (in DllMain).

## What's BROKEN / unsolved
1. **Tempo/time distortion (the big one).** DOS audio plays time-distorted ("stretched cassette", in-tune, drifting). **This is NTVDM's emulated timebase, not our code** — it hits native NTVDM, VDMSound, and us identically. Root cause: NTVDM idle-detection + PIT/BIOS-tick inaccuracy. **Nothing fixed it:** affinity, BIOS SpeedStep off, PIF Idle-Sensitivity=Low, Compatible-Timer toggle, full-screen — all no effect. Overriding the BIOS tick (`0040:006C`) from a host thread made it **worse** (fights NTVDM's IRQ0). This is the genuine NTVDM floor; even VDMSound never fixed it (it's "smooth but slow").
2. **FM content** (Skyroads): some instruments missing / not quite right — genuinely our OPL handling (register writes applied at audio-block granularity; OPL2/3 detail).
3. **SB digital PCM/DMA**: not implemented (detection stub only). PCM-using programs (Skyroads SFX, Duke3D, Doom menu) fail or hang the VM — expected, post-MVP.
4. **Glitchiness vs VDMSound smoothness**: likely our small ~96 ms DirectSound ring buffer underrunning (VDMSound used ~1.5 s). Untried fix.

## Recommended next steps (ranked)
1. **Enlarge the DirectSound buffer** (`audio_out.c`, `CHUNK_FRAMES`/`NUM_CHUNKS`, currently 768×6≈96 ms → try ~250–500 ms) to remove FM glitchiness. Cheap.
2. **Fix FM content** — apply OPL register writes more promptly (flush on data-port write), verify OPL3 bank-1 (`0x38A/0x38B`) handling. (`opl_module.c`.)
3. **SB16 digital DSP+DMA** path (`VDDRequestDMA`, IRQ via `call_ica_hw_interrupt`, mix PCM into the DirectSound buffer). The real differentiator; large.
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
src/sb_module.c/.h          SB detection stub (no PCM)
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
