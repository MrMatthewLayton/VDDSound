/*
 * crt0.c - minimal CRT replacement so the DLL links with -nostdlib and has no
 * dependency on msvcrt/UCRT (Windows XP has no UCRT api-ms-win-crt DLLs).
 *
 * Provides:
 *   - the DLL entry point (_DllMainCRTStartup), forwarding to our DllMain;
 *   - mem* functions the compiler and the vendored OPL core emit calls to.
 *
 * We have no C++ globals, TLS callbacks, or static constructors, so no CRT
 * initialisation is required. Build with -fno-builtin so the loops below are
 * not turned back into self-referential mem* calls.
 */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved);

BOOL WINAPI DllMainCRTStartup(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved)
{
    return DllMain(hinstDLL, reason, reserved);
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}
