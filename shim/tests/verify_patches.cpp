// Checks every patch the shim applies against a copy of the DRM-free SADK.exe, without running the game:
// verify_patches.exe <SADK.exe>. The shim's own patch code runs unchanged in sadkmod's verify mode.
#include "../shim.hpp"

#include <sadkmod/game/sadk_noav/module.hpp>

#include <cstdio>

void *real_ptrs[N_EXPORTS];   // unused here; the patch code does not touch sockets

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::printf("usage: verify_patches.exe <SADK.exe>\n");
        return 2;
    }
    char log[MAX_PATH];
    GetTempPathA(MAX_PATH, log);
    std::strcat(log, "verify_patches.log");
    DeleteFileA(log);
    sadk::log_open(log);
    if (sadk::file_md5(argv[1]) != sadk::game::md5) {
        std::printf("%s is not the DRM-free build (%s)\n", argv[1], sadk::game::md5);
        return 2;
    }
    static sadk::MappedImage image;
    if (!sadk::map_image(argv[1], image)) return 2;
    sadk::verify_with(sadk::Module::sadk, &image);
    cfg.disable_billboards = true;
    apply_map_sharing();
    apply_billboards();
    sadk::verify_with(sadk::Module::sadk, nullptr);
    auto n = sadk::verify_counts();
    std::printf("%d patches match, %d do not (13 expected to match; log: %s)\n", n.matched, n.mismatched, log);
    if (FILE *f = std::fopen(log, "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) std::fputs(line, stdout);
        std::fclose(f);
    }
    return n.matched == 13 && n.mismatched == 0 ? 0 : 1;
}
