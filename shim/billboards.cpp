// The lobby's advertising screens (docs/BINARY_PATCHES.md, "Billboards").
//
// Lobby::CGfxTextureMgr::GetTexture S 00504650: textures named ad0/ad1/ad2.tga become an embedded Internet Explorer
// rendering http://www.funatics.de/sadk/forwardingN.html. Those pages are gone (HTTP 404), so the screens show red
// plus IE's "navigation canceled" page. With DisableBillboards:
//   - the three compare literals are renamed, so the screens load from file like any other texture;
//   - GetTexture's file load, the virtual call `MOV EDX,[ESI+0x28]; MOV ECX,EAX; CALL EDX` at 00504806
//     (S2CE::CTexture::CreateFromFile S 004e80d0), becomes `MOV ECX,EAX; CALL billboard_load`, which loads
//     BillboardTexture instead of adN.tga and crops BillboardRect of it with the game's own D3DX (d3dx9_38.dll,
//     which also decodes DXT) into a 512x512 texture that replaces the full sheet. No game art is shipped.
#include "shim.hpp"

#include <sadkmod/game/sadk_noav/types.hpp>

#include <d3d9.h>

using sadk::log;
using sadk::game::S2CE::CTexture;

typedef HRESULT(WINAPI *d3dx_create_texture_fn)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                IDirect3DTexture9 **);
typedef HRESULT(WINAPI *d3dx_load_surface_fn)(IDirect3DSurface9 *, const PALETTEENTRY *, const RECT *,
                                              IDirect3DSurface9 *, const PALETTEENTRY *, const RECT *, DWORD, D3DCOLOR);
constexpr DWORD D3DX_FILTER_LINEAR = 3;

static sadk::msvc::string billboard_name;   // BillboardTexture, inline (up to 15 characters)

static void billboard_crop(CTexture *tex)
{
    if (!cfg.billboard_crop) return;
    auto create = sadk::proc<d3dx_create_texture_fn>("d3dx9_38.dll", "D3DXCreateTexture");
    auto load = sadk::proc<d3dx_load_surface_fn>("d3dx9_38.dll", "D3DXLoadSurfaceFromSurface");
    auto *device = static_cast<IDirect3DDevice9 *>(tex->device);
    auto *full = static_cast<IDirect3DTexture9 *>(tex->d3dTexture);
    if (!create || !load || !device || !full) {
        log("billboards: crop skipped (d3dx %s, texture %p)", create && load ? "ok" : "missing", static_cast<void *>(full));
        return;
    }
    const int *b = cfg.billboard_rect;
    RECT r = {b[0], b[1], b[2], b[3]};
    IDirect3DTexture9 *cropped = nullptr;
    IDirect3DSurface9 *src = nullptr, *dst = nullptr;
    HRESULT hr = create(device, 512, 512, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cropped);
    if (SUCCEEDED(hr)) hr = full->GetSurfaceLevel(0, &src);
    if (SUCCEEDED(hr)) hr = cropped->GetSurfaceLevel(0, &dst);
    if (SUCCEEDED(hr)) hr = load(dst, nullptr, nullptr, src, nullptr, &r, D3DX_FILTER_LINEAR, 0);
    if (src) src->Release();
    if (dst) dst->Release();
    if (FAILED(hr)) {
        if (cropped) cropped->Release();
        log("billboards: crop failed (hr %08lx)", static_cast<unsigned long>(hr));
        return;
    }
    tex->d3dTexture = cropped;
    tex->width = 512;
    tex->height = 512;
    full->Release();
    log("billboards: screen = %s (%d,%d)-(%d,%d)", cfg.billboard_texture.c_str(), b[0], b[1], b[2], b[3]);
}

// Stands in for the loader call at 00504806: CreateFromFile through the texture's vtable, as GetTexture did.
static bool SADK_THISCALL billboard_load(CTexture *tex, void *path, bool single_level, bool default_pool,
                                         bool full_quality)
{
    auto *name = static_cast<const sadk::msvc::string *>(path);
    bool screen = name->equals_icase("ad0.tga") || name->equals_icase("ad1.tga") || name->equals_icase("ad2.tga");
    bool ok = tex->vftable->CreateFromFile(tex, screen ? &billboard_name : path, single_level, default_pool, full_quality);
    if (ok && screen) billboard_crop(tex);
    return ok;
}

void apply_billboards()
{
    if (!cfg.disable_billboards) return;
    billboard_name = sadk::msvc::string::small(cfg.billboard_texture);
    int ok = 0;
    static const std::uintptr_t literals[3] = {0x007e6240, 0x007e6208, 0x007e61d0};   // "ad0.tga" .. "ad2.tga"
    for (int i = 0; i < 3; i++) {
        auto digit = static_cast<std::uint8_t>('0' + i);
        ok += sadk::patch(literals[i], {'a', 'd', digit}, {'#', 'd', digit}, "billboard texture from disk");
    }
    ok += sadk::patch(0x00504806, {0x8B, 0x56, 0x28, 0x8B, 0xC8, 0xFF, 0xD2},
                      sadk::Bytes{0x8B, 0xC8} + sadk::call_to(sadk::Module::sadk, 0x00504808, (void *)billboard_load),
                      "billboard screen texture swap");
    log("billboards: %d of 4 patches active (DisableBillboards, screens show %.15s, crop %s)", ok,
        cfg.billboard_texture.c_str(), cfg.billboard_crop ? "on" : "off");
}
