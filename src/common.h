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

/* NTVDM's per-channel 8237 state (packed; matches the DDK VDD_DMA_INFO that
 * VDDQueryDMA/VDDSetDMA exchange). 9 bytes: a WORD[16] over-reads it safely. */
#pragma pack(push, 1)
typedef struct _VDD_DMA_INFO {
    WORD addr;    /* current address register (byte- or word-granular)      */
    WORD count;   /* current count register (transfers - 1, counts down)    */
    WORD page;    /* page register                                          */
    BYTE status;  /* status (TC/DREQ bits)                                  */
    BYTE mode;    /* mode register (bits 6-7 = mode, bit 4 = auto-init)     */
    BYTE mask;    /* mask register bit for this channel                     */
} VDD_DMA_INFO, *PVDD_DMA_INFO;
#pragma pack(pop)

/* VDDSetDMA field-select flags. */
#define VDD_DMA_ADDR    0x01u
#define VDD_DMA_COUNT   0x02u
#define VDD_DMA_PAGE    0x04u
#define VDD_DMA_STATUS  0x08u

/* Transfer up to Length bytes from the guest's DMA buffer on channel iChannel
 * into Buffer, advancing the virtual 8237 controller. Returns the number of
 * bytes actually transferred. NOTE: returns 0 on this NTVDM (it only services
 * Single-mode unmasked channels) - we drive the controller with VDDSetDMA
 * instead, the way VDMSound does. */
ULONG WINAPI VDDRequestDMA(HANDLE hVdd, ULONG iChannel, PVOID Buffer,
                           ULONG Length);

/* Report NTVDM's view of a DMA channel (addr/count/page/mode/mask) into a
 * VDD_DMA_INFO struct; non-consuming. */
VOID WINAPI VDDQueryDMA(HANDLE hVdd, ULONG iChannel, PVOID pDmaInfo);

/* Write selected fields (fDMA = VDD_DMA_* mask) of NTVDM's emulated 8237 for
 * iChannel from pDmaInfo. This is how a VDD advances the controller's current
 * address/count so a guest that polls the DMA ports (DOOM/DMX) sees the play
 * cursor move. The blessed approach (VDMSound's), since VDDRequestDMA is inert
 * here. */
BOOL WINAPI VDDSetDMA(HANDLE hVdd, ULONG iChannel, ULONG fDMA, PVOID pDmaInfo);

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
