/*
 * logger.c - one event per line, microsecond timestamps from QPC.
 * CRT-free: formatting is hand-rolled so the DLL needs no C runtime (XP has
 * no UCRT). Integer-only timestamp math (64-bit divide via libgcc).
 */
#include "logger.h"

static HANDLE log_file = INVALID_HANDLE_VALUE;
static LARGE_INTEGER qpc_freq;
static CRITICAL_SECTION log_lock;
static int log_ready = 0;
static int trace_enabled = 0; /* per-access I/O tracing; off unless VDDSOUND_TRACE set */

#define DEFAULT_LOG_PATH "C:\\vddsound\\trace.log"

static int put_str(char *b, int pos, const char *s)
{
    while (*s) {
        b[pos++] = *s++;
    }
    return pos;
}

static int put_u(char *b, int pos, unsigned long v, int width)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    while (n < width) {
        tmp[n++] = '0';
    }
    while (n > 0) {
        b[pos++] = tmp[--n];
    }
    return pos;
}

static int put_hex(char *b, int pos, unsigned long v, int width)
{
    static const char hexd[] = "0123456789ABCDEF";
    char tmp[8];
    int n = 0;
    do {
        tmp[n++] = hexd[v & 0xFu];
        v >>= 4;
    } while (v != 0);
    while (n < width) {
        tmp[n++] = '0';
    }
    while (n > 0) {
        b[pos++] = tmp[--n];
    }
    return pos;
}

void logger_init(void)
{
    char path[MAX_PATH];
    DWORD n;

    n = GetEnvironmentVariableA("VDDSOUND_LOG", path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) {
        lstrcpynA(path, DEFAULT_LOG_PATH, sizeof(path));
    }

    CreateDirectoryA("C:\\vddsound", NULL); /* best-effort for default path */

    if (!QueryPerformanceFrequency(&qpc_freq) || qpc_freq.QuadPart == 0) {
        qpc_freq.QuadPart = 1;
    }

    /* Share read AND write: a DOS program launched in its own ntvdm.exe is a
     * separate process that must also be able to append to this same log.
     * FILE_APPEND_DATA makes concurrent appends atomic at end-of-file. */
    log_file = CreateFileA(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) {
        return;
    }

    InitializeCriticalSection(&log_lock);
    log_ready = 1;

    /* Per-access I/O tracing is OFF by default: a synchronous WriteFile on
     * every trapped port access wrecks the guest's real-time timing (stretched
     * audio). Set VDDSOUND_TRACE=1 to enable it for forensic capture. */
    {
        char buf[8];
        DWORD n = GetEnvironmentVariableA("VDDSOUND_TRACE", buf, sizeof(buf));
        trace_enabled = (n > 0 && n < sizeof(buf) && buf[0] != '0');
    }
    logger_note(trace_enabled ? "logger: per-access TRACE enabled (VDDSOUND_TRACE)"
                              : "logger: per-access trace off (notes only)");
}

void logger_close(void)
{
    if (!log_ready) {
        return;
    }
    EnterCriticalSection(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(log_file);
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&log_lock);
    DeleteCriticalSection(&log_lock);
    log_ready = 0;
}

void logger_note(const char *msg)
{
    LARGE_INTEGER now;
    unsigned long long ticks, freq, rem;
    unsigned long whole, micros;
    char line[200];
    int pos = 0;
    DWORD written;

    if (!log_ready) {
        return;
    }

    QueryPerformanceCounter(&now);
    ticks = (unsigned long long)now.QuadPart;
    freq  = (unsigned long long)qpc_freq.QuadPart;
    whole = (unsigned long)(ticks / freq);
    rem   = ticks % freq;
    micros = (unsigned long)((rem * 1000000ULL) / freq);

    pos = put_u(line, pos, whole, 1);
    line[pos++] = '.';
    pos = put_u(line, pos, micros, 6);
    pos = put_str(line, pos, "  NOTE ");
    while (*msg && pos < (int)sizeof(line) - 2) {
        line[pos++] = *msg++;
    }
    line[pos++] = '\r';
    line[pos++] = '\n';

    EnterCriticalSection(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE) {
        WriteFile(log_file, line, (DWORD)pos, &written, NULL);
    }
    LeaveCriticalSection(&log_lock);
}

void logger_note_kv(const char *msg, unsigned long value)
{
    LARGE_INTEGER now;
    unsigned long long ticks, freq, rem;
    unsigned long whole, micros;
    char line[200];
    int pos = 0;
    DWORD written;

    if (!log_ready) {
        return;
    }

    QueryPerformanceCounter(&now);
    ticks = (unsigned long long)now.QuadPart;
    freq  = (unsigned long long)qpc_freq.QuadPart;
    whole = (unsigned long)(ticks / freq);
    rem   = ticks % freq;
    micros = (unsigned long)((rem * 1000000ULL) / freq);

    pos = put_u(line, pos, whole, 1);
    line[pos++] = '.';
    pos = put_u(line, pos, micros, 6);
    pos = put_str(line, pos, "  NOTE ");
    while (*msg && pos < (int)sizeof(line) - 40) {
        line[pos++] = *msg++;
    }
    line[pos++] = '=';
    pos = put_u(line, pos, value, 1);
    pos = put_str(line, pos, " (0x");
    pos = put_hex(line, pos, value, 1);
    line[pos++] = ')';
    line[pos++] = '\r';
    line[pos++] = '\n';

    EnterCriticalSection(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE) {
        WriteFile(log_file, line, (DWORD)pos, &written, NULL);
    }
    LeaveCriticalSection(&log_lock);
}

void logger_log(int is_in, WORD port, int size, DWORD value)
{
    LARGE_INTEGER now;
    unsigned long long ticks, freq, rem;
    unsigned long whole, micros;
    char line[160];
    int pos = 0;
    DWORD written;

    if (!log_ready || !trace_enabled) {
        return;
    }

    QueryPerformanceCounter(&now);
    ticks = (unsigned long long)now.QuadPart;
    freq  = (unsigned long long)qpc_freq.QuadPart;
    whole = (unsigned long)(ticks / freq);
    rem   = ticks % freq;
    micros = (unsigned long)((rem * 1000000ULL) / freq);

    pos = put_u(line, pos, whole, 1);
    line[pos++] = '.';
    pos = put_u(line, pos, micros, 6);
    line[pos++] = ' ';
    line[pos++] = ' ';
    pos = put_str(line, pos, is_in ? "IN " : "OUT");
    line[pos++] = ' ';
    pos = put_str(line, pos, "port=0x");
    pos = put_hex(line, pos, port, 3);
    pos = put_str(line, pos, " size=");
    pos = put_u(line, pos, (unsigned long)size, 1);
    pos = put_str(line, pos, " value=0x");
    pos = put_hex(line, pos, value, size >= 2 ? 4 : 2);
    pos = put_str(line, pos, " cs:ip=0x");
    pos = put_hex(line, pos, getCS(), 4);
    line[pos++] = ':';
    pos = put_str(line, pos, "0x");
    pos = put_hex(line, pos, getIP(), 4);
    line[pos++] = '\r';
    line[pos++] = '\n';

    EnterCriticalSection(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE) {
        WriteFile(log_file, line, (DWORD)pos, &written, NULL);
    }
    LeaveCriticalSection(&log_lock);
}
