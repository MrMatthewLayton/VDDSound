/*
 * vdd_entry.c - DllMain, DOS-task hook, and port-hook installation.
 *
 * NTVDM registers a VDD's hVdd ONCE (VDDInstallIOHook's hVdd->index table has
 * 6 slots and returns ERROR_ALREADY_EXISTS if the same hVdd is registered
 * twice). So VDDInstallIOHook must be called a single time with an ARRAY of all
 * the port ranges and ONE handler set - not once per range. We install all
 * three ranges (SB, MPU-401, OPL) in one call, with a unified handler that
 * dispatches by port. (The earlier per-range calls failed with 183 on the 2nd
 * and 3rd call - that was our bug, not the built-in owning the ports.)
 *
 * Install happens from a DOS-task "create" callback (VDDInstallUserHook), which
 * fires after autoexec.nt has run, so the built-in VSB has already settled.
 *
 * Every trapped access is logged.
 */
#include "common.h"
#include "logger.h"
#include "midi_module.h"
#include "opl_module.h"
#include "sb_module.h"
#include "audio_out.h"

static HANDLE hVdd;
static volatile LONG runtime_started;
static int hooks_installed;

/* All three port ranges, claimed in one VDDInstallIOHook call. */
static VDD_IO_PORTRANGE ranges[3] = {
    { SB_PORT_FIRST,  SB_PORT_LAST  },
    { MPU_PORT_FIRST, MPU_PORT_LAST },
    { OPL_PORT_FIRST, OPL_PORT_LAST }
};

/*
 * Timebase assist. NTVDM's emulated PIT/BIOS-tick runs unevenly (idle
 * throttling etc.), so DOS programs that pace themselves off the BIOS tick at
 * 0040:006C drift. We override it: a host thread continuously writes an
 * accurate tick count (from QueryPerformanceCounter) into that guest memory.
 * Experiment - see if it steadies MIDIPLAY's tempo.
 */
static void ensure_runtime(void)
{
    if (InterlockedCompareExchange(&runtime_started, 1, 0) == 0) {
        int midi_rc, audio_rc;
        logger_note("runtime: starting midi+audio");
        midi_rc = midi_init();
        logger_note(midi_rc == 0 ? "runtime: midi opened"
                                 : "runtime: midi unavailable");
        audio_rc = audio_init();
        logger_note(audio_rc == 0 ? "runtime: audio (directsound) started"
                                  : "runtime: audio unavailable");
        /* Note: overriding the BIOS tick (0040:006C) was tried and removed - it
         * fought NTVDM's IRQ0 updates and made timing worse. */
    }
}

/* One handler set serves all ranges; dispatch to the right module by port. */
static VOID WINAPI uni_outb(WORD p, BYTE d)
{
    ensure_runtime();
    logger_log(0, p, 1, d);
    if (p >= SB_PORT_FIRST && p <= SB_PORT_LAST) {
        sb_outb(p, d);
    } else if (p >= MPU_PORT_FIRST && p <= MPU_PORT_LAST) {
        midi_outb(p, d);
    } else if (p >= OPL_PORT_FIRST && p <= OPL_PORT_LAST) {
        opl_outb(p, d);
    }
}

static VOID WINAPI uni_inb(WORD p, PBYTE d)
{
    ensure_runtime();
    if (p >= SB_PORT_FIRST && p <= SB_PORT_LAST) {
        sb_inb(p, d);
    } else if (p >= MPU_PORT_FIRST && p <= MPU_PORT_LAST) {
        midi_inb(p, d);
    } else if (p >= OPL_PORT_FIRST && p <= OPL_PORT_LAST) {
        opl_inb(p, d);
    } else {
        *d = 0xFF;
    }
    logger_log(1, p, 1, *d);
}

static VDD_IO_HANDLERS uni_handlers = {
    uni_inb, NULL, NULL, NULL, uni_outb, NULL, NULL, NULL
};

static VOID WINAPI on_dos_create(USHORT DosPDB)
{
    logger_note_kv("dos-create: pdb", (unsigned long)DosPDB);
    if (!hooks_installed) {
        BOOL ok;
        DWORD err;
        SetLastError(0);
        ok = VDDInstallIOHook(hVdd, 3, ranges, &uni_handlers);
        err = ok ? 0 : GetLastError();
        logger_note_kv("install all 3 ranges ok", (unsigned long)ok);
        if (ok) {
            hooks_installed = 1;
        } else {
            logger_note_kv("  ^ real err", err);
        }
    }
}

static VOID WINAPI on_dos_terminate(USHORT DosPDB)
{
    (void)DosPDB;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        hVdd = (HANDLE)hinstDLL;

        logger_init();
        logger_note("attach: vddsound build [b44-vdmmeter]");
        /* Pin ntvdm.exe to one CPU. NTVDM's PIT/timer emulation reads host
         * timing that is unstable across cores (unsynchronised TSCs, SpeedStep)
         * on multi-core machines, which makes DOS programs that time themselves
         * off the PIT (e.g. MIDIPLAY) speed up/slow down. Single-core affinity
         * is the standard fix. Loader-lock-safe (kernel32, no LoadLibrary). */
        if (SetProcessAffinityMask(GetCurrentProcess(), 1)) {
            logger_note("attach: pinned ntvdm to CPU0 (timer stability)");
        } else {
            logger_note_kv("attach: affinity pin failed", GetLastError());
        }
        logger_note_kv("attach: pid", (unsigned long)GetCurrentProcessId());
        logger_note_kv("hVdd", (unsigned long)(ULONG_PTR)hVdd);
        opl_init(AUDIO_SAMPLE_RATE);
        sb_init(hVdd);
        {
            BOOL ok = VDDInstallUserHook(hVdd, on_dos_create, on_dos_terminate,
                                         NULL, NULL);
            logger_note_kv("attach: VDDInstallUserHook ok", (unsigned long)ok);
        }
        break;

    case DLL_PROCESS_DETACH:
        audio_shutdown();
        midi_close();
        opl_shutdown();
        if (hooks_installed) {
            VDDDeInstallIOHook(hVdd, 3, ranges);
        }
        VDDDeInstallUserHook(hVdd);
        logger_close();
        break;

    default:
        break;
    }
    return TRUE;
}
