// Imports heroes4.exe needs that x86recomp's HLE does not provide (or
// provides for later APIs only). Registered from main (RegisterHleOverrides),
// after the runtime's own registrars, so a name defined in both uses this one.
//
//   GDI      the game draws its frames into 16-bit DIB sections and copies
//            them onto DirectDraw surfaces with GetDC + BitBlt (0x7D7BC0,
//            0x8D00A0), so device contexts, DIB sections and SRCCOPY blits
//            are real here; surface DCs lock the surface for their lifetime.
//   Miles    only the MP3 decoder is used, through RIB: the "ASI codec" and
//            "ASI stream" interfaces (0x7A3E20); playback is DirectSound.
//   menus    the window menu (0x8D11C1..) and accelerators: inert handles.
//   network  no multiplayer: sockets fail.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/audio/source.h"
#include "runtime/guest_alloc.h"
#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

// The handlers below must replace the runtime's of the same name. A
// registration is a write to the registry, the last one wins, and static
// initialisers run in link order: with the runtime in shared libraries
// (the Mac) it comes first, linked statically (iOS) after this file. So
// these are collected here and registered by RegisterHleOverrides, from
// main, after every static initialiser.
namespace recomp::h4 {
namespace {
struct LateDef {
  const char* dll;
  const char* name;
  int nargs;
  hle::Fn fn;
  bool x64;
};
std::vector<LateDef>& LateDefs() {
  static std::vector<LateDef> defs;
  return defs;
}
struct LateRegistrar {
  LateRegistrar(const char* dll, const char* name, int nargs, hle::Fn fn, bool x64 = false) {
    LateDefs().push_back({dll, name, nargs, fn, x64});
  }
};
}  // namespace

void RegisterHleOverrides() {
  for (const LateDef& d : LateDefs()) hle::Registrar(d.dll, d.name, d.nargs, d.fn, d.x64);
}
}  // namespace recomp::h4

#undef HLE_NAMED_X
#define HLE_NAMED_X(dll, export_name, ident, nargs, x64)                   \
  static uint64_t RECOMP_HLE_CAT(hle_, ident)(::recomp::Ctx & c);              \
  static ::recomp::h4::LateRegistrar RECOMP_HLE_CAT(reg_, ident){             \
      dll, export_name, nargs, &RECOMP_HLE_CAT(hle_, ident), x64};          \
  static uint64_t RECOMP_HLE_CAT(hle_, ident)([[maybe_unused]] ::recomp::Ctx & c)

namespace recomp::hle {

namespace {

std::mutex g_mu;

// Guest memory for buffers the game reads or that thunks write (lock
// descriptors, MP3 fetches, inet_ntoa's string). Committed on first use.
gaddr GuestBlock(uint32_t size) {
  GuestAllocator& vm = g_proc->vm;
  gaddr base = vm.Reserve(0, size);
  if (!base || !vm.Commit(base, size, Prot::RW)) FatalF("h4: out of guest memory (%u bytes)", size);
  std::memset(GPtr(base), 0, size);
  return base;
}

// ---- GDI: device contexts and DIB sections

struct Dib {
  gaddr bits = 0, block = 0;
  int w = 0, h = 0, bpp = 0;
  uint32_t pitch = 0;
  bool bottom_up = false;
};

struct Dc {
  uint32_t selected = 0;  // memory DC: the selected bitmap
  uint32_t surface = 0;   // surface DC: the locked IDirectDrawSurface7
  gaddr desc = 0;         // surface DC: its DDSURFACEDESC2 (lock result)
};

std::map<uint32_t, Dib> g_dibs;
std::map<uint32_t, Dc> g_dcs;
uint32_t g_next = 0x00F00010;
uint32_t NewHandle() { return g_next += 0x10; }

// A DC's pixels: its surface's lock or its selected DIB section.
struct Pixels {
  uint8_t* p = nullptr;
  int w = 0, h = 0, bpp = 0;
  int64_t pitch = 0;  // negative for bottom-up DIBs (row 0 is the last)
};

bool PixelsOf(uint32_t hdc, Pixels* out) {
  auto d = g_dcs.find(hdc);
  if (d == g_dcs.end()) return false;
  if (d->second.surface) {
    gaddr desc = d->second.desc;
    out->w = int(Rd32(desc + 12));
    out->h = int(Rd32(desc + 8));
    out->pitch = int32_t(Rd32(desc + 16));
    out->bpp = int(Rd32(desc + 72 + 12));  // ddpfPixelFormat.dwRGBBitCount
    out->p = GPtr(Rd32(desc + 36));        // lpSurface
    return true;
  }
  auto b = g_dibs.find(d->second.selected);
  if (b == g_dibs.end()) return false;
  const Dib& dib = b->second;
  out->w = dib.w;
  out->h = dib.h;
  out->bpp = dib.bpp;
  out->pitch = dib.bottom_up ? -int64_t(dib.pitch) : int64_t(dib.pitch);
  out->p = GPtr(dib.bits) + (dib.bottom_up ? int64_t(dib.pitch) * (dib.h - 1) : 0);
  return true;
}

HLE("gdi32.dll", CreateCompatibleDC, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t h = NewHandle();
  g_dcs[h] = Dc{};
  return h;
}

HLE("gdi32.dll", DeleteDC, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_dcs.erase(Arg(c, 0));
  return 1;
}

HLE("gdi32.dll", SelectObject, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  auto d = g_dcs.find(Arg(c, 0));
  if (d == g_dcs.end()) return 0x00E00000u;
  uint32_t prev = d->second.selected;
  d->second.selected = Arg(c, 1);
  return prev ? prev : 0x00E00000u;
}

HLE("gdi32.dll", DeleteObject, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  auto b = g_dibs.find(Arg(c, 0));
  if (b != g_dibs.end()) {
    g_proc->vm.Release(b->second.block);
    g_dibs.erase(b);
  }
  return 1;
}

// (hdc, BITMAPINFO*, usage, void** bits, section, offset)
HLE("gdi32.dll", CreateDIBSection, 6) {
  gaddr bmi = Arg(c, 1), ppv = Arg(c, 3);
  Dib dib;
  dib.w = int32_t(Rd32(bmi + 4));
  int32_t h = int32_t(Rd32(bmi + 8));
  dib.bottom_up = h > 0;
  dib.h = h < 0 ? -h : h;
  dib.bpp = Rd16(bmi + 14);
  if (dib.w <= 0 || dib.h <= 0 || (dib.bpp != 8 && dib.bpp != 16 && dib.bpp != 24 && dib.bpp != 32)) {
    WarnF("h4: CreateDIBSection %dx%d %d bpp unsupported", dib.w, h, dib.bpp);
    if (ppv) Wr32(ppv, 0);
    return 0;
  }
  dib.pitch = (uint32_t(dib.w) * dib.bpp + 31) / 32 * 4;
  uint32_t size = (dib.pitch * dib.h + 0xFFF) & ~0xFFFu;
  dib.block = dib.bits = GuestBlock(size);
  if (ppv) Wr32(ppv, dib.bits);
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t handle = NewHandle();
  g_dibs[handle] = dib;
  return handle;
}

// (dst, x, y, w, h, src, sx, sy, rop): SRCCOPY between same-depth DCs.
HLE("gdi32.dll", BitBlt, 9) {
  constexpr uint32_t kSrcCopy = 0x00CC0020;
  uint32_t rop = Arg(c, 8);
  int x = int32_t(Arg(c, 1)), y = int32_t(Arg(c, 2));
  int w = int32_t(Arg(c, 3)), h = int32_t(Arg(c, 4));
  int sx = int32_t(Arg(c, 6)), sy = int32_t(Arg(c, 7));
  std::lock_guard<std::mutex> l(g_mu);
  Pixels d, s;
  if (rop != kSrcCopy || !PixelsOf(Arg(c, 0), &d) || !PixelsOf(Arg(c, 5), &s)) {
    WarnF("h4: BitBlt rop %08X dst %08X src %08X not handled", rop, uint32_t(Arg(c, 0)),
          uint32_t(Arg(c, 5)));
    return 1;
  }
  if (d.bpp != s.bpp) {
    WarnF("h4: BitBlt %d bpp -> %d bpp not handled", s.bpp, d.bpp);
    return 1;
  }
  // Clip to both bitmaps.
  if (x < 0) { sx -= x; w += x; x = 0; }
  if (y < 0) { sy -= y; h += y; y = 0; }
  if (sx < 0) { x -= sx; w += sx; sx = 0; }
  if (sy < 0) { y -= sy; h += sy; sy = 0; }
  w = std::min({w, d.w - x, s.w - sx});
  h = std::min({h, d.h - y, s.h - sy});
  if (w <= 0 || h <= 0) return 1;
  const int bytes = d.bpp / 8;
  for (int row = 0; row < h; row++)
    std::memmove(d.p + (y + row) * d.pitch + int64_t(x) * bytes,
                 s.p + (sy + row) * s.pitch + int64_t(sx) * bytes, size_t(w) * bytes);
  return 1;
}

// IDirectDrawSurface7::GetDC(this, HDC*): a DC over the locked surface.
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetDC", H4_DDS7_GetDC, 2) {
  constexpr uint32_t kLockWait = 0x1, kDescSize = 124;
  uint32_t surface = Arg(c, 0), phdc = Arg(c, 1);
  gaddr desc = GuestBlock(0x1000);
  Wr32(desc, kDescSize);
  uint32_t hr = uint32_t(CallGuest(c, ThunkFor("ddraw.dll", "IDirectDrawSurface7::Lock"),
                                   {surface, 0, desc, kLockWait, 0}));
  if (hr != 0) {
    g_proc->vm.Release(desc);
    return hr;
  }
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t h = NewHandle();
  g_dcs[h] = Dc{0, surface, desc};
  Wr32(phdc, h);
  return 0;
}

// Dev aid: H4_DC_SHOT_EVERY=N saves every Nth released surface DC (the
// frames the game draws through GDI never reach the runtime's frame dump)
// as $H4_SHOT_DIR/dc_NNNNN.bmp.
void MaybeDumpDc(const Pixels& px) {
  static const int every = [] {
    const char* e = Env("DC_SHOT_EVERY");
    return e ? std::atoi(e) : 0;
  }();
  static int n = 0;
  if (every <= 0 || ++n % every || px.bpp != 16 || px.w <= 0 || px.h <= 0) return;
  const char* dir = Env("SHOT_DIR");
  char path[512];
  std::snprintf(path, sizeof path, "%s/dc_%05d.bmp", dir ? dir : ".", n);
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  const uint32_t row = (uint32_t(px.w) * 3 + 3) & ~3u, size = 54 + row * px.h;
  uint8_t hdr[54] = {'B', 'M'};
  auto put = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
  put(2, size); put(10, 54); put(14, 40); put(18, uint32_t(px.w)); put(22, uint32_t(px.h));
  hdr[26] = 1; hdr[28] = 24;
  std::fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> line(row);
  for (int y = px.h - 1; y >= 0; y--) {
    const uint16_t* src = reinterpret_cast<const uint16_t*>(px.p + y * px.pitch);
    for (int x = 0; x < px.w; x++) {
      uint16_t v = src[x];  // R5G6B5
      line[3 * x + 0] = uint8_t((v & 0x1F) * 255 / 31);
      line[3 * x + 1] = uint8_t((v >> 5 & 0x3F) * 255 / 63);
      line[3 * x + 2] = uint8_t((v >> 11) * 255 / 31);
    }
    std::fwrite(line.data(), 1, row, f);
  }
  std::fclose(f);
}

// IDirectDrawSurface7::ReleaseDC(this, HDC)
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::ReleaseDC", H4_DDS7_ReleaseDC, 2) {
  gaddr desc = 0;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto d = g_dcs.find(Arg(c, 1));
    if (d == g_dcs.end() || d->second.surface != Arg(c, 0)) return 0x80070057;  // E_INVALIDARG
    desc = d->second.desc;
    Pixels px;
    if (PixelsOf(d->first, &px)) MaybeDumpDc(px);
    g_dcs.erase(d);
  }
  uint32_t hr = uint32_t(CallGuest(c, ThunkFor("ddraw.dll", "IDirectDrawSurface7::Unlock"),
                                   {Arg(c, 0), 0}));
  g_proc->vm.Release(desc);
  return hr;
}

// IDirectDraw7::CreateClipper(this, flags, IDirectDrawClipper**, outer):
// the game clips its windowed presents to its window, which here is all
// of the frame; the clipper only remembers its window.
struct Clipper : com::Object {
  uint32_t hwnd = 0;
};

uint32_t ClipperVtable() {
  return com::Vtable("ddraw.dll", "IDirectDrawClipper",
                     {"QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd",
                      "Initialize", "IsClipListChanged", "SetClipList", "SetHWnd"});
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::CreateClipper", H4_DD7_CreateClipper, 4) {
  constexpr uint32_t kNoAggregation = 0x80040110;
  if (Arg(c, 3)) return kNoAggregation;
  Wr32(Arg(c, 2), com::Publish(std::make_shared<Clipper>(), ClipperVtable()));
  return 0;
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::QueryInterface", H4_DDC_QueryInterface, 3) {
  Wr32(Arg(c, 2), 0);
  return 0x80004002;  // E_NOINTERFACE
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::AddRef", H4_DDC_AddRef, 1) {
  return com::AddRef(Arg(c, 0));
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::Release", H4_DDC_Release, 1) {
  return com::Release(Arg(c, 0));
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::SetHWnd", H4_DDC_SetHWnd, 3) {
  if (Clipper* k = com::As<Clipper>(Arg(c, 0))) k->hwnd = Arg(c, 2);
  return 0;
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::GetHWnd", H4_DDC_GetHWnd, 2) {
  Clipper* k = com::As<Clipper>(Arg(c, 0));
  Wr32(Arg(c, 1), k ? k->hwnd : 0);
  return 0;
}
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::IsClipListChanged", H4_DDC_IsClipListChanged, 2) {
  Wr32(Arg(c, 1), 0);
  return 0;
}


// ---- COM methods the runtime's interfaces list but do not implement
//
// A call to one of them would be fatal ("unimplemented import"), as
// IDirectDraw7::CreateClipper was. None has been seen in this game; each is
// answered here so a rare path cannot end the game: methods with no effect
// worth modelling succeed, the rest fail with E_NOTIMPL (and clear their
// out-pointer) as unsupported features do on Windows, with a warning.
uint64_t ComFallback(Ctx& c, const char* name, bool ok, int out) {
  if (out >= 0 && Arg(c, out)) Wr32(Arg(c, out), 0);
  if (!ok) WarnF("h4: %s: not supported, failing\n", name);
  return ok ? 0 : 0x80004001;  // S_OK / E_NOTIMPL
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::AddOverlayDirtyRect", H4_ComFallback0, 2) { return ComFallback(c, "IDirectDrawSurface7::AddOverlayDirtyRect", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::BltBatch", H4_ComFallback1, 4) { return ComFallback(c, "IDirectDrawSurface7::BltBatch", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::EnumAttachedSurfaces", H4_ComFallback2, 3) { return ComFallback(c, "IDirectDrawSurface7::EnumAttachedSurfaces", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::EnumOverlayZOrders", H4_ComFallback3, 4) { return ComFallback(c, "IDirectDrawSurface7::EnumOverlayZOrders", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetClipper", H4_ComFallback4, 2) { return ComFallback(c, "IDirectDrawSurface7::GetClipper", false, 1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetOverlayPosition", H4_ComFallback5, 3) { return ComFallback(c, "IDirectDrawSurface7::GetOverlayPosition", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetPalette", H4_ComFallback6, 2) { return ComFallback(c, "IDirectDrawSurface7::GetPalette", false, 1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::Initialize", H4_ComFallback7, 3) { return ComFallback(c, "IDirectDrawSurface7::Initialize", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetOverlayPosition", H4_ComFallback8, 3) { return ComFallback(c, "IDirectDrawSurface7::SetOverlayPosition", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetPalette", H4_ComFallback9, 2) { return ComFallback(c, "IDirectDrawSurface7::SetPalette", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::UpdateOverlay", H4_ComFallback10, 6) { return ComFallback(c, "IDirectDrawSurface7::UpdateOverlay", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::UpdateOverlayDisplay", H4_ComFallback11, 2) { return ComFallback(c, "IDirectDrawSurface7::UpdateOverlayDisplay", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::UpdateOverlayZOrder", H4_ComFallback12, 3) { return ComFallback(c, "IDirectDrawSurface7::UpdateOverlayZOrder", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetDDInterface", H4_ComFallback13, 2) { return ComFallback(c, "IDirectDrawSurface7::GetDDInterface", false, 1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetSurfaceDesc", H4_ComFallback14, 3) { return ComFallback(c, "IDirectDrawSurface7::SetSurfaceDesc", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetPrivateData", H4_ComFallback15, 5) { return ComFallback(c, "IDirectDrawSurface7::SetPrivateData", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetPrivateData", H4_ComFallback16, 4) { return ComFallback(c, "IDirectDrawSurface7::GetPrivateData", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::FreePrivateData", H4_ComFallback17, 2) { return ComFallback(c, "IDirectDrawSurface7::FreePrivateData", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::ChangeUniquenessValue", H4_ComFallback18, 1) { return ComFallback(c, "IDirectDrawSurface7::ChangeUniquenessValue", true, -1); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::CreatePalette", H4_ComFallback19, 5) { return ComFallback(c, "IDirectDraw7::CreatePalette", false, 3); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::DuplicateSurface", H4_ComFallback20, 3) { return ComFallback(c, "IDirectDraw7::DuplicateSurface", false, 2); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::EnumSurfaces", H4_ComFallback21, 5) { return ComFallback(c, "IDirectDraw7::EnumSurfaces", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::GetGDISurface", H4_ComFallback22, 2) { return ComFallback(c, "IDirectDraw7::GetGDISurface", false, 1); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::Initialize", H4_ComFallback23, 2) { return ComFallback(c, "IDirectDraw7::Initialize", true, -1); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::GetSurfaceFromDC", H4_ComFallback24, 3) { return ComFallback(c, "IDirectDraw7::GetSurfaceFromDC", false, 2); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::StartModeTest", H4_ComFallback25, 4) { return ComFallback(c, "IDirectDraw7::StartModeTest", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::EvaluateMode", H4_ComFallback26, 3) { return ComFallback(c, "IDirectDraw7::EvaluateMode", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawGammaControl::QueryInterface", H4_ComFallback27, 3) { return ComFallback(c, "IDirectDrawGammaControl::QueryInterface", false, 2); }
HLE_NAMED("dsound.dll", "IDirectSound8::Compact", H4_ComFallback28, 1) { return ComFallback(c, "IDirectSound8::Compact", true, -1); }
HLE_NAMED("dsound.dll", "IDirectSound8::SetSpeakerConfig", H4_ComFallback29, 2) { return ComFallback(c, "IDirectSound8::SetSpeakerConfig", true, -1); }
HLE_NAMED("dsound.dll", "IDirectSound8::Initialize", H4_ComFallback30, 2) { return ComFallback(c, "IDirectSound8::Initialize", true, -1); }
HLE_NAMED("dsound.dll", "IDirectSound8::VerifyCertification", H4_ComFallback31, 2) { return ComFallback(c, "IDirectSound8::VerifyCertification", true, -1); }
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::Initialize", H4_ComFallback32, 3) { return ComFallback(c, "IDirectSoundBuffer8::Initialize", true, -1); }
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::SetFX", H4_ComFallback33, 4) { return ComFallback(c, "IDirectSoundBuffer8::SetFX", false, -1); }
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::AcquireResources", H4_ComFallback34, 4) { return ComFallback(c, "IDirectSoundBuffer8::AcquireResources", false, -1); }
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetObjectInPath", H4_ComFallback35, 5) { return ComFallback(c, "IDirectSoundBuffer8::GetObjectInPath", false, 4); }
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetConeAngles", H4_ComFallback36, 3) { return ComFallback(c, "IDirectSound3DBuffer::GetConeAngles", false, -1); }
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetConeOrientation", H4_ComFallback37, 2) { return ComFallback(c, "IDirectSound3DBuffer::GetConeOrientation", false, -1); }
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetConeOutsideVolume", H4_ComFallback38, 2) { return ComFallback(c, "IDirectSound3DBuffer::GetConeOutsideVolume", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::GetClipList", H4_ComFallback39, 4) { return ComFallback(c, "IDirectDrawClipper::GetClipList", false, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::Initialize", H4_ComFallback40, 3) { return ComFallback(c, "IDirectDrawClipper::Initialize", true, -1); }
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::SetClipList", H4_ComFallback41, 3) { return ComFallback(c, "IDirectDrawClipper::SetClipList", true, -1); }

// ---- user32: menus, accelerators, window geometry

HLE("user32.dll", LoadAcceleratorsA, 2) { return 0x00A10001u; }
HLE("user32.dll", LoadMenuA, 2) { return 0x00A20000u | (Arg(c, 1) & 0xFFFF); }
HLE("user32.dll", DestroyMenu, 1) { return 1; }
HLE("user32.dll", EnableMenuItem, 3) { return 0; }  // previous state: MF_ENABLED
HLE("user32.dll", CheckMenuItem, 3) { return 0; }   // previous state: MF_UNCHECKED

uint32_t g_menu = 0;
HLE("user32.dll", SetMenu, 2) {
  g_menu = Arg(c, 1);
  return 1;
}
HLE("user32.dll", GetMenu, 1) { return g_menu; }

// Windows here have no frame: the window rectangle is the client's.
HLE("user32.dll", AdjustWindowRectEx, 4) { return 1; }

// (POINT by value): the game only asks whether the pointer is over its
// window, the one active window.
HLE("user32.dll", WindowFromPoint, 2) {
  return CallGuest(c, ThunkFor("user32.dll", "GetActiveWindow"), {});
}

// ---- kernel32

int Compare(const std::u16string& a, const std::u16string& b, bool nocase) {
  auto fold = [&](char16_t ch) -> char16_t {
    return nocase && ch < 0x80 && ch >= 'a' && ch <= 'z' ? char16_t(ch - 32) : ch;
  };
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    char16_t x = fold(a[i]), y = fold(b[i]);
    if (x != y) return x < y ? 1 : 3;
  }
  return a.size() == b.size() ? 2 : a.size() < b.size() ? 1 : 3;
}

std::u16string Counted(gaddr p, int32_t n, bool wide) {
  std::u16string s;
  for (int32_t i = 0; n < 0 || i < n; i++) {
    char16_t ch = wide ? Rd16(p + 2 * i) : uint8_t(GPtr(p)[i]);
    if (n < 0 && !ch) break;
    s += ch;
  }
  return s;
}

constexpr uint32_t kNormIgnoreCase = 1;

// (lcid, flags, s1, n1, s2, n2): CSTR_LESS_THAN 1, EQUAL 2, GREATER 3.
HLE("kernel32.dll", CompareStringA, 6) {
  return Compare(Counted(Arg(c, 2), int32_t(Arg(c, 3)), false),
                 Counted(Arg(c, 4), int32_t(Arg(c, 5)), false), Arg(c, 1) & kNormIgnoreCase);
}
HLE("kernel32.dll", CompareStringW, 6) {
  return Compare(Counted(Arg(c, 2), int32_t(Arg(c, 3)), true),
                 Counted(Arg(c, 4), int32_t(Arg(c, 5)), true), Arg(c, 1) & kNormIgnoreCase);
}

// ---- dsound: DirectSoundCreate is DirectSoundCreate8 (IDirectSound8 starts
// with IDirectSound's methods).
HLE_NAMED("dsound.dll", "#1", H4_DirectSoundCreate, 3) {
  return CallGuest(c, ThunkFor("dsound.dll", "#11"), {Arg(c, 0), Arg(c, 1), Arg(c, 2)});
}

// ---- binkw32 (Bink 1.0 entry points)

// (bink, volume): the volume of the first track.
HLE_NAMED("binkw32.dll", "_BinkSetVolume@8", H4_BinkSetVolume8, 2) {
  return CallGuest(c, ThunkFor("binkw32.dll", "_BinkSetVolume@12"), {Arg(c, 0), 0, Arg(c, 1)});
}
// (bink, flags): the frame's dirty rectangles, in the BINK struct
// (FrameRects at +0x34, {left, top, width, height} each; NumRects at
// +0xB4). The game invalidates exactly these (0x58DA00), so the movie only
// reaches the screen through them. Every decoded frame is whole here.
HLE_NAMED("binkw32.dll", "_BinkGetRects@8", H4_BinkGetRects, 2) {
  gaddr b = Arg(c, 0);
  Wr32(b + 0x34, 0);
  Wr32(b + 0x38, 0);
  Wr32(b + 0x3C, Rd32(b + 0x00));  // Width
  Wr32(b + 0x40, Rd32(b + 0x04));  // Height
  Wr32(b + 0xB4, 1);
  return 1;
}
// BINKSUMMARY from the BINK struct: the game reads TotalFrames / FrameRate.
HLE_NAMED("binkw32.dll", "_BinkGetSummary@8", H4_BinkGetSummary, 2) {
  gaddr b = Arg(c, 0), s = Arg(c, 1);
  std::memset(GPtr(s), 0, 0x58);
  Wr32(s + 0x00, Rd32(b + 0x00));  // Width
  Wr32(s + 0x04, Rd32(b + 0x04));  // Height
  Wr32(s + 0x0C, Rd32(b + 0x14));  // FileFrameRate
  Wr32(s + 0x10, Rd32(b + 0x18));  // FileFrameRateDiv
  Wr32(s + 0x14, std::max(1u, Rd32(b + 0x14)));  // FrameRate
  Wr32(s + 0x18, Rd32(b + 0x18));  // FrameRateDiv
  Wr32(s + 0x20, Rd32(b + 0x08));  // TotalFrames
  return 0;
}

// ---- mss32: RIB and the MP3 ASI codec

constexpr uint32_t kProvider = 0x5A510001;
enum Pref : uint32_t { kLayer = 1, kRate, kWidth, kChannels };

// (interface, attribute, suffix, attribute 2, suffix 2)
HLE_NAMED("mss32.dll", "_RIB_find_files_provider@20", H4_RIB_find_files_provider, 5) {
  std::string iface = GStr(Arg(c, 0)), suffix = Arg(c, 2) ? win32::Lower(GStr(Arg(c, 2))) : "";
  return iface == "ASI codec" && suffix.size() >= 4 && suffix.substr(suffix.size() - 4) == ".mp3"
             ? kProvider
             : 0;
}

// (provider, interface, count, RIB_INTERFACE_ENTRY* {type, name, &dest, subtype})
HLE("mss32.dll", RIB_request_interface, 4) {
  if (Arg(c, 0) != kProvider) return 1;
  gaddr e = Arg(c, 3);
  for (uint32_t i = 0; i < Arg(c, 2); i++, e += 16) {
    uint32_t type = Rd32(e);
    std::string name = GStr(Rd32(e + 4));
    gaddr dest = Rd32(e + 8);
    uint32_t v = 0;
    if (type == 0) {  // RIB_FUNCTION
      v = ThunkFor("mss32.dll", "ASI::" + name);
    } else {  // RIB_ATTRIBUTE / RIB_PREFERENCE
      v = name == "Layer" ? kLayer
          : name == "Raw source sample rate" ? kRate
          : name == "Raw source sample width" ? kWidth
          : name == "Raw source channels" ? kChannels : 0;
      if (!v) WarnF("h4: ASI attribute '%s' unknown", name.c_str());
    }
    Wr32(dest, v);
  }
  return 0;
}

struct AsiStream {
  std::unique_ptr<audio::Source> src;
  std::vector<float> frames;  // decoded, not yet returned
  size_t head = 0;
  bool eof = false;
};
std::map<uint32_t, std::unique_ptr<AsiStream>> g_streams;
uint32_t g_next_stream = 0x5A520000;

// (user, fetch callback, total size): the whole MP3 is fetched now. The
// game's callback (0x7A3DF0) reads sequentially (offset -1).
HLE_NAMED("mss32.dll", "ASI::ASI_stream_open", H4_ASI_stream_open, 3) {
  constexpr uint32_t kChunk = 0x10000;
  uint32_t user = Arg(c, 0), cb = Arg(c, 1), total = Arg(c, 2);
  gaddr buf = GuestBlock(kChunk);
  auto data = std::make_shared<std::vector<uint8_t>>();
  for (;;) {
    uint32_t want = total ? std::min<uint32_t>(kChunk, total - uint32_t(data->size())) : kChunk;
    if (!want) break;
    int32_t got = int32_t(CallGuest(c, cb, {user, buf, want, 0xFFFFFFFFu}));
    if (got <= 0) break;
    data->insert(data->end(), GPtr(buf), GPtr(buf) + got);
    if (uint32_t(got) < want) break;
  }
  g_proc->vm.Release(buf);
  if (g_trace) {
    std::string head;
    for (size_t i = 0; i < std::min<size_t>(16, data->size()); i++) {
      char b[4];
      std::snprintf(b, sizeof b, "%02X ", (*data)[i]);
      head += b;
    }
    TraceF("  ASI_stream_open: %zu bytes: %s\n", data->size(), head.c_str());
  }
  std::string err;
  auto s = std::make_unique<AsiStream>();
  s->src = audio::MakeMp3Source(data, &err);
  if (!s->src) {
    WarnF("h4: ASI_stream_open: %s (%zu bytes)", err.c_str(), data->size());
    return 0;
  }
  if (g_trace) TraceF("  ASI_stream_open: %d Hz, %d channels\n", s->src->rate(), s->src->channels());
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t h = g_next_stream += 0x10;
  g_streams[h] = std::move(s);
  return h;
}

// (stream, buffer, bytes): interleaved 16-bit PCM at the MP3's own rate
// and channels; returns the bytes written (fewer only at the end).
HLE_NAMED("mss32.dll", "ASI::ASI_stream_process", H4_ASI_stream_process, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_streams.find(Arg(c, 0));
  if (it == g_streams.end()) return 0;
  AsiStream& s = *it->second;
  const int ch = s.src->channels();
  int16_t* out = reinterpret_cast<int16_t*>(GPtr(Arg(c, 1)));
  const size_t want = Arg(c, 2) / (2 * ch) * ch;  // whole frames
  size_t n = 0;
  while (n < want) {
    if (s.head == s.frames.size()) {
      if (s.eof) break;
      s.frames.resize(4096 * ch);
      int got = s.src->Read(s.frames.data(), 4096);
      s.frames.resize(size_t(got) * ch);
      s.head = 0;
      if (got < 4096) s.eof = true;
      if (!got) break;
    }
    size_t take = std::min(want - n, s.frames.size() - s.head);
    for (size_t i = 0; i < take; i++) {
      float v = std::clamp(s.frames[s.head + i], -1.0f, 1.0f);
      out[n + i] = int16_t(v * 32767.0f);
    }
    n += take;
    s.head += take;
  }
  if (g_trace && n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += double(out[i]) * out[i];
    TraceF("  ASI_stream_process: %zu samples, rms %.0f\n", n, std::sqrt(sum / double(n)));
  }
  return uint32_t(n * 2);
}

HLE_NAMED("mss32.dll", "ASI::ASI_stream_close", H4_ASI_stream_close, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_streams.erase(Arg(c, 0));
  return 0;
}

// (stream, offset): only a rewind is possible.
HLE_NAMED("mss32.dll", "ASI::ASI_stream_seek", H4_ASI_stream_seek, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_streams.find(Arg(c, 0));
  if (it == g_streams.end() || Arg(c, 1) != 0) return 0;
  AsiStream& s = *it->second;
  s.src->Seek(0);
  s.frames.clear();
  s.head = 0;
  s.eof = false;
  return 0;
}

// The game sets the layer and the "raw source" format, which only matter
// for raw input; MP3 frames carry their own.
HLE_NAMED("mss32.dll", "ASI::ASI_stream_set_preference", H4_ASI_stream_set_preference, 3) {
  return 0;
}
HLE_NAMED("mss32.dll", "ASI::ASI_stream_attribute", H4_ASI_stream_attribute, 2) { return 0; }

gaddr g_empty_string = 0;
HLE_NAMED("mss32.dll", "ASI::ASI_error", H4_ASI_error, 0) {
  if (!g_empty_string) g_empty_string = GuestBlock(0x1000);
  return g_empty_string;
}

// ---- wsock32: no network

constexpr uint32_t kSocketError = 0xFFFFFFFF;
HLE_NAMED("wsock32.dll", "#23", H4_socket, 3) { return kSocketError; }  // INVALID_SOCKET
HLE_NAMED("wsock32.dll", "#4", H4_connect, 3) { return kSocketError; }
HLE_NAMED("wsock32.dll", "#3", H4_closesocket, 1) { return 0; }
HLE_NAMED("wsock32.dll", "#6", H4_getsockname, 3) { return kSocketError; }
HLE_NAMED("wsock32.dll", "#9", H4_htons, 1) {
  uint16_t v = uint16_t(Arg(c, 0));
  return uint16_t(v << 8 | v >> 8);
}
gaddr g_ntoa = 0;
HLE_NAMED("wsock32.dll", "#11", H4_inet_ntoa, 1) {
  if (!g_ntoa) g_ntoa = GuestBlock(0x1000);
  uint32_t a = Arg(c, 0);
  std::string s = std::to_string(a & 0xFF) + "." + std::to_string(a >> 8 & 0xFF) + "." +
                  std::to_string(a >> 16 & 0xFF) + "." + std::to_string(a >> 24);
  std::memcpy(GPtr(g_ntoa), s.c_str(), s.size() + 1);
  return g_ntoa;
}

}  // namespace
}  // namespace recomp::hle
