# vddsound internals

## What it is

A statically-registered NTVDM Virtual Device Driver (VDD) — a 32-bit DLL loaded
into `ntvdm.exe` — that traps legacy sound-card port I/O from 16-bit DOS
programs and bridges it to the host Windows audio stack, superseding NTVDM's
built-in Virtual Sound Blaster (VSB).

## VDD API surface used

Imported from `ntvdm.exe` (see `defs/ntvdm.def`, validated against the real XP
SP3 binary). All are `__stdcall` and exported **undecorated**, so the import
library is generated with `dlltool --kill-at`.

- `VDDInstallIOHook` / `VDDDeInstallIOHook` — register/remove per-port-range
  byte handlers.
- `getCS` / `getIP` — read the guest CPU registers for the trace log.
- (declared for the post-MVP digital path: `VDDRequestDMA`, `VDDQueryDMA`,
  `VDDSetDMA`, `VDDReserveIrqLine`, `VDDReleaseIrqLine`, `call_ica_hw_interrupt`,
  `MGetVdmPointer`.)

## How it supersedes the built-in VSB

NTVDM's startup runs `SetupInstallableVDD()` (which loads the registry `VDD`
list and calls each DLL's `DllMain`) **before** `SbInitialize()` (which installs
the built-in VSB's I/O hooks). The VSB is coded to fail gracefully if a port is
already owned. So `vddsound` installs its hooks in `DLL_PROCESS_ATTACH`, takes
the ports first, and the built-in VSB stands down. We hook the full SB range
`0x220–0x22F` (not just the VSB's sub-ports) to guarantee it fully backs off.

## Control plane vs data plane

- **MIDI (`midi_module`)** — control plane only. MPU-401 bytes are already
  host-consumable; we reassemble them (running status, real-time interleave,
  SysEx) and forward via `midiOutShortMsg` / `midiOutLongMsg`.
- **FM (`opl_module` + `audio_out`)** — no host data plane. The OPL chip
  synthesises from register writes, so we emulate it (Nuked OPL3) and stream the
  PCM out ourselves.
- **SB digital (`sb_module`)** — hybrid; MVP only does detection. The digital
  DSP/DMA path is post-MVP.

## Module boundaries and threading

```
            VDM CPU thread                         audio render thread
  ┌──────────────────────────────┐        ┌──────────────────────────────┐
  guest OUT/IN ─▶ vdd_entry shims  │        │  audio_out: DirectSound ring │
   (log + dispatch)                │        │  buffer, refilled behind the │
        │            │       │     │        │  play cursor                 │
        ▼            ▼       ▼     │        │            │                 │
   sb_module   midi_module  opl_module ──▶ (SPSC queue) ─▶ opl_render ─▶ Nuked OPL3
                  │                │        └──────────────────────────────┘
                  ▼                │
              winmm midiOut        │
```

- I/O hook callbacks run on the **VDM CPU thread**. They must be fast and must
  never block.
- `audio_out` runs a dedicated **render thread** that owns the Nuked chip and
  the DirectSound buffer.
- The only shared structure is a **lock-free single-producer/single-consumer
  ring** in `opl_module`: the hook enqueues `(register, value)` writes; the
  render thread drains them before synthesising each block. The Nuked chip is
  touched only by the render thread; the AdLib detection status is touched only
  by the VDM thread — neither needs a lock.

## CRT-free build

Windows XP has no UCRT (`api-ms-win-crt-*.dll`), and the Homebrew MinGW defaults
to UCRT, so we link `-nostdlib`. `src/crt0.c` supplies the DLL entry point
(`DllMainCRTStartup`, forwarding to `DllMain`) and `memcpy`/`memmove`/`memset`.
The logger formats with hand-rolled integer routines (no `snprintf`). Result:
the DLL imports only XP-present system DLLs plus static `libgcc`.

## Post-MVP roadmap

1. **SB16 digital playback** — DSP command FSM, `VDDRequestDMA` to pull guest
   PCM, IRQ completion via `call_ica_hw_interrupt`, mixed into the same
   DirectSound buffer as FM (sum in int32, clamp to int16).
2. **Mixer chip** — the SB mixer registers (volumes, source select).
3. **Stereo / high-speed / auto-init DMA** modes.
4. **Control Panel applet (`.cpl`)** — device selection, OPL core choice, log
   path, output latency.
