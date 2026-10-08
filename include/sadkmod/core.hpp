// sadkmod core: the game's modules, typed addresses of its functions and globals, calling conventions.
//
// Every address in sadkmod is a static address from the project's Ghidra map (sourcemap/), i.e. relative to the
// module's preferred base: SADK.exe 0x00400000 (no relocations, so always there), tincat3.dll 0x10000000.
// A generated declaration such as
//     sadk::game::fn::S2CE::CTexture::CreateFromFile
// is an empty constexpr object of type Fn<Module::sadk, 0x004e80d0, bool (SADK_THISCALL *)(CTexture *, ...)>.
// Calling it calls the game function; hook.hpp hooks it by that same object.
#pragma once
#include <cstddef>
#include <cstdint>

// The game was built with MSVC 2005. GCC spells its calling conventions as attributes; they are part of the
// function type, so a hook with the wrong convention is a compile error, not a crash.
#define SADK_THISCALL __attribute__((thiscall))   // this in ECX, arguments on the stack, callee cleans up
#define SADK_STDCALL __attribute__((stdcall))     // arguments on the stack, callee cleans up
#define SADK_CDECL __attribute__((cdecl))         // arguments on the stack, caller cleans up
#define SADK_FASTCALL __attribute__((fastcall))   // first two in ECX, EDX, rest on the stack, callee cleans up

namespace sadk {

enum class Module : std::uint8_t { sadk, tincat3 };

constexpr std::uintptr_t preferred_base(Module m) { return m == Module::sadk ? 0x00400000u : 0x10000000u; }
const char *module_file(Module m);

// Where the module is loaded in this process (0 if it is not loaded). Cached after the first success.
std::uintptr_t module_base(Module m);

// Static (Ghidra) address -> address in this process. In verify mode (verify.hpp) this is the address inside the
// mapped image being checked instead.
std::uintptr_t resolve(Module m, std::uintptr_t static_address);

// A name as a template argument (C++20): the generated declarations carry their Ghidra name, for logs.
template <std::size_t N>
struct Name {
    char text[N];
    constexpr Name(const char (&s)[N]) { for (std::size_t i = 0; i < N; i++) text[i] = s[i]; }
};

// A function's code as the Ghidra map has it: N ranges [start, end) of static addresses. Switch tables are data
// and lie outside them, so the ranges can be decoded instruction by instruction (calls.hpp). Body<0>: not known.
template <std::size_t N>
struct Body {
    std::uintptr_t ranges[2 * N];
    static constexpr std::size_t size = N;
    constexpr const std::uintptr_t *data() const { return ranges; }
};
template <>
struct Body<0> {
    static constexpr std::size_t size = 0;
    constexpr const std::uintptr_t *data() const { return nullptr; }
};

// A plain address with no type: code locations, data without a known type, patch sites. For a function of the map,
// B is its body.
template <Module M, std::uintptr_t A, Name N = "", auto B = Body<0>{}>
struct Addr {
    static constexpr Module module = M;
    static constexpr std::uintptr_t address = A;
    static constexpr const char *name = N.text;
    static constexpr auto body = B;
    std::uintptr_t get() const { return resolve(M, A); }
};

// A game function. P is its function pointer type, calling convention included; B its body (calls.hpp).
template <Module M, std::uintptr_t A, class P, Name N = "", auto B = Body<0>{}>
struct Fn {
    using pointer = P;
    static constexpr Module module = M;
    static constexpr std::uintptr_t address = A;
    static constexpr const char *name = N.text;
    static constexpr auto body = B;
    P get() const { return reinterpret_cast<P>(resolve(M, A)); }
    template <class... X>
    decltype(auto) operator()(X &&...x) const { return get()(static_cast<X &&>(x)...); }
};

// A game global of type T.
template <Module M, std::uintptr_t A, class T, Name N = "">
struct Var {
    using type = T;
    static constexpr Module module = M;
    static constexpr std::uintptr_t address = A;
    static constexpr const char *name = N.text;
    T *get() const { return reinterpret_cast<T *>(resolve(M, A)); }
    T &operator*() const { return *get(); }
    T *operator->() const { return get(); }
};

// A field of a game object by byte offset, for objects whose layout is not (fully) typed yet.
template <class T = std::uint8_t, class O>
T *at(O *object, std::size_t offset) { return reinterpret_cast<T *>(reinterpret_cast<std::uint8_t *>(object) + offset); }

}  // namespace sadk
