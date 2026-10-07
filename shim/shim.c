/* Test shim: a proxy wsock32.dll for tincat3.dll (which imports all of its sockets from WSOCK32.dll by
 * ordinal). Every export is forwarded by ordinal to the system wsock32.dll. Proof of loading: writes
 * wsock32_shim.txt next to the game exe, and appends a line for every listen() with its local port. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include "ordinals.h"

void *real_ptrs[N_EXPORTS];
static HMODULE real_dll;
static char log_path[MAX_PATH];

static void log_line(const char *fmt, ...)
{
    FILE *f = fopen(log_path, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

typedef int (WINAPI *listen_fn)(SOCKET, int);
typedef int (WINAPI *getsockname_fn)(SOCKET, struct sockaddr *, int *);

int WINAPI shim_listen(SOCKET s, int backlog)
{
    int rc = ((listen_fn)real_ptrs[LISTEN_INDEX])(s, backlog);
    struct sockaddr_in a;
    int len = sizeof a;
    unsigned port = 0;
    if (((getsockname_fn)GetProcAddress(real_dll, MAKEINTRESOURCEA(6)))(s, (struct sockaddr *)&a, &len) == 0)
        port = (unsigned)(((a.sin_port & 0xff) << 8) | (a.sin_port >> 8));   /* no ws2_32 import */
    log_line("listen(socket %u, backlog %d) on port %u -> %d", (unsigned)s, backlog, port, rc);
    return rc;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    char exe[MAX_PATH], me[MAX_PATH], sys[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    GetModuleFileNameA(self, me, MAX_PATH);
    strcpy(log_path, exe);
    char *slash = strrchr(log_path, '\\');
    strcpy(slash ? slash + 1 : log_path, "wsock32_shim.txt");

    GetSystemDirectoryA(sys, MAX_PATH);            /* SysWOW64 for this 32-bit process */
    strcat(sys, "\\wsock32.dll");
    real_dll = LoadLibraryA(sys);
    int missing = 0;
    for (int i = 0; i < N_EXPORTS; i++) {
        real_ptrs[i] = real_dll ? (void *)GetProcAddress(real_dll, MAKEINTRESOURCEA(ordinals[i])) : NULL;
        if (!real_ptrs[i]) missing++;
    }
    log_line("shim loaded: pid %lu, exe %s, shim %s, real %s (%s), %d of %d exports resolved",
             GetCurrentProcessId(), exe, me, sys, real_dll ? "ok" : "LOAD FAILED",
             N_EXPORTS - missing, N_EXPORTS);
    return real_dll != NULL;
}
