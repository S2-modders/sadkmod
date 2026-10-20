// sadkmod core: the game's modules, typed addresses of its functions and globals, calling conventions.
//
// Every address in sadkmod is a static address from the project's Ghidra map (sourcemap/), i.e. relative to the
// module's preferred base: SADK.exe 0x00400000 (no relocations, so always there), tincat3.dll 0x10000000.
// A generated declaration such as
//     sadk::game::fn::S2CE::CTexture::CreateFromFile
// is a constexpr object of type Fn<Conv::this_call, bool(CTexture *, ...)> that holds its module, address, name and
// code ranges as values. Calling it calls the game function; hook.hpp hooks it by that same object.
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

// The `this` of a method that the game enters through a secondary vftable: it points Offset bytes into the object
// (at the base class whose virtual the method overrides), not at the object's start. The generated declarations use
// Face<T, Offset> * for such a parameter, so that it cannot be taken for a T *; object() gives the T.
template <class T, std::size_t Offset>
struct Face {
    Face() = delete;
    T *object() { return reinterpret_cast<T *>(reinterpret_cast<char *>(this) - Offset); }
    static Face *of(T *object) { return reinterpret_cast<Face *>(reinterpret_cast<char *>(object) + Offset); }
};

// A name as a template argument (C++20): the generated globals (Var) carry their Ghidra name, for logs.
template <std::size_t N>
struct Name {
    char text[N];
    constexpr Name(const char (&s)[N]) { for (std::size_t i = 0; i < N; i++) text[i] = s[i]; }
};

// A plain address with no type: code locations, data without a known type, patch sites. For a function of the map,
// `body` is its code as the Ghidra map has it: body_size ranges [start, end) of static addresses, 2 * body_size values
// (nullptr, 0: not known). Switch tables are data and lie outside them, so the ranges can be decoded instruction by
// instruction (calls.hpp). `name` and `body` point at named arrays (not literals), so that a declaration can be a
// template argument (Hook<F>).
struct Addr {
    Module module;
    std::uintptr_t address;
    const char *name;
    const std::uintptr_t *body;
    std::size_t body_size;
    std::uintptr_t get() const { return resolve(module, address); }
};

// A game function's calling convention. Not named after the conventions: Windows headers define `cdecl` as a macro.
enum class Conv : std::uint8_t { this_call, std_call, c_call, fast_call };

// The function pointer type of convention C and plain function type F.
template <Conv C, class F> struct conv_pointer;
template <class R, class... A> struct conv_pointer<Conv::this_call, R(A...)> { using type = R (SADK_THISCALL *)(A...); };
template <class R, class... A> struct conv_pointer<Conv::std_call, R(A...)> { using type = R (SADK_STDCALL *)(A...); };
template <class R, class... A> struct conv_pointer<Conv::c_call, R(A...)> { using type = R (SADK_CDECL *)(A...); };
template <class R, class... A> struct conv_pointer<Conv::c_call, R(A..., ...)> { using type = R (SADK_CDECL *)(A..., ...); };
template <class R, class... A> struct conv_pointer<Conv::fast_call, R(A...)> { using type = R (SADK_FASTCALL *)(A...); };

// A game function: an Addr with its type. The type is the convention plus a plain function type, not the pointer type
// itself: GCC instantiates a template on thousands of calling-convention pointer types very slowly (PCH build 107 s
// instead of 24 s, measured 2026-10-20), and only the type is a template argument, so functions of one signature
// share one Fn.
template <Conv C, class F>
struct Fn : Addr {
    using pointer = typename conv_pointer<C, F>::type;
    pointer get() const { return reinterpret_cast<pointer>(resolve(module, address)); }
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
