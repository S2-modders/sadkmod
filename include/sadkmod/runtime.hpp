// Process-level helpers for a mod running inside SADK.exe: log, paths, settings, the exe check, the game heap.
#pragma once
#include <cstddef>
#include <string>

#include "core.hpp"

namespace sadk {

// Log file. Every line gets a timestamp; thread-safe. Until log_open() is called, lines go nowhere.
void log_open(const char *path);
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// <root>\bin\SADK.exe -> paths below <root>, the folder the game data lives in.
const char *exe_path();
const char *game_root();
std::string game_path(const char *relative);   // game_root() + '\\' + relative

// MD5 of a file as 32 lowercase hex digits; empty on failure.
std::string file_md5(const char *path);

// True if this process is the build the generated declarations describe (sadk::game::md5, the DRM-free SADK.exe).
// Every address in sadkmod is only valid for that build: check this before patching or hooking anything.
bool exe_is_supported();

// Windows INI settings, read the way the game reads its own (GetPrivateProfileString).
struct Ini {
    std::string path;
    explicit Ini(std::string file) : path(std::move(file)) {}
    std::string get(const char *section, const char *key, const char *fallback = "") const;  // quotes, spaces trimmed
    int get_int(const char *section, const char *key, int fallback) const;
    bool get_bool(const char *section, const char *key, bool fallback) const;                // true/yes/1
};

// An export of a loaded DLL as a typed function pointer (nullptr if the DLL or the export is missing).
void *proc_address(const char *dll, const char *name);
template <class P>
P proc(const char *dll, const char *name) { return reinterpret_cast<P>(proc_address(dll, name)); }

// The game's own heap (MSVC 2005 runtime of SADK.exe). Memory the game will free must come from here.
void *game_malloc(std::size_t size);
void game_free(void *p);

}  // namespace sadk
