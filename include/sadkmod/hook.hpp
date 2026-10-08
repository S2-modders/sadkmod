// Function hooks.
//
// Hook<F> (MinHook): replaces the start of the game function F with a jump to our function. Every caller is
// affected. Hook<F>::original calls the unhooked function (a trampoline that runs the overwritten instructions
// and jumps back), so a hook usually looks like
//
//     using CreateFromFile = sadk::Hook<sadk::game::fn::S2CE::CTexture::CreateFromFile>;
//     static bool SADK_THISCALL my_load(sadk::game::S2CE::CTexture *t, void *path, bool a, bool b, bool c) {
//         ...
//         return CreateFromFile::original(t, path, a, b, c);
//     }
//     CreateFromFile::install(my_load, "texture loads");
//
// The detour's type must match the declaration exactly (calling convention included), else it does not compile.
//
// before<F> / after<F>: callbacks around the original, without writing a detour. sadkmod generates the detour with
// F's calling convention; the callbacks are ordinary functions (or capture-less lambdas):
//
//     namespace fn = sadk::game::fn;
//     using sadk::game::S2CE::CGraphicDevice;
//     sadk::before<fn::S2CE::CGraphicDevice::Init>(
//         [](CGraphicDevice *&dev, std::int32_t *&mode) { /* may change dev or mode */ }, "before Init");
//     sadk::after<fn::S2CE::CGraphicDevice::Init>(
//         [](bool &ok, CGraphicDevice *dev, std::int32_t *mode) { /* may change ok */ }, "after Init");
//
// A before-callback gets every argument by reference and may change them; an after-callback gets the result by
// reference (unless F returns void) and the arguments as passed, and may change the result. Callbacks run in the
// order they were added; all of one module's callbacks for F share one hook. Up to 8 of each per function and module.
//
// hook_slot: replaces one entry of a vtable (or any table of function pointers) instead; only calls through that
// table are affected.
#pragma once
#include <type_traits>

#include "core.hpp"

namespace sadk {

// Untyped layer (MinHook). `original` receives the trampoline. Logged; false on failure.
bool hook_function(void *target, void *detour, void **original, const char *what);
bool unhook_function(void *target);
bool write_slot(void **slot, void *value, void **previous, const char *what);

// Tag selects an independent hook on the same function (before/after use their own, so a module can also detour F).
template <auto F, class Tag = void>
struct Hook {
    using pointer = typename decltype(F)::pointer;
    static inline pointer original = nullptr;
    static bool installed() { return original != nullptr; }
    static bool install(pointer detour, const char *what) {
        return hook_function(reinterpret_cast<void *>(F.get()), reinterpret_cast<void *>(detour),
                             reinterpret_cast<void **>(&original), what);
    }
};

namespace detail {

// The shape of a function pointer type: result, arguments, and a generator for functions with its calling convention.
template <class P>
struct Shape;

template <class R, class... A>
struct Callbacks {
    using before = void (*)(A &...);
    using after = void (*)(R &, A...);
};
template <class... A>
struct Callbacks<void, A...> {
    using before = void (*)(A &...);
    using after = void (*)(A...);
};

#define SADK_DETAIL_SHAPE(CONVENTION)                                                    \
    template <class R, class... A>                                                       \
    struct Shape<R (CONVENTION *)(A...)> : Callbacks<R, A...> {                          \
        template <class Impl>                                                            \
        static R CONVENTION thunk(A... a) { return Impl::template call<R>(a...); }       \
    };
SADK_DETAIL_SHAPE(SADK_THISCALL)
SADK_DETAIL_SHAPE(SADK_STDCALL)
SADK_DETAIL_SHAPE(SADK_CDECL)   // also plain R (*)(A...)
SADK_DETAIL_SHAPE(SADK_FASTCALL)
#undef SADK_DETAIL_SHAPE

struct WrapTag;

template <auto F>
struct Wrap {
    using S = Shape<typename decltype(F)::pointer>;
    using H = Hook<F, WrapTag>;
    static inline typename S::before befores[8];
    static inline typename S::after afters[8];
    static inline int n_before = 0, n_after = 0;

    template <class R, class... A>
    static R call(A... a) {
        for (int i = 0; i < n_before; i++) befores[i](a...);
        if constexpr (std::is_void_v<R>) {
            H::original(a...);
            for (int i = 0; i < n_after; i++) afters[i](a...);
        } else {
            R r = H::original(a...);
            for (int i = 0; i < n_after; i++) afters[i](r, a...);
            return r;
        }
    }

    static bool ensure(const char *what) {
        return H::installed() || H::install(&S::template thunk<Wrap>, what);
    }
};

}  // namespace detail

// Runs `callback` with F's arguments (by reference, changeable) before F. False if the hook could not be installed.
template <auto F>
bool before(typename detail::Wrap<F>::S::before callback, const char *what) {
    using W = detail::Wrap<F>;
    if (W::n_before == 8) return false;
    W::befores[W::n_before++] = callback;
    if (W::ensure(what)) return true;
    W::n_before--;
    return false;
}

// Runs `callback` with F's result (by reference, changeable) and arguments after F.
template <auto F>
bool after(typename detail::Wrap<F>::S::after callback, const char *what) {
    using W = detail::Wrap<F>;
    if (W::n_after == 8) return false;
    W::afters[W::n_after++] = callback;
    if (W::ensure(what)) return true;
    W::n_after--;
    return false;
}

// Typed vtable slot: `slot` is the address of the entry in the game's table (e.g. &vtbl->CreateFromFile).
template <class P>
bool hook_slot(P *slot, P detour, P *previous, const char *what) {
    return write_slot(reinterpret_cast<void **>(slot), reinterpret_cast<void *>(detour),
                      reinterpret_cast<void **>(previous), what);
}

}  // namespace sadk
