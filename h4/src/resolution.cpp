// The game's screen size and wide screens. Replacements of lifted functions
// named with `replace` in lifter.manifest.
//
// Every display mode change goes through set_mode(width, height, repaint,
// full_screen) at 0x8D3750 (cdecl): the startup mode from the "resolution"
// setting (0x73D9BF), the options dialog's 800x600 / 1024x768 / 1280x1024
// (0x773EB6..), and the full-screen toggle (0x8D3710). It records the size
// (0xAD2508 / 0xAD250C), recreates the DirectDraw surfaces and lays the
// interface out for it.
//
// The interface exists for 800, 1024 and 1280 wide screens only: every
// screen (main menu 0x76D9B0, adventure map, combat, towns, dialogs) picks
// its art and layout by the exact width of its window, and finds nothing
// for another. Its windows are positioned from their parents; a screen
// without one takes the "screen rectangle" (0x8D2E20, the client area) and
// the client area again on WM_SIZE (0x8D22D0). Wide (H4_RESOLUTION=WxH,
// W > 1280, H >= 1024, or "wide": 1024 high at the host display's aspect):
// the display mode is W x H and the screen rectangle the 1280x1024 one in
// its middle, so every screen lays out as at 1280x1024, centred.
//
// The adventure map then takes the whole width (WideAdventure below): the
// top window and the adventure frame span the screen, the right-hand panel
// moves to the right edge, the frame's left edge to the left one, and the
// map window grows in between. Screens built while it is shown (towns,
// combat, dialogs) read their parent's width in their constructors, so
// those run with the top window back at the layout rectangle.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <string>
#include <vector>
#include <random>
#include <set>

#include "runtime/ctx.h"
#include "runtime/gfx/device.h"
#include "runtime/display.h"
#include "runtime/guest_alloc.h"
#include "runtime/hle/present.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::gen {

void f_008D3750_lifted(::recomp::Ctx& c);
void f_008D1360_lifted(::recomp::Ctx& c);
void f_00598170_lifted(::recomp::Ctx& c);
void f_004ACBB0_lifted(::recomp::Ctx& c);
void f_004A3450_lifted(::recomp::Ctx& c);
void f_004A37C0_lifted(::recomp::Ctx& c);
void f_0062B6A0_lifted(::recomp::Ctx& c);
void f_0069FB50_lifted(::recomp::Ctx& c);
void f_008AC0C0_lifted(::recomp::Ctx& c);
void f_0076D9B0_lifted(::recomp::Ctx& c);
void f_0076DEB0_lifted(::recomp::Ctx& c);
void f_008D2E20_lifted(::recomp::Ctx& c);
void f_008D22D0_lifted(::recomp::Ctx& c);
void f_007CA0D0_lifted(::recomp::Ctx& c);
void f_008D0C70_lifted(::recomp::Ctx& c);
void f_00720530_lifted(::recomp::Ctx& c);
void f_0071E6D0_lifted(::recomp::Ctx& c);
void f_0063B840_lifted(::recomp::Ctx& c);

namespace {

constexpr int kLayoutW = 1280, kLayoutH = 1024;
constexpr uint32_t kSetRect = 0x71EEE0;  // window::move(const RECT*, bool repaint), thiscall
constexpr uint32_t kTopWindow = 0xABACF4;
constexpr uint32_t kScreenW = 0xAD2508;  // the size set_mode last set
constexpr uint32_t kInvalidate = 0x71FD10;  // window::invalidate(const RECT* local), thiscall
constexpr uint32_t kMapResize = 0x4F5480;   // adventure_map_window::on_size(const RECT*), thiscall

struct Screen {
  int w = 0, h = 0;  // 0: the game's own sizes
  bool wide = false;
};

const Screen& TheScreen() {
  static Screen s = [] {
    Screen r;
    const char* e = Env("RESOLUTION");
    if (!e) return r;
    if (!std::strcmp(e, "wide")) {
      int dw = 0, dh = 0;
      DesktopSize(&dw, &dh);
      r.h = kLayoutH;
      r.w = dh > 0 ? int(int64_t(dw) * kLayoutH / dh) & ~1 : kLayoutW;
    } else if (std::sscanf(e, "%dx%d", &r.w, &r.h) != 2) {
      r.w = r.h = 0;
    }
    r.wide = r.w > kLayoutW && r.h >= kLayoutH;
    WarnF("h4: screen %dx%d%s\n", r.w, r.h, r.wide ? " (wide: 1280x1024 interface centred)" : "");
    return r;
  }();
  return s;
}

// Wide layout in effect: a wide size set and the game in it (a mode change
// may still be under way, or the game in another size).
bool Wide() {
  const Screen& s = TheScreen();
  return s.wide && int32_t(Rd32(kScreenW)) == s.w;
}

// The layout rectangle in client coordinates.
void LayoutRect(int* l, int* t) {
  const Screen& s = TheScreen();
  *l = (s.w - kLayoutW) / 2;
  *t = (s.h - kLayoutH) / 2;
}

// A guest RECT for calls into the game.
gaddr ScratchRect() {
  static gaddr rect = 0;
  if (!rect) {
    GuestAllocator& vm = g_proc->vm;
    rect = vm.Reserve(0, 0x1000);
    vm.Commit(rect, 0x1000, Prot::RW);
  }
  return rect;
}

// Window fields: rectangle in screen coordinates, children (circular list
// of {next, prev, window}), visibility.
constexpr uint32_t kRect = 0x90, kChildren = 0x34, kVisible = 0xA0;

struct Rect {
  int l, t, r, b;
};
Rect RectOf(uint32_t w) {
  return {int32_t(Rd32(w + kRect)), int32_t(Rd32(w + kRect + 4)), int32_t(Rd32(w + kRect + 8)),
          int32_t(Rd32(w + kRect + 12))};
}
void SetRectRaw(uint32_t w, const Rect& r) {
  Wr32(w + kRect, uint32_t(r.l));
  Wr32(w + kRect + 4, uint32_t(r.t));
  Wr32(w + kRect + 8, uint32_t(r.r));
  Wr32(w + kRect + 12, uint32_t(r.b));
}

template <typename F>
void ForChildren(uint32_t w, F f) {
  const uint32_t head = Rd32(w + kChildren);
  if (!head) return;
  int i = 0;
  for (uint32_t node = Rd32(head); node != head && i < 1000; node = Rd32(node), i++)
    f(Rd32(node + 8));
}

// The windows' vtables (their RTTI: t_adventure_frame, t_adventure_map_window).
// Compared on every main loop pass, so by address.
constexpr uint32_t kAdventureFrameVtable = 0x979720, kAdventureMapVtable = 0x979BC0;
bool IsAdventureFrame(uint32_t w) { return Rd32(w) == kAdventureFrameVtable; }
bool IsAdventureMap(uint32_t w) { return Rd32(w) == kAdventureMapVtable; }

// The layout rectangle in screen coordinates (the client origin is 0,0 in
// full screen).
Rect Layout() {
  int l, t;
  LayoutRect(&l, &t);
  return {l, t, l + kLayoutW, t + kLayoutH};
}

// Moves a child of `parent` to `r` (screen coordinates), its own children
// along, through the game's window::move (which also tells a window that
// changed size).
void MoveWindow(::recomp::Ctx& c, uint32_t w, const Rect& r) {
  const uint32_t parent = Rd32(w + 0x7C);
  const Rect p = parent ? RectOf(parent) : Rect{0, 0, 0, 0};
  const gaddr rect = ScratchRect();
  Wr32(rect, uint32_t(r.l - p.l));
  Wr32(rect + 4, uint32_t(r.t - p.t));
  Wr32(rect + 8, uint32_t(r.r - p.l));
  Wr32(rect + 12, uint32_t(r.b - p.t));
  const uint32_t ecx = c.ecx, eax = c.eax;
  c.ecx = w;
  CallGuest(c, kSetRect, {rect, 1});
  c.ecx = ecx;
  c.eax = eax;
}

// The adventure frame shown in the top window, or 0.
uint32_t AdventureFrame(uint32_t top) {
  uint32_t frame = 0;
  ForChildren(top, [&](uint32_t w) {
    if (!frame && GPtr(w + kVisible)[0] && IsAdventureFrame(w)) frame = w;
  });
  return frame;
}

// Repaints a window's w x h area.
void InvalidateWhole(::recomp::Ctx& c, uint32_t window, int w, int h) {
  const gaddr rect = ScratchRect();
  Wr32(rect, 0);
  Wr32(rect + 4, 0);
  Wr32(rect + 8, uint32_t(w));
  Wr32(rect + 12, uint32_t(h));
  const uint32_t ecx = c.ecx, eax = c.eax;
  c.ecx = window;
  CallGuest(c, kInvalidate, {rect});
  c.ecx = ecx;
  c.eax = eax;
}

uint32_t g_widened = 0;  // the adventure frame laid out across the screen
std::set<uint32_t> g_tiled;  // bitmap layers drawn repeated across their width
std::map<uint32_t, Rect> g_layout_rects;  // the widened frame's children, as laid out
int g_refresh_map = 0;       // main loop passes until the map is repainted whole

// Moves the adventure map window. Its own resize (vtable +0x44) only acts
// on a size that differs from its rectangle, and window::move calls it after
// changing that, so it is called first; it also renders only the area it did
// not have before, so an empty size first (which drops the view) makes all
// of it new.
void MoveMapWindow(::recomp::Ctx& c, uint32_t w, const Rect& r) {
  const gaddr size = ScratchRect() + 0x20;
  const uint32_t ecx = c.ecx, eax = c.eax;
  for (int pass = 0; pass < 2; pass++) {
    Wr32(size, 0);
    Wr32(size + 4, 0);
    Wr32(size + 8, pass ? uint32_t(r.r - r.l) : 0);
    Wr32(size + 12, pass ? uint32_t(r.b - r.t) : 0);
    c.ecx = w;
    CallGuest(c, kMapResize, {size});
  }
  c.ecx = ecx;
  c.eax = eax;
  MoveWindow(c, w, r);
}

// Puts the widened frame's children back where its 1280 layout had them
// (before the game lays the frame out again), the frame and the top window
// at the layout rectangle.
void Unwiden(::recomp::Ctx& c, uint32_t frame) {
  const uint32_t top = Rd32(kTopWindow);
  const Rect lay = Layout();
  if (top) SetRectRaw(top, lay);
  SetRectRaw(frame, lay);
  ForChildren(frame, [&](uint32_t w) {
    auto it = g_layout_rects.find(w);
    if (it == g_layout_rects.end()) return;
    if (IsAdventureMap(w))
      MoveMapWindow(c, w, it->second);
    else
      MoveWindow(c, w, it->second);
  });
  g_layout_rects.clear();
  g_tiled.clear();
  g_widened = 0;
}

void WideAdventure(::recomp::Ctx& c) {
  const Screen& s = TheScreen();
  const uint32_t top = Rd32(kTopWindow);
  if (!Wide() || !top) return;
  const Rect lay = Layout();
  const uint32_t frame = AdventureFrame(top);
  if (!frame) {
    // Centred screens (main menu, or combat over the hidden map, which
    // stays widened for when it is shown again): the top window spans the
    // screen all the same, so that the game's cursor (its child) shows
    // over the wings. A move of its origin shifts what the offscreen holds:
    // all of it is painted again.
    const Rect full{0, lay.t, s.w, lay.b};
    if (RectOf(top).l != full.l || RectOf(top).r != full.r) {
      SetRectRaw(top, full);
      InvalidateWhole(c, top, s.w, lay.b - lay.t);
    }
    return;
  }
  const Rect full{0, lay.t, s.w, lay.b};
  if (frame == g_widened) {
    // Laid out wide already. The top window may have gone back to the
    // layout rectangle meanwhile (a screen over the map, combat, WM_SIZE
    // when the game is reactivated): it would clip the frame's sides.
    if (RectOf(top).l != full.l || RectOf(top).r != full.r) {
      SetRectRaw(top, full);
      g_refresh_map = 1;
      InvalidateWhole(c, frame, s.w, lay.b - lay.t);
    }
    g_widened = frame;
    // The map window paints from a cache built for its old size: once it
    // has settled, have it repaint whole.
    if (g_refresh_map > 0 && --g_refresh_map == 0) {
      ForChildren(frame, [&](uint32_t w) {
        if (!IsAdventureMap(w)) return;
        const Rect r = RectOf(w);
        const gaddr rect = ScratchRect();
        Wr32(rect, 0);
        Wr32(rect + 4, 0);
        Wr32(rect + 8, uint32_t(r.r - r.l));
        Wr32(rect + 12, uint32_t(r.b - r.t));
        const uint32_t ecx = c.ecx, eax = c.eax;
        c.ecx = w;
        CallGuest(c, kInvalidate, {rect});
        c.ecx = ecx;
        c.eax = eax;
      });
    }
    return;
  }
  g_tiled.clear();
  const Rect f = RectOf(frame);
  if (f.r - f.l != kLayoutW) return;  // not laid out yet, or ours already
  const int dx = lay.l;               // how far each side grows
  SetRectRaw(top, full);
  SetRectRaw(frame, full);
  g_layout_rects.clear();
  ForChildren(frame, [&](uint32_t w) {
    const Rect r = RectOf(w);
    g_layout_rects[w] = r;
    const int rl = r.l - f.l, rr = r.r - f.l;  // in the 1280 layout
    static const bool dump = Env("DUMP_WINDOWS") != nullptr;
    if (dump)
      WarnF("h4: widen %08X vt %08X %d,%d-%d,%d vis %u\n", w, Rd32(w), rl, r.t, rr, r.b,
            GPtr(w + kVisible)[0]);
    if (IsAdventureMap(w)) {
      // Its own resize (vtable +0x44) only acts on a size that differs from
      // its rectangle, and window::move calls it after changing that: call
      // it first.
      MoveMapWindow(c, w, {r.l - dx, r.t, r.r + dx, r.b});
    } else if (rl >= 880) {  // the map's right edge, the panel
      MoveWindow(c, w, {r.l + dx, r.t, r.r + dx, r.b});
    } else if (rr <= 58) {  // the left edge
      MoveWindow(c, w, {r.l - dx, r.t, r.r - dx, r.b});
    } else if (r.b - r.t <= 16) {  // the thin top and bottom bars: repeated
      g_tiled.insert(w);
      MoveWindow(c, w, {r.l - dx, r.t, r.r + dx, r.b});
    } else {  // the ornaments by the left corners
      MoveWindow(c, w, {r.l - dx, r.t, r.r - dx, r.b});
    }
  });
  g_refresh_map = 3;
  g_widened = frame;
  InvalidateWhole(c, frame, s.w, lay.b - lay.t);
}

// Runs a screen constructor with the top window, and its children wider
// than the layout (the widened adventure frame), at the layout rectangle:
// the constructors pick their art by their parent's width (800, 1024 or
// 1280) and use an unset layout for any other (the town screen crashed).
int g_narrowed = 0;  // screen constructors running narrowed (KeepWide waits)

void Narrowed(::recomp::Ctx& c, void (*lifted)(::recomp::Ctx&)) {
  const uint32_t top = Rd32(kTopWindow);
  std::vector<std::pair<uint32_t, Rect>> saved;
  if (Wide() && top) {
    auto narrow = [&](uint32_t w) {
      const Rect r = RectOf(w);
      if (r.r - r.l <= kLayoutW) return;
      saved.push_back({w, r});
      SetRectRaw(w, Layout());
    };
    narrow(top);
    ForChildren(top, narrow);
  }
  g_narrowed++;
  lifted(c);
  g_narrowed--;
  for (const auto& [w, r] : saved) SetRectRaw(w, r);
}

// ---- wings: the side areas left by the centred 1280 screens (main menu,
// towns, combat) show two
// panels (H4_WINGS_DIR/paradise.bmp left, hell.bmp right: 24-bit BMPs, any
// size, scaled to the side's width and the layout's height), drawn straight
// onto the primary surface and the game's offscreen, which it never paints
// outside its windows.

constexpr uint32_t kCombatWindowVtable = 0x984388;  // t_combat_window
constexpr uint32_t kTownWindowVtable = 0x9966E4;    // t_town_window
constexpr uint32_t kCursorWindowVtable = 0x98D174;  // the game's cursor, a window under the top one
constexpr uint32_t kPrimarySurface = 0xAD2548;      // IDirectDrawSurface7*
constexpr uint32_t kBackSurface = 0xAD254C;         // the offscreen the game composes in
constexpr uint32_t kLockSlot = 0x64, kUnlockSlot = 0x80;  // its vtable

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;  // top-down
};

bool LoadBmp(const std::string& path, Image* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  auto u32 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24); };
  if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
  const uint32_t off = u32(10);
  const int w = int32_t(u32(18)), hs = int32_t(u32(22));
  const int bpp = d[28] | d[29] << 8;
  if (w <= 0 || hs == 0 || (bpp != 24 && bpp != 32) || u32(30) != 0) return false;
  const int h = hs < 0 ? -hs : hs;
  const size_t bytes = size_t(bpp / 8), stride = (size_t(w) * bytes + 3) & ~size_t(3);
  if (off + stride * size_t(h) > d.size()) return false;
  out->w = w;
  out->h = h;
  out->rgb.resize(size_t(w) * h * 3);
  for (int y = 0; y < h; y++) {
    const uint8_t* row = &d[off + stride * size_t(hs < 0 ? y : h - 1 - y)];
    for (int x = 0; x < w; x++) {
      uint8_t* o = &out->rgb[(size_t(y) * w + x) * 3];
      o[0] = row[x * bytes + 2];
      o[1] = row[x * bytes + 1];
      o[2] = row[x * bytes + 0];
    }
  }
  return true;
}

// `src` scaled to w x h (box filter over each output pixel's source area),
// as RGB.
std::vector<uint8_t> Scale(const Image& src, int w, int h) {
  std::vector<uint8_t> out(size_t(w) * h * 3);
  for (int y = 0; y < h; y++) {
    const int y0 = y * src.h / h, y1 = std::max(y0 + 1, (y + 1) * src.h / h);
    for (int x = 0; x < w; x++) {
      const int x0 = x * src.w / w, x1 = std::max(x0 + 1, (x + 1) * src.w / w);
      uint32_t sum[3] = {0, 0, 0}, cnt = 0;
      for (int sy = y0; sy < y1; sy++)
        for (int sx = x0; sx < x1; sx++) {
          const uint8_t* p = &src.rgb[(size_t(sy) * src.w + sx) * 3];
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          cnt++;
        }
      uint8_t* o = &out[(size_t(y) * w + x) * 3];
      for (int k = 0; k < 3; k++) o[k] = uint8_t(sum[k] / cnt);
    }
  }
  return out;
}

struct Wings {
  bool tried = false, ok = false;
  int w = 0, h = 0;
  std::vector<uint8_t> left, right;  // RGB, w x h
};

// The wings at their own resolution, drawn over the frame at the window's
// wherever it still shows them as PaintWingsOn draws them (RGB565).
void ShowWingsSharp(const Wings& wings, const Image& l, const Image& r) {
  const Screen& s = TheScreen();
  const Rect lay = Layout();
  std::vector<::recomp::gfx::PresentOverlay> ov(2);
  for (int side = 0; side < 2; side++) {
    const Image& img = side ? r : l;
    const std::vector<uint8_t>& small = side ? wings.right : wings.left;
    ::recomp::gfx::PresentOverlay& o = ov[size_t(side)];
    o.x = side ? s.w - wings.w : 0;
    o.y = lay.t;
    o.w = wings.w;
    o.h = wings.h;
    o.iw = img.w;
    o.ih = img.h;
    o.image.resize(size_t(img.w) * img.h);
    for (size_t i = 0; i < o.image.size(); i++)
      o.image[i] = 0xFF000000u | uint32_t(img.rgb[i * 3]) << 16 |
                   uint32_t(img.rgb[i * 3 + 1]) << 8 | img.rgb[i * 3 + 2];
    o.ref.resize(size_t(wings.w) * wings.h);
    for (size_t i = 0; i < o.ref.size(); i++) {
      const uint8_t* p = &small[i * 3];
      const uint16_t v = uint16_t((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
      o.ref[i] = ::recomp::gfx::DecodePixel(reinterpret_cast<const uint8_t*>(&v),
                                            ::recomp::gfx::Format::R5G6B5);
    }
  }
  ::recomp::gfx::SetPresentOverlays(std::move(ov));
}

// The wings shown: H4_WINGS_DIR/<name>.bmp, the right one mirrored when
// `mirror_right` (combat's faction wings face the battle). The main menu and
// towns: paradise and hell.
std::string g_wing_left = "paradise", g_wing_right = "hell";
bool g_mirror_right = false;

void Mirror(Image* im) {
  for (int y = 0; y < im->h; y++)
    for (int x = 0; x < im->w / 2; x++)
      for (int k = 0; k < 3; k++)
        std::swap(im->rgb[(size_t(y) * im->w + x) * 3 + k],
                  im->rgb[(size_t(y) * im->w + (im->w - 1 - x)) * 3 + k]);
}

const Wings& TheWings(int w, int h) {
  static Wings wings;
  static std::string shown;  // the pair wings holds
  const std::string want = g_wing_left + "|" + g_wing_right + (g_mirror_right ? "|m" : "");
  if (shown == want && wings.tried) return wings;
  shown = want;
  wings.tried = true;
  const char* dir = Env("WINGS_DIR");
  Image l, r;
  if (dir && LoadBmp(std::string(dir) + "/" + g_wing_left + ".bmp", &l) &&
      LoadBmp(std::string(dir) + "/" + g_wing_right + ".bmp", &r) && w > 0) {
    if (g_mirror_right) Mirror(&r);
    wings.w = w;
    wings.h = h;
    wings.left = Scale(l, w, h);
    wings.right = Scale(r, w, h);
    wings.ok = true;
    ShowWingsSharp(wings, l, r);
  } else if (dir) {
    WarnF("h4: no wings %s, %s in %s\n", g_wing_left.c_str(), g_wing_right.c_str(), dir);
  }
  return wings;
}

// Draws the wings onto a surface (locked through its own vtable).
// (ox, oy): where the surface's origin is on the screen; `hole`: a screen
// area left alone, or where `black_in_hole`, painted only where black.
void PaintWingsOn(::recomp::Ctx& c, uint32_t prim, int ox, int oy, const Rect& hole,
                  bool black_in_hole = false);

// The wings go into the game's offscreen, which its present and its cursor
// restore copy to the screen (its origin is the top window's), and with
// `primary` straight onto the primary surface: the sides of a centred
// screen lie outside the top window, which the game never presents.
void PaintWings(::recomp::Ctx& c, bool primary) {
  const uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx;
  const uint32_t top = Rd32(kTopWindow);
  if (top) PaintWingsOn(c, Rd32(kBackSurface), RectOf(top).l, RectOf(top).t, Rect{});
  if (primary && top) {
    // All of it (a hole round the cursor kept whatever was there, the
    // adventure panel say), then the cursor drawn again over it.
    PaintWingsOn(c, Rd32(kPrimarySurface), 0, 0, Rect{});
    ForChildren(top, [&](uint32_t w) {
      if (Rd32(w) != kCursorWindowVtable) return;
      const Rect r = RectOf(w);
      InvalidateWhole(c, w, r.r - r.l, r.b - r.t);
    });
  }
  c.eax = eax;
  c.ecx = ecx;
  c.edx = edx;
}

void PaintWingsOn(::recomp::Ctx& c, uint32_t prim, int ox, int oy, const Rect& hole,
                  bool black_in_hole) {
  const Screen& s = TheScreen();
  const Rect lay = Layout();
  const Wings& wings = TheWings(lay.l, lay.b - lay.t);
  if (!wings.ok || !prim) return;
  const uint32_t vt = Rd32(prim);
  const gaddr desc = ScratchRect() + 0x100;
  std::memset(GPtr(desc), 0, 124);
  Wr32(desc, 124);
  const uint32_t ecx = c.ecx, eax = c.eax;
  const uint32_t hr = uint32_t(CallGuest(c, Rd32(vt + kLockSlot), {prim, 0, desc, 1, 0}));
  if (hr == 0) {
    uint8_t* bits = GPtr(Rd32(desc + 36));
    const int32_t pitch = int32_t(Rd32(desc + 16));
    const int bpp = int(Rd32(desc + 72 + 12));
    const int sw = int(Rd32(desc + 12)), sh = int(Rd32(desc + 8));
    for (int side = 0; side < 2; side++) {
      const std::vector<uint8_t>& img = side ? wings.right : wings.left;
      const int x0 = side ? s.w - wings.w : 0;
      for (int y = 0; y < wings.h; y++) {
        const int sy = lay.t + y - oy;
        if (sy < 0 || sy >= sh) continue;
        uint8_t* row = bits + int64_t(sy) * pitch;
        const bool in_hole_rows = lay.t + y >= hole.t && lay.t + y < hole.b;
        for (int x = 0; x < wings.w; x++) {
          const int sx = x0 + x - ox;
          if (sx < 0 || sx >= sw) continue;
          const uint8_t* p = &img[(size_t(y) * wings.w + x) * 3];
          if (in_hole_rows && x0 + x >= hole.l && x0 + x < hole.r) {
            if (!black_in_hole) continue;
            uint32_t v = 0;
            std::memcpy(&v, row + sx * (bpp / 8), size_t(bpp / 8));
            if (v) continue;
          }
          if (bpp == 16) {
            const uint16_t v = uint16_t((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
            std::memcpy(row + sx * 2, &v, 2);
          } else if (bpp == 32) {
            const uint32_t v = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
            std::memcpy(row + sx * 4, &v, 4);
          }
        }
      }
    }
    CallGuest(c, Rd32(vt + kUnlockSlot), {prim, 0});
  }
  c.ecx = ecx;
  c.eax = eax;
}

// Dev aid: H4_DUMP_WINDOWS=1 logs the window tree (vtable, rectangle,
// visibility) whenever the wings come or go.
void DumpTree(uint32_t w, int depth) {
  WarnF("h4: %*s%08X vt %08X rect %d,%d-%d,%d vis %u\n", depth * 2, "", w, Rd32(w),
        int(Rd32(w + 0x90)), int(Rd32(w + 0x94)), int(Rd32(w + 0x98)), int(Rd32(w + 0x9C)),
        GPtr(w + kVisible)[0]);
  if (depth >= 1) return;
  ForChildren(w, [&](uint32_t ch) { DumpTree(ch, depth + 1); });
}

bool g_wings = false;          // the wings are wanted

// A centred full screen shown over the adventure map: a battle or a town
// (by class), or any visible 1280 x 1024 window, right under the top window
// or inside the adventure frame (the town opens there). 0 if none.
uint32_t CentredScreen(uint32_t top) {
  uint32_t found = 0;
  auto check = [&](uint32_t w) {
    if (found || !GPtr(w + kVisible)[0] || IsAdventureFrame(w)) return;
    const uint32_t vt = Rd32(w);
    const Rect r = RectOf(w);
    if (vt == kCombatWindowVtable || vt == kTownWindowVtable ||
        (r.r - r.l == kLayoutW && r.b - r.t == kLayoutH))
      found = w;
  };
  ForChildren(top, check);
  if (const uint32_t frame = AdventureFrame(top)) ForChildren(frame, check);
  return found;
}
bool g_wings_primary = false;  // and must go onto the primary surface again

// ---- combat wings by the battlefield: a light field gets dark wings, a dark
// one light wings, both sides the same picture (the right one mirrored),
// one of the faction collages (tools/collage) picked at random per battle.
constexpr uint32_t kCombatBattlefield = 0x104;  // t_combat_window -> t_battlefield
constexpr const char* kLightWings[] = {"life_light", "order_light", "nature_light", "chaos_light",
                                       "might_light"};
constexpr const char* kDarkWings[] = {"death_dark", "order_dark", "nature_dark", "chaos_dark",
                                      "might_dark"};

// The battlefield's mean luma (0..255) on the primary surface, sampled over
// the layout's left four fifths (the right one is the combat panel); -1
// when it cannot be read.
int FieldLuma(::recomp::Ctx& c) {
  const uint32_t prim = Rd32(kPrimarySurface);
  if (!prim) return -1;
  const Rect lay = Layout();
  const uint32_t vt = Rd32(prim);
  const gaddr desc = ScratchRect() + 0x100;
  std::memset(GPtr(desc), 0, 124);
  Wr32(desc, 124);
  const uint32_t ecx = c.ecx, eax = c.eax, edx = c.edx;
  int luma = -1;
  if (uint32_t(CallGuest(c, Rd32(vt + kLockSlot), {prim, 0, desc, 1, 0})) == 0) {
    const uint8_t* bits = GPtr(Rd32(desc + 36));
    const int32_t pitch = int32_t(Rd32(desc + 16));
    const int bpp = int(Rd32(desc + 72 + 12));
    const int sw = int(Rd32(desc + 12)), sh = int(Rd32(desc + 8));
    uint64_t sum = 0, n = 0;
    const int x1 = std::min(sw, lay.l + (lay.r - lay.l) * 4 / 5);
    for (int y = std::max(0, lay.t); y < std::min(sh, lay.b); y += 7)
      for (int x = std::max(0, lay.l); x < x1; x += 7) {
        int r, g, b;
        if (bpp == 16) {
          uint16_t v;
          std::memcpy(&v, bits + int64_t(y) * pitch + x * 2, 2);
          r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
        } else {
          const uint8_t* q = bits + int64_t(y) * pitch + x * (bpp / 8);
          b = q[0], g = q[1], r = q[2];
        }
        sum += uint64_t(r * 77 + g * 150 + b * 29) >> 8;
        n++;
      }
    luma = n ? int(sum / n) : -1;
    CallGuest(c, Rd32(vt + kUnlockSlot), {prim, 0});
  }
  c.ecx = ecx;
  c.eax = eax;
  c.edx = edx;
  return luma;
}

// Once per battle, when its field is on screen: the wings for its armies.
// Back to the default pair outside combat.
void ChooseWings(::recomp::Ctx& c) {
  static uint32_t decided = 0;  // the combat window chosen for
  const uint32_t top = Rd32(kTopWindow);
  uint32_t combat = 0;
  if (Wide() && top)
    ForChildren(top, [&](uint32_t w) {
      if (Rd32(w) == kCombatWindowVtable && GPtr(w + kVisible)[0]) combat = w;
    });
  std::string left = "paradise", right = "hell";
  bool mirror = false;
  if (!combat) {
    decided = 0;
  } else if (combat == decided) {
    return;
  } else {
    const uint32_t field = Rd32(combat + kCombatBattlefield);
    if (!field) return;
    const int luma = FieldLuma(c);
    if (luma < 12) return;  // not drawn yet
    const bool dark = luma < 110;
    static std::mt19937 rng(std::random_device{}());
    left = right = dark ? kLightWings[rng() % 5] : kDarkWings[rng() % 5];
    mirror = true;
    decided = combat;
    WarnF("h4: battle wings: field luma %d: %s\n", luma, left.c_str());
  }
  if (left == g_wing_left && right == g_wing_right && mirror == g_mirror_right) return;
  g_wing_left = left;
  g_wing_right = right;
  g_mirror_right = mirror;
  g_wings_primary = true;  // painted again over the old ones
}

// On the centred screens (the main menu, combat: no adventure frame shown;
// a town or combat window over it), wide: the wings. The paint pass keeps
// them in the offscreen; onto the primary surface (and the offscreen) only
// when they appear or the screen was redone: painting them there over and
// over made the cursor over them blink.
void SideWings(::recomp::Ctx& c) {
  ChooseWings(c);
  static uint32_t last_primary = 0;
  const uint32_t top = Rd32(kTopWindow);
  const bool on = Wide() && top && (!AdventureFrame(top) || CentredScreen(top));
  const bool was = g_wings;
  g_wings = on;
  if (was != on) WarnF("h4: side wings %s\n", on ? "on" : "off");
  // Clicks on the wings go nowhere (the pointer moves over them freely).
  if (was != on) {
    const Rect lay = Layout();
    SetPointerBounds(on, lay.l, lay.t, lay.r, lay.b);
  }
  static const bool dump = Env("DUMP_WINDOWS") != nullptr;
  if (was != on && dump && top) DumpTree(top, 0);
  if (!on) {
    // Whatever replaces the wings (the adventure frame back under a closed
    // town or battle) repaints only what the game thinks changed, which is
    // not the sides it had there all along: repaint the whole screen.
    if (was && top) {
      const Rect r = RectOf(top);
      InvalidateWhole(c, top, r.r - r.l, r.b - r.t);
    }
    return;
  }
  const uint32_t primary = Rd32(kPrimarySurface);
  if (!was || primary != last_primary) g_wings_primary = true;
  last_primary = primary;
  if (!g_wings_primary) return;
  PaintWings(c, true);
  g_wings_primary = false;
}

// Screens and dialogs right under the top window, placed for a top window
// at the layout rectangle but put there while it spanned the screen, sit
// exactly the layout's offset left of centre (combat: built without a
// parent and attached later, at x 0): they go back to the middle. Other
// windows off centre are logged once (H4_DUMP_WINDOWS), to find more.
void CentreScreens(::recomp::Ctx& c) {
  static std::set<uint32_t> logged;
  static const bool dump = Env("DUMP_WINDOWS") != nullptr;
  const uint32_t top = Rd32(kTopWindow);
  if (!Wide() || !top) return;
  const Rect lay = Layout();
  const int mid = TheScreen().w / 2;
  ForChildren(top, [&](uint32_t w) {
    const uint32_t vt = Rd32(w);
    if (vt == kCursorWindowVtable || IsAdventureFrame(w) || !GPtr(w + kVisible)[0]) return;
    const Rect r = RectOf(w);
    const int width = r.r - r.l, centre = (r.l + r.r) / 2;
    if (width >= 300 && std::abs(centre - (mid - lay.l)) <= 1) {
      WarnF("h4: window %08X (vt %08X) at x %d, %d wide: moved right by %d\n", w, vt, r.l, width,
            lay.l);
      MoveWindow(c, w, {r.l + lay.l, r.t, r.r + lay.l, r.b});
      InvalidateWhole(c, w, width, r.b - r.t);
    } else if (dump && width >= 300 && std::abs(centre - mid) > 1 && logged.insert(w).second) {
      WarnF("h4: off-centre window %08X (vt %08X) %d,%d-%d,%d\n", w, vt, r.l, r.t, r.r, r.b);
    }
  });
  // With the wings shown, popups (right-click help, creature info) kept on
  // the screen by the game would run under them: they go inside the
  // layout rectangle.
  if (!g_wings) return;
  const uint32_t screen = CentredScreen(top);
  ForChildren(top, [&](uint32_t w) {
    if (w == screen || Rd32(w) == kCursorWindowVtable || IsAdventureFrame(w) ||
        !GPtr(w + kVisible)[0])
      return;
    const Rect r = RectOf(w);
    if (r.r - r.l > kLayoutW) return;
    const int dx = r.l < lay.l ? lay.l - r.l : r.r > lay.r ? lay.r - r.r : 0;
    if (!dx) return;
    MoveWindow(c, w, {r.l + dx, r.t, r.r + dx, r.b});
    InvalidateWhole(c, w, r.r - r.l, r.b - r.t);
  });
}

// The wide layout kept up and the wings drawn, from every loop the game
// runs (after a battle it runs one that never calls the main loop pass).
void KeepWide(::recomp::Ctx& c) {
  // A constructor presenting (a loading screen) while the top window is
  // narrowed for it: leave it so until it returns.
  if (g_narrowed) return;
  const uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx, esp = c.esp;
  WideAdventure(c);
  CentreScreens(c);
  SideWings(c);
  c.eax = eax;
  c.ecx = ecx;
  c.edx = edx;
  c.esp = esp;
}

}  // namespace

void PaintDirty(::recomp::Ctx& c);

namespace {

// ---- the pointer drawn over the frame (H4_GPU_CURSOR=0: the game's own)
//
// The game's cursor is a window (t_mouse_window, an animated window in it)
// at kCursorWindowVtable. Every move of it ran a paint pass, and every pass
// recomputes the visible region of every window (0x71FA90): 15-30 ms in a
// town or under a combat dialog, so the pointer lagged wherever windows
// were open. Here the window stays invisible to the game (moving it paints
// nothing) and its picture goes over the frame (gfx::SetPresentCursor),
// where the game keeps the window. The picture is taken from the game when
// its sprite changes, once per sprite: one paint pass of the cursor's area
// without it and one with it, the pixels that differ being the cursor.
// (Animated cursors keep their first frame.)
bool GpuCursorOn() {
  static const bool on = [] {
    const char* e = Env("GPU_CURSOR");
    return !e || *e != '0';
  }();
  return on;
}

// The cursor window: the newest, should there be several.
uint32_t CursorWindow(uint32_t top) {
  uint32_t cw = 0;
  ForChildren(top, [&](uint32_t w) {
    if (Rd32(w) == kCursorWindowVtable) cw = w;
  });
  return cw;
}

// Every cursor window invisible to the game.
void HideCursors(uint32_t top) {
  ForChildren(top, [](uint32_t w) {
    if (Rd32(w) == kCursorWindowVtable) GPtr(w + kVisible)[0] = 0;
  });
}

// The offscreen's pixels in r (16-bit), or none.
std::vector<uint16_t> ReadOffscreen(::recomp::Ctx& c, const Rect& r) {
  std::vector<uint16_t> out;
  const uint32_t surf = Rd32(kBackSurface);
  if (!surf || r.r <= r.l || r.b <= r.t) return out;
  const uint32_t vt = Rd32(surf);
  const gaddr desc = ScratchRect() + 0x300;
  std::memset(GPtr(desc), 0, 124);
  Wr32(desc, 124);
  const uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx, esp = c.esp;
  if (uint32_t(CallGuest(c, Rd32(vt + kLockSlot), {surf, 0, desc, 1, 0})) == 0) {
    const uint8_t* bits = GPtr(Rd32(desc + 36));
    const int32_t pitch = int32_t(Rd32(desc + 16));
    const int sw = int(Rd32(desc + 12)), sh = int(Rd32(desc + 8));
    if (int(Rd32(desc + 72 + 12)) == 16 && r.l >= 0 && r.t >= 0 && r.r <= sw && r.b <= sh) {
      out.resize(size_t(r.r - r.l) * (r.b - r.t));
      for (int y = r.t; y < r.b; y++)
        std::memcpy(&out[size_t(y - r.t) * (r.r - r.l)], bits + int64_t(y) * pitch + r.l * 2,
                    size_t(r.r - r.l) * 2);
    }
    CallGuest(c, Rd32(vt + kUnlockSlot), {surf, 0});
  }
  c.eax = eax;
  c.ecx = ecx;
  c.edx = edx;
  c.esp = esp;
  return out;
}

// Marks r (screen coordinates) for the next paint pass.
void InvalidateArea(::recomp::Ctx& c, uint32_t top, const Rect& r) {
  const Rect t = RectOf(top);
  const gaddr rect = ScratchRect() + 0x280;
  Wr32(rect, uint32_t(r.l - t.l));
  Wr32(rect + 4, uint32_t(r.t - t.t));
  Wr32(rect + 8, uint32_t(r.r - t.l));
  Wr32(rect + 12, uint32_t(r.b - t.t));
  const uint32_t ecx = c.ecx, eax = c.eax;
  c.ecx = top;
  CallGuest(c, kInvalidate, {rect});
  c.ecx = ecx;
  c.eax = eax;
}

// The paint pass once more (for the cursor's area), the stack as on entry.
void PaintAgain(::recomp::Ctx& c) {
  const uint32_t esp = c.esp, eax = c.eax;
  PaintDirty(c);
  c.esp = esp;
  c.eax = eax;
}

// Before a paint pass: every cursor window invisible to the game; the
// picture of the newest taken again when its sprite or size changed.
void GpuCursorBeforePaint(::recomp::Ctx& c) {
  const uint32_t top = Rd32(kTopWindow);
  const uint32_t cw = top ? CursorWindow(top) : 0;
  if (!cw) return;
  uint32_t aw = 0;
  ForChildren(cw, [&](uint32_t w) {
    if (!aw) aw = w;
  });
  const Rect r = RectOf(cw);
  const int w = r.r - r.l, h = r.b - r.t;
  const uint32_t sprite = aw ? Rd32(aw + 0xD8) : 0;
  static uint32_t last_sprite = 0;
  static int last_w = -1, last_h = -1;
  static std::map<uint64_t, std::vector<uint32_t>> taken;  // by sprite and size
  if (sprite != last_sprite || w != last_w || h != last_h) {
    last_sprite = sprite;
    last_w = w;
    last_h = h;
    const uint64_t key = uint64_t(sprite) << 32 | uint32_t(w) << 16 | uint32_t(h);
    auto it = taken.find(key);
    if (it == taken.end()) {
      // Without the cursor, then with it, over its area; then without again.
      HideCursors(top);
      InvalidateArea(c, top, r);
      PaintAgain(c);
      const std::vector<uint16_t> bg = ReadOffscreen(c, r);
      GPtr(cw + kVisible)[0] = 1;
      InvalidateArea(c, top, r);
      PaintAgain(c);
      const std::vector<uint16_t> fg = ReadOffscreen(c, r);
      HideCursors(top);
      InvalidateArea(c, top, r);
      PaintAgain(c);
      if (fg.empty() || fg.size() != bg.size()) return;
      std::vector<uint32_t> argb(fg.size());
      for (size_t i = 0; i < fg.size(); i++) {
        if (fg[i] == bg[i]) continue;
        const uint32_t v = fg[i];
        argb[i] = 0xFF000000u | ((v >> 11) & 31) * 255 / 31 << 16 |
                  ((v >> 5) & 63) * 255 / 63 << 8 | (v & 31) * 255 / 31;
      }
      it = taken.emplace(key, std::move(argb)).first;
    }
    ::recomp::gfx::SetPresentCursor(it->second.data(), w, h);
  }
  HideCursors(top);
}

// From the main loop pass and the present: the pointer where the game keeps
// the cursor window; none while there is none (an army being dragged: the
// game takes the window away and moves the army's picture instead).
void GpuCursorPlace() {
  const uint32_t top = Rd32(kTopWindow);
  const uint32_t cw = top ? CursorWindow(top) : 0;
  const Rect r = cw ? RectOf(cw) : Rect{};
  static int last_x = -1, last_y = -1;
  static bool last_shown = false;
  if (!!cw == last_shown && (!cw || (r.l == last_x && r.t == last_y))) return;
  last_x = r.l;
  last_y = r.t;
  last_shown = cw != 0;
  ::recomp::gfx::SetPresentCursorPos(r.l, r.t, cw != 0);
  ::recomp::hle::PresentAgain();  // the game drew nothing
}

}  // namespace

// Paints the windows' dirty region into the offscreen and returns it
// (RECT*), the cursor drawn by the GPU (above).
void f_00720530(::recomp::Ctx& c) {
  if (GpuCursorOn()) GpuCursorBeforePaint(c);
  PaintDirty(c);
}

// The paint pass. With the wings shown, the adventure frame under a town or
// battle sits the pass out (the game clears what it repaints to black and
// draws the windows there), and the wings go back over the side columns
// after it: under the game's cursor (a window of its own) only onto the
// black round its shape.
void PaintDirty(::recomp::Ctx& c) {
  const uint32_t top = Rd32(kTopWindow);
  if (!g_wings || !top) {
    f_00720530_lifted(c);
    return;
  }
  const uint32_t out = Rd32(c.esp + 4);
  // Under the screen: the adventure frame, or (a screen inside it) the
  // frame's other children.
  // A screen inside the frame: the frame's other children skip the pass,
  // and the frame itself is cut to the layout rectangle for it, so that
  // nothing of it lands in the side columns (under the cursor there, where
  // the wings go only onto black).
  std::vector<uint32_t> hidden;
  uint32_t narrowed = 0;
  Rect frame_rect{};
  if (const uint32_t frame = AdventureFrame(top)) {
    const uint32_t screen = CentredScreen(top);
    if (screen && Rd32(screen + 0x7C) == frame) {
      // Only the frame's windows that were there when the screen came: the
      // ones under it. A window made later is over it, a dialog opened from
      // the town (creature info on a double click), which hidden was never
      // drawn: the town froze, the dialog waiting for a click nobody saw.
      static uint32_t under_screen = 0;
      static std::set<uint32_t> under;
      if (screen != under_screen) {
        under_screen = screen;
        under.clear();
        ForChildren(frame, [&](uint32_t w) {
          if (w != screen) under.insert(w);
        });
      }
      ForChildren(frame, [&](uint32_t w) {
        if (w != screen && GPtr(w + kVisible)[0] && under.count(w)) hidden.push_back(w);
      });
      narrowed = frame;
      frame_rect = RectOf(frame);
    } else {
      hidden.push_back(frame);
    }
  }
  for (uint32_t w : hidden) GPtr(w + kVisible)[0] = 0;
  if (narrowed) SetRectRaw(narrowed, Layout());
  f_00720530_lifted(c);
  if (narrowed) SetRectRaw(narrowed, frame_rect);
  for (uint32_t w : hidden) GPtr(w + kVisible)[0] = 1;
  const Rect lay = Layout();
  const int l = out ? int32_t(Rd32(out)) : 0, r = out ? int32_t(Rd32(out + 8)) : 0;
  if (r <= l || (l >= lay.l && r <= lay.r)) return;
  uint32_t cursor = 0;
  ForChildren(top, [&](uint32_t w) {
    if (Rd32(w) == kCursorWindowVtable) cursor = w;
  });
  const uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx, esp = c.esp;
  const Rect t = RectOf(top);
  PaintWingsOn(c, Rd32(kBackSurface), t.l, t.t, cursor ? RectOf(cursor) : Rect{}, true);
  c.eax = eax;
  c.ecx = ecx;
  c.edx = edx;
  c.esp = esp;
}

// The game's present (dirty region to the screen).
void f_008D0C70(::recomp::Ctx& c) {
  KeepWide(c);
  f_008D0C70_lifted(c);
  if (GpuCursorOn()) GpuCursorPlace();  // its own loops (a drag) present here
}

void f_008D1360(::recomp::Ctx& c) {
  KeepWide(c);
  if (GpuCursorOn()) GpuCursorPlace();
  f_008D1360_lifted(c);
}

// bitmap_layer_window::paint(paint_context*): draws the part of its bitmap
// under the context's clip rectangle ([7..10], with the window's origin at
// [2], [3], all in target coordinates). A repeated layer is drawn one
// bitmap width at a time, each piece as if the window started there.
void f_00598170(::recomp::Ctx& c) {
  const uint32_t w = c.ecx;
  if (!g_tiled.count(w) || !Rd32(w + 0xC4)) return f_00598170_lifted(c);
  const uint32_t bitmap = Rd32(w + 0xC4);
  const int period = int32_t(Rd32(bitmap + 0x20)) - int32_t(Rd32(bitmap + 0x18));
  const uint32_t ctx = Rd32(c.esp + 4);
  if (period <= 0) return f_00598170_lifted(c);
  const int ox = int32_t(Rd32(ctx + 8)), cl = int32_t(Rd32(ctx + 28)), cr = int32_t(Rd32(ctx + 36));
  const int a = cl - ox, b = cr - ox;  // window-local columns to paint
  const uint32_t esp = c.esp;
  bool painted = false;
  for (int k = a >= 0 ? a / period : -((-a + period - 1) / period); k * period < b; k++) {
    const int l = std::max(a, k * period), r = std::min(b, (k + 1) * period);
    if (l >= r) continue;
    Wr32(ctx + 8, uint32_t(ox + k * period));
    Wr32(ctx + 28, uint32_t(ox + l));
    Wr32(ctx + 36, uint32_t(ox + r));
    c.ecx = w;
    c.esp = esp;
    f_00598170_lifted(c);
    painted = true;
  }
  if (!painted) c.esp = esp + 8;  // its `ret 4`
  Wr32(ctx + 8, uint32_t(ox));
  Wr32(ctx + 28, uint32_t(cl));
  Wr32(ctx + 36, uint32_t(cr));
}

// adventure_frame::layout(width), thiscall: its window's resize (vtable
// +0x44) lays the frame out anew for an 800, 1024 or 1280 wide screen,
// e.g. when the game is reactivated and sets its mode again. Wide, it lays
// out the centred 1280 layout, and the next main loop pass widens it again.
void f_004ACBB0(::recomp::Ctx& c) {
  const uint32_t frame = c.ecx;
  const uint32_t top = Rd32(kTopWindow);
  if (!Wide() || !top) return f_004ACBB0_lifted(c);
  Wr32(c.esp + 4, uint32_t(kLayoutW));
  if (frame == g_widened) {
    Unwiden(c, frame);
  } else {
    SetRectRaw(top, Layout());
    SetRectRaw(frame, Layout());
  }
  f_004ACBB0_lifted(c);
  // The next main loop pass widens it again.
}

void f_004A3450(::recomp::Ctx& c) { Narrowed(c, f_004A3450_lifted); }
void f_004A37C0(::recomp::Ctx& c) { Narrowed(c, f_004A37C0_lifted); }
void f_0062B6A0(::recomp::Ctx& c) { Narrowed(c, f_0062B6A0_lifted); }
void f_0069FB50(::recomp::Ctx& c) { Narrowed(c, f_0069FB50_lifted); }
void f_008AC0C0(::recomp::Ctx& c) { Narrowed(c, f_008AC0C0_lifted); }
// main_menu::layout(int width): picks its art by the width it is given (a
// rectangle's, the screen's when the top window spans it).
void f_0076D9B0(::recomp::Ctx& c) {
  if (int32_t(Rd32(c.esp + 4)) > kLayoutW) Wr32(c.esp + 4, uint32_t(kLayoutW));
  Narrowed(c, f_0076D9B0_lifted);
}
void f_0076DEB0(::recomp::Ctx& c) { Narrowed(c, f_0076DEB0_lifted); }
void f_007CA0D0(::recomp::Ctx& c) { Narrowed(c, f_007CA0D0_lifted); }  // t_options_dialog

// window::window(const RECT* rect, window* parent), thiscall: no rectangle
// means the parent's whole area. Screens made that way right under the top
// window (combat, the adventure frame) take the layout rectangle, not the
// screen: combat built while the top window spanned it came out at x 0.
void f_0071E6D0(::recomp::Ctx& c) {
  const uint32_t top = Rd32(kTopWindow);
  if (Rd32(c.esp + 4) == 0 && top && Rd32(c.esp + 8) == top)
    Narrowed(c, f_0071E6D0_lifted);
  else
    f_0071E6D0_lifted(c);
}

// read_setting(const char* name, uint32_t* out, bool user) -> found. Wide,
// "resolution" (the screen width the game picks its layout and buffers
// by) reads 1024 whatever was saved: the Options dialog, which shows the
// 1280 layout, saves 1280, and with that the adventure map keeps the width
// it is made with and never renders the widened part (black up to the
// panel). set_mode above decides the real size anyway.
void f_0063B840(::recomp::Ctx& c) {
  const uint32_t name = Rd32(c.esp + 4), out = Rd32(c.esp + 8);
  f_0063B840_lifted(c);
  if (!TheScreen().wide || !(c.eax & 0xFF) || !out || !name) return;
  if (GStr(name) == "resolution") Wr32(out, 1024);
}

void f_008D3750(::recomp::Ctx& c) {
  const uint32_t args = c.esp + 4;  // width, height, repaint, full_screen
  const Screen& s = TheScreen();
  // Windowed too (the game's full_screen setting off): the host shows the
  // window like a full screen, fitted to the display.
  if (s.w) {
    Wr32(args, uint32_t(s.w));
    Wr32(args + 4, uint32_t(s.h));
  }
  f_008D3750_lifted(c);
  // Applying the options sets the mode again: the new surfaces are blank,
  // and the game repaints only what changes next (the main menu stayed
  // black but under the cursor).
  if (const uint32_t top = Rd32(kTopWindow); top && s.w) {
    const uint32_t eax = c.eax;
    const Rect r = RectOf(top);
    InvalidateWhole(c, top, r.r - r.l, r.b - r.t);
    c.eax = eax;
  }
}

// screen_rect(RECT* out) -> out: the client area in screen coordinates.
void f_008D2E20(::recomp::Ctx& c) {
  const uint32_t out = Rd32(c.esp + 4);
  f_008D2E20_lifted(c);
  if (!Wide()) return;
  int l, t;
  LayoutRect(&l, &t);
  const int x = int32_t(Rd32(out)), y = int32_t(Rd32(out + 4));
  Wr32(out, uint32_t(x + l));
  Wr32(out + 4, uint32_t(y + t));
  Wr32(out + 8, uint32_t(x + l + kLayoutW));
  Wr32(out + 12, uint32_t(y + t + kLayoutH));
}

// on_size(type, width, height): grows the back buffer and fits the top
// window to the client area; wide, the top window goes back to the layout
// rectangle.
void f_008D22D0(::recomp::Ctx& c) {
  f_008D22D0_lifted(c);
  const uint32_t top = Rd32(kTopWindow);
  if (!Wide() || !top) return;
  // The top window goes where it was: across the screen, as the main loop
  // keeps it (moved with its children, which undoes the move to the client
  // area above).
  const uint32_t eax = c.eax;
  const gaddr rect = ScratchRect();
  CallGuest(c, 0x8D2E20, {rect});  // the layout rectangle, via the replacement above
  const int dx = Layout().l;
  Wr32(rect, Rd32(rect) - uint32_t(dx));
  Wr32(rect + 8, Rd32(rect + 8) + uint32_t(dx));
  c.ecx = top;
  CallGuest(c, kSetRect, {rect, 1});
  c.eax = eax;
  g_wings_primary = true;  // reactivated: the screen was redone without them
}

}  // namespace recomp::gen
