# vddsound — a replacement Virtual Sound Blaster for NTVDM

> **Current project state, build/deploy loop, and known issues live in
> [`STATUS.md`](STATUS.md).** This file is the design-level install/test guide;
> the actual build is published via `scripts/build.sh` into `../vddsound/` and
> deployed on the VM with `redeploy.bat`. Read STATUS.md first.

`vddsound.dll` is a 32-bit NTVDM Virtual Device Driver for Windows XP SP3. It
replaces NTVDM's half-working built-in Virtual Sound Blaster (VSB) and routes
DOS sound to your modern Windows audio devices:

- **FM (OPL2/OPL3)** is synthesised in software (Nuked OPL3) and streamed to
  **DirectSound**.
- **MIDI (MPU-401 UART)** is passed through to the Windows MIDI output via
  **winmm**.
- Every trapped port access is written to a **forensic trace log**.

It installs once and is active automatically in every DOS box — no launcher,
no per-game shortcut.

## Building (on macOS)

```sh
./scripts/build.sh
```

This installs the toolchain if needed (`brew install mingw-w64 cmake`),
cross-compiles for i686, and prints the DLL's dependency list. Output:
`build/vddsound.dll`. The DLL depends only on `ntvdm.exe`, `winmm.dll`,
`dsound.dll`, `user32.dll` and `kernel32.dll` — all present on Windows XP. It
has **no C-runtime dependency** (no `msvcrt`/UCRT), by design.

## Installing on the XP VM

> **Snapshot the VM first.** A VDD runs inside `ntvdm.exe`; a bug can take the
> whole DOS box down. Always have a snapshot to roll back to.

1. Copy `build/vddsound.dll` to `C:\vddsound\vddsound.dll` on the VM.
2. Register it as a VDD. **Check for existing entries first** — the value is a
   multi-string list and you must not clobber other VDDs:
   ```
   reg query "HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers" /v VDD
   ```
   - If it is **empty/absent**, you can import `scripts\install.reg`.
   - If it **already has lines**, open `regedit`, go to
     `HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers`, edit the
     `VDD` value (REG_MULTI_SZ), and add a new line:
     `C:\vddsound\vddsound.dll`
3. Make sure `C:\WINDOWS\system32\autoexec.nt` advertises the card so games
   autodetect it (the default line is fine; this is what an OPL3/SB16 profile
   looks like):
   ```
   SET BLASTER=A220 I5 D1 H5 P330 T6
   SET MIDI=SYNTH:1 MAP:E MODE:0
   ```
4. **Reboot the VM** (the VDD list is read at NTVDM startup).

## Smoke test

1. **Supersession** — open a DOS prompt and run any sound-using DOS program,
   then look at `C:\vddsound\trace.log`. You should see our handlers firing on
   ports `0x388`/`0x389` (FM), `0x330`/`0x331` (MIDI) or `0x22x` (SB) with
   sensible `cs:ip` values. That proves we took the ports and the built-in VSB
   stood down.
2. **FM** — run an AdLib/OPL game (or an OPL test tool). FM music should be
   audible through your Windows default playback device, with no stuck notes.
3. **MIDI** — run a DOS MPU-401 MIDI player against a `.mid` file. It should be
   audible through your default MIDI device; the log shows `0x330` writes.

Example trace line:

```
1234.567890  OUT port=0x388 size=1 value=0x20 cs:ip=0xF000:0x1238
```

## Troubleshooting

- **DLL not loading / no log file** — re-check the registry path is exactly
  `C:\vddsound\vddsound.dll`, that the file is really there, and that the
  account can write to `C:\vddsound`. Set a custom log path with the
  environment variable `VDDSOUND_LOG`.
- **No FM audio** — check your Windows default playback (DirectSound) device in
  Control Panel → Sounds and Audio Devices. FM still requires the render thread
  and DirectSound to initialise; MIDI works independently of FM.
- **No MIDI** — check the Windows default MIDI device (Control Panel → Sounds →
  Audio → MIDI music playback).
- **Game says "no sound card"** — confirm the `BLASTER`/`MIDI` variables made it
  into the DOS environment: run `set` inside the DOS box and look for them.
- **Still hearing the old crackly built-in VSB** — our hooks didn't install
  first. Confirm the registry entry and reboot. As a guaranteed fallback you
  can force the built-in VSB off by setting an invalid base in `autoexec.nt`
  (`SET BLASTER=A0`) — but then re-advertise a valid card for the game in a
  per-application start-up file, since `A0` also hides the card from the game.

## Licensing

`src/opl3.c` / `src/opl3.h` are Nuked OPL3 by Nuke.YKT, **LGPL-2.1-or-later**.
They are kept as a separable object so the library can be relinked against a

##### modified OPL core, satisfying the LGPL. The rest of vddsound is original.
