/*
 * common.h - shared types and the hand-declared NTVDM VDD ABI.
 *
 * There is no Windows DDK on the build host, so the VDD I/O-hook structures,
 * handler calling conventions, and the ntvdm.exe register accessors are
 * declared here by hand. Layouts are taken verbatim from the ReactOS DDK
 * headers nt_vdd.h / vddsvc.h (which mirror the Microsoft DDK); the imported
 * symbol names are validated against the real target ntvdm/ntvdm.exe.
 */
#ifndef VDDSOUND_COMMON_H
#define VDDSOUND_COMMON_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- VDD I/O hook ABI (nt_vdd.h) ----------------------------------------- */

/* All handlers are WINAPI (__stdcall) and return VOID. A wrong calling
 * convention corrupts the stack and crashes the whole VM. */
typedef VOID (WINAPI *PFNVDD_INB)  (WORD iport, PBYTE data);
typedef VOID (WINAPI *PFNVDD_INW)  (WORD iport, PWORD data);
typedef VOID (WINAPI *PFNVDD_INSB) (WORD iport, PBYTE data, WORD count);
typedef VOID (WINAPI *PFNVDD_INSW) (WORD iport, PWORD data, WORD count);
typedef VOID (WINAPI *PFNVDD_OUTB) (WORD iport, BYTE data);
typedef VOID (WINAPI *PFNVDD_OUTW) (WORD iport, WORD data);
typedef VOID (WINAPI *PFNVDD_OUTSB)(WORD iport, PBYTE data, WORD count);
typedef VOID (WINAPI *PFNVDD_OUTSW)(WORD iport, PWORD data, WORD count);

typedef struct _VDD_IO_PORTRANGE {
    WORD First;
    WORD Last;
} VDD_IO_PORTRANGE, *PVDD_IO_PORTRANGE;

typedef struct _VDD_IO_HANDLERS {
    PFNVDD_INB   inb_handler;
    PFNVDD_INW   inw_handler;
    PFNVDD_INSB  insb_handler;
    PFNVDD_INSW  insw_handler;
    PFNVDD_OUTB  outb_handler;
    PFNVDD_OUTW  outw_handler;
    PFNVDD_OUTSB outsb_handler;
    PFNVDD_OUTSW outsw_handler;
} VDD_IO_HANDLERS, *PVDD_IO_HANDLERS;

/* ---- Imported from ntvdm.exe (via libntvdm.a) ---------------------------- */

BOOL WINAPI VDDInstallIOHook(HANDLE hVdd, WORD cPortRange,
                             PVDD_IO_PORTRANGE pPortRange,
                             PVDD_IO_HANDLERS IoHandlers);
VOID WINAPI VDDDeInstallIOHook(HANDLE hVdd, WORD cPortRange,
                               PVDD_IO_PORTRANGE pPortRange);

/* User (DOS-task) hooks. NTVDM calls the create handler when a DOS program
 * starts. We install our I/O hooks from there, not from DllMain, so they run
 * AFTER autoexec.nt (after SET BLASTER=A0 has torn down the built-in VSB and
 * freed the sound ports). A port already owned by the built-in VSB cannot be
 * taken; it must be free when VDDInstallIOHook runs. */
typedef VOID (WINAPI *PFNVDD_UCREATE)(USHORT DosPDB);
typedef VOID (WINAPI *PFNVDD_UTERMINATE)(USHORT DosPDB);
typedef VOID (WINAPI *PFNVDD_UBLOCK)(VOID);
typedef VOID (WINAPI *PFNVDD_URESUME)(VOID);

BOOL WINAPI VDDInstallUserHook(HANDLE hVdd, PFNVDD_UCREATE ucr,
                               PFNVDD_UTERMINATE uterm,
                               PFNVDD_UBLOCK ublock, PFNVDD_URESUME uresume);
BOOL WINAPI VDDDeInstallUserHook(HANDLE hVdd);

/* Guest CPU register accessors (undecorated WINAPI exports of ntvdm.exe). */
USHORT WINAPI getCS(VOID);
USHORT WINAPI getIP(VOID);
ULONG  WINAPI getEIP(VOID);

/* GetVDMPointer target: flat host pointer into VDM memory. Address is the
 * linear (seg<<4)+offset; ProtectedMode FALSE for real-mode/V86 addresses. */
PBYTE WINAPI MGetVdmPointer(ULONG Address, ULONG Size, BOOL ProtectedMode);

/* ---- DMA + IRQ (digital SB playback) ------------------------------------ */

/* Transfer up to Length bytes from the guest's DMA buffer on channel iChannel
 * into Buffer, advancing the virtual 8237 controller. Returns the number of
 * bytes actually transferred (0 when the guest side has nothing queued). */
ULONG WINAPI VDDRequestDMA(HANDLE hVdd, ULONG iChannel, PVOID Buffer,
                           ULONG Length);

/* Post a hardware IRQ into the guest's 8259(s). ica = controller (0 = master,
 * lines 0-7; 1 = slave, lines 8-15), line = IRQ line on that controller, count
 * = number of interrupts to inject. The SB completion IRQ5 is (0, 5, 1).
 * (VDDReserveIrqLine/VDDReleaseIrqLine are in the .def but intentionally left
 * unreferenced for now - they lack an @N byte count there, which would need
 * fixing before they can link.) */
VOID WINAPI call_ica_hw_interrupt(int ica, int line, int count);

#ifdef __cplusplus
}
#endif

#endif /* VDDSOUND_COMMON_H */
