// Verify mode: check a mod's patches against a copy of the game binary, without running the game.
//
// map_image() maps a module's file (e.g. SADK.exe) the way the loader would, at any address. While an image is
// set with verify_with(), resolve() points into it, patch() only compares the expected bytes and writes nothing,
// and hooks only check that the target lies inside the image. A test program can then run a mod's
// "apply patches" code unchanged and report every patch whose expected bytes do not match.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core.hpp"

namespace sadk {

struct MappedImage {
    std::vector<std::uint8_t> bytes;   // the image, index = RVA
    std::uintptr_t preferred = 0;      // its preferred base
};

bool map_image(const char *path, MappedImage &out);
void verify_with(Module m, const MappedImage *image);   // nullptr: back to normal mode
bool verifying();

struct VerifyCounts {
    int matched = 0, mismatched = 0;
};
VerifyCounts verify_counts();

}  // namespace sadk
