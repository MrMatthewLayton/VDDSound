# vddsound

A replacement **Virtual Sound Blaster** for Windows XP's NTVDM (the 16-bit DOS
subsystem). It's a 32-bit Virtual Device Driver (`vddsound.dll`) that loads into
every NTVDM, traps the legacy sound ports, and routes DOS audio to the host:

- **MIDI** (MPU-401, `0x330/0x331`) → Windows MIDI out (`winmm`)
- **FM** (OPL2/3, `0x388`) → software synthesis (Nuked OPL3) → DirectSound
- **Sound Blaster DSP** (`0x220`) → detection stub (digital PCM/DMA is future work)
- Forensic CS:IP trace logging (opt-in via `VDDSOUND_TRACE=1`)

Cross-built from **macOS** with MinGW-w64 (i686), CRT-free, depending only on
DLLs present on Windows XP.

## Status

Port ownership and FM/MIDI routing work on real XP SP3. The outstanding issue is
DOS tempo/time distortion, which is a limitation of NTVDM's emulated timebase
(it affects native NTVDM and VDMSound equally), not this driver.

👉 **See [`docs/STATUS.md`](docs/STATUS.md) for the authoritative state, the
build/deploy loop, the module map, and the engineering findings.** Start there.
`spec.md` is the original design brief.

## Build

```sh
./scripts/build.sh        # macOS + Homebrew mingw-w64; output: build/vddsound.dll
```

Install/test on a (snapshotted) XP SP3 VM — see `docs/README.md` and `docs/STATUS.md`.

## Local-only / not in this repo

- `vdm/` — the target machine's `ntvdm.exe`/`vdmdbg.dll`, used locally to
  reverse-engineer call offsets. These are **Microsoft binaries and are
  git-ignored**; supply your own from an XP SP3 install. The hard-coded RVAs in
  the source are specific to `ntvdm.exe` 5.1.2600.5512.
- `build/`, `vddsound/` — build output and the generated publish folder.

## Licensing

Original vddsound code: (project owner's choice — add a LICENSE).
Vendored `src/opl3.c` / `src/opl3.h` are **Nuked OPL3** © Nuke.YKT, **LGPL-2.1+**
(license headers retained in the files).
