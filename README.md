# vddsound

A replacement **Virtual Sound Blaster** for Windows XP's NTVDM (the 16-bit DOS
subsystem). `vddsound.dll` is a 32-bit Virtual Device Driver that loads into
every NTVDM, traps the legacy sound ports, and routes DOS audio to the host:

- **MIDI** (MPU-401, `0x330/0x331`) → Windows MIDI out (`winmm`)
- **FM** (OPL2/3, `0x388`) → software synthesis (Nuked OPL3) → DirectSound
- **Sound Blaster DSP** (`0x220`) → detection stub (digital PCM/DMA is future work)
- Forensic CS:IP trace logging (opt-in: `VDDSOUND_TRACE=1`)

Cross-built from **macOS** with MinGW-w64 (i686), CRT-free, depending only on
DLLs present on Windows XP (`ntvdm.exe`, `winmm`, `dsound`, `user32`, `kernel32`).

## Status

Port ownership and FM/MIDI routing work on real XP SP3. The outstanding issue is
DOS tempo/time distortion — a limitation of NTVDM's emulated timebase that
affects native NTVDM and VDMSound equally, not this driver.

## Documentation

- **[`docs/STATUS.md`](docs/STATUS.md)** — start here. Current state (what works /
  what's blocked), the build & deploy loop, the module map, next steps, and the
  hard-won engineering findings. The authoritative "where are we" doc.
- **[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** — how it works: VDD API
  surface, control/data-plane model, threading, and design rationale.

## Build & deploy

```sh
./scripts/build.sh
```

Builds `build/vddsound.dll` (Homebrew `mingw-w64`) and publishes the DLL +
`redeploy.bat` + `install.reg` into `./vddsound/` (a VM shared folder). On a
**snapshotted** XP SP3 VM, run `vddsound/redeploy.bat` (kills the resident
`ntvdm.exe`, copies the DLL into `C:\vddsound\`), register it once in the
registry (`install.reg`), reboot, and run a DOS program. Full details and the
stale-DLL gotcha are in `docs/STATUS.md`.

## Local-only (git-ignored, not in this repo)

- `vdm/` — the target machine's `ntvdm.exe` / `vdmdbg.dll`, used locally to
  reverse-engineer call offsets. **Microsoft binaries — do not commit.** Supply
  your own from an XP SP3 install. Hard-coded RVAs in the source target
  `ntvdm.exe` 5.1.2600.5512 (the DLL still builds without `vdm/` present).
- `build/`, `vddsound/` — build output and the generated publish folder.

## Licensing

- vddsound original code: add a `LICENSE` of your choice.
- `src/opl3.c` / `src/opl3.h` — **Nuked OPL3** © Nuke.YKT, **LGPL-2.1-or-later**
  (license headers retained).
