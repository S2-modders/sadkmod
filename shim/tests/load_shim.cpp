// Loads wsock32.dll (the shim) into a program that is not SADK.exe and checks the pass-through: the forwarded
// exports reach the system wsock32.dll, the hooked ones work, and the shim reports itself inactive.
// load_shim.exe <path to the shim's wsock32.dll>
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    HMODULE shim = LoadLibraryA(argv[1]);
    if (!shim) {
        std::printf("FAIL: LoadLibrary %lu\n", GetLastError());
        return 1;
    }
    int failures = 0;
    auto ord = [&](int n) { return reinterpret_cast<void *>(GetProcAddress(shim, MAKEINTRESOURCEA(n))); };
    auto htons_ = reinterpret_cast<u_short(WINAPI *)(u_short)>(ord(9));
    auto startup = reinterpret_cast<int(WINAPI *)(WORD, LPWSADATA)>(ord(115));
    auto socket_ = reinterpret_cast<SOCKET(WINAPI *)(int, int, int)>(ord(23));
    auto connect_ = reinterpret_cast<int(WINAPI *)(SOCKET, const sockaddr *, int)>(ord(4));
    auto close_ = reinterpret_cast<int(WINAPI *)(SOCKET)>(ord(3));
    if (!htons_ || htons_(0x1234) != 0x3412) { std::printf("FAIL: htons (forwarded)\n"); failures++; }
    WSADATA wsa;
    if (!startup || startup(MAKEWORD(1, 1), &wsa) != 0) { std::printf("FAIL: WSAStartup (forwarded)\n"); failures++; }
    SOCKET s = socket_ ? socket_(AF_INET, SOCK_STREAM, IPPROTO_TCP) : INVALID_SOCKET;
    if (s == INVALID_SOCKET) { std::printf("FAIL: socket (forwarded)\n"); failures++; }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons_(1);                    // nothing listens on 127.0.0.1:1
    a.sin_addr.s_addr = 0x0100007f;
    int rc = connect_(s, reinterpret_cast<sockaddr *>(&a), sizeof a);
    if (rc == 0) { std::printf("FAIL: connect (hooked) to a closed port succeeded\n"); failures++; }
    if (close_(s) != 0) { std::printf("FAIL: closesocket (hooked)\n"); failures++; }
    Sleep(500);                                // the start-up thread writes the inactive line
    char log[MAX_PATH];
    GetModuleFileNameA(nullptr, log, MAX_PATH);
    std::strcpy(std::strrchr(log, '\\') + 1, "wsock32_shim.txt");
    bool loaded = false, inactive = false;
    if (FILE *f = std::fopen(log, "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::fputs(line, stdout);
            loaded |= std::strstr(line, "shim loaded") && std::strstr(line, "exports resolved");
            inactive |= std::strstr(line, "stays inactive") != nullptr;
        }
        std::fclose(f);
    }
    if (!loaded || !inactive) { std::printf("FAIL: log lines missing\n"); failures++; }
    std::printf("%s\n", failures ? "FAILED" : "pass-through OK");
    return failures != 0;
}
