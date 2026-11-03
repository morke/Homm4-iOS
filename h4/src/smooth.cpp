// Smoother motion at 60 updates a second. Replacements of lifted functions
// named with `replace` in lifter.manifest.
//
// The game moves things from timers (manager 0xABDC98; each timer: +0
// vtable, +4 interval in ms, +0x14 due time). Edge scrolling of the
// adventure map is t_adventure_frame's timer (vtable 0x979714, tick
// 0x4BE240): every 50 ms it moves the view by direction x speed (+0x220,
// x4 below speed setting 5), i.e. 20 updates a second. Here it ticks three
// times as often with a third of the step each time; the remainders carry
// over, so the view moves exactly as far per 50 ms as before.
//
// Armies walk on t_army_mover's timer (vtable 0x97B37C, tick 0x544C70 ->
// 0x545560, 66 ms): each tick adds the velocity (+0x40, +0x44) to the
// position (+0x6C, +0x70, in 1/(+0x50) pixels) and puts the army's sprite
// (timer +0x34, minus 0xC) there; at a new tile it rebases the position.
// The next tick always adds the current velocity first, so between ticks
// the sprite can be drawn that part of the step ahead: after every pass of
// the timer list it is moved there, and before the next pass (and before
// the timer is destroyed) back to where the game put it. Walking frames
// still change once a tick: there are no drawings in between.
// A tick that comes late (the game runs one timer per pass) does not stop
// the motion: drawing goes on into the following step, which the late tick
// then catches up with.
//
// Creatures walk in combat the same way, on t_play_combat_walk's timer
// (vtable 0x98E9BC, at +8 of the walk; tick 0x7DEFD0, one tick per
// animation frame, 74 ms or more): each tick adds (+0x78 += +0x5C) / +0x7C
// units along the creature's direction (+0x6C) to its position (+0xC, x y
// z), at most the +0x54 units left to the next waypoint. They are drawn
// ahead the same way, never past the waypoint. The battlefield paints from
// its own timers, so a creature stays ahead through the timer passes and
// goes back only for its walk's tick (and destructor).
//
// Missiles (t_move_missile, timer vtable 0x98D1FC at +8, tick 0x7A3B00,
// 33 ms): each tick moves the missile (+0x70) by its velocity (+0x50, x y z,
// in 1/(+0x5C) units) times min(speed +0x74, distance left +0x60), plus
// +0x84 - 2 in height on an arc (+0x68). Drawn ahead the same way.
//
// Flying creatures (t_play_combat_flight, timer vtable 0x98E98C at +8, tick
// 0x7DD8D0): each tick adds, per axis, sign (+0x5C..) x accumulated rate
// (+0x50.. into +0x90.., per +0x9C) towards the target (+0x68, x y z), and
// puts the shadow (+0x8C) under the creature. Drawn ahead the same way,
// never past the target, while cruising (not taking off or landing).

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "runtime/ctx.h"
#include "runtime/guest_alloc.h"
#include "runtime/hle/present.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::gen {

void f_004BE240_lifted(::recomp::Ctx& c);
void f_00748970_lifted(::recomp::Ctx& c);
void f_005438F0_lifted(::recomp::Ctx& c);
void f_007DEB60_lifted(::recomp::Ctx& c);
void f_007DEFD0_lifted(::recomp::Ctx& c);
void f_007A3B00_lifted(::recomp::Ctx& c);
void f_007A3A40_lifted(::recomp::Ctx& c);
void f_007DD8D0_lifted(::recomp::Ctx& c);
void f_007DD690_lifted(::recomp::Ctx& c);

namespace {
struct Split {
  int32_t remainder = 0;  // speed units not yet scrolled
  int phase = 0;          // which third of the original 50 ms
};
std::map<uint32_t, Split> g_scroll;  // by timer

constexpr uint32_t kTimers = 0xABDC98;  // manager: list +0xC of {next, prev, timer}
constexpr uint32_t kArmyMoverVtable = 0x97B37C;
constexpr uint32_t kTime = 0x71E180;      // the game's clock (ms)
constexpr uint32_t kSetOffset = 0x41B690;  // sprite::set_offset(POINT*)
constexpr int32_t kFrame = 16;            // ms
// Combat (walks, flights, missiles) drawn ahead too, but not on iOS: on an
// iPad the motion looked choppy with it, at any combat speed, and smooth
// enough as the game moves it. Armies on the adventure map still are.
#if defined(__APPLE__) && TARGET_OS_IOS
constexpr bool kDrawCombatAhead = false;
#else
constexpr bool kDrawCombatAhead = true;
#endif
constexpr uint32_t kCombatWalkVtable = 0x98E9BC;
constexpr uint32_t kMissileVtable = 0x98D1FC;
constexpr uint32_t kFlightVtable = 0x98E98C;
constexpr uint32_t kDirection = 0x705B60;  // (dir) -> {dx, dy}
constexpr uint32_t kGroundZ = 0x563470;    // battlefield::height(creature, POINT*)
constexpr uint32_t kPlace = 0x5640A0;      // battlefield::move(creature, POINT3*)
constexpr uint32_t kSetFrame = 0x564500;   // battlefield::set_frame(object, frame)
constexpr uint32_t kFrameCount = 0x5D5600; // object::frame_count()

// Sprites and creatures drawn ahead, by timer: where the game put them.
struct Ahead {
  uint32_t object;  // sprite, or creature in combat
  uint32_t field;   // 0, or the creature's battlefield
  int32_t x, y, z;
};
std::map<uint32_t, Ahead> g_ahead;

gaddr Scratch() {
  static gaddr pt = 0;
  if (!pt) {
    pt = g_proc->vm.Reserve(0, 0x1000);
    g_proc->vm.Commit(pt, 0x1000, Prot::RW);
  }
  return pt;
}

// A thiscall into the game, the caller's registers kept.
uint32_t Call(::recomp::Ctx& c, uint32_t self, uint32_t fn, std::initializer_list<uint64_t> args) {
  const uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx, esp = c.esp;
  c.ecx = self;
  const uint32_t r = uint32_t(CallGuest(c, fn, args));
  c.eax = eax;
  c.ecx = ecx;
  c.edx = edx;
  c.esp = esp;
  return r;
}

// Moves a combat object. The battlefield redraws an object where it is only
// when its animation frame changes, so the frame is set to another one and
// back (no paint in between).
void Place(::recomp::Ctx& c, uint32_t field, uint32_t creature, int32_t x, int32_t y, int32_t z) {
  const gaddr pt = Scratch();
  Wr32(pt, uint32_t(x));
  Wr32(pt + 4, uint32_t(y));
  Wr32(pt + 8, uint32_t(z));
  Call(c, field, kPlace, {creature, pt});
  const uint32_t frame = Rd32(creature + 0x70);
  const uint32_t frames = Call(c, creature, kFrameCount, {});
  if (frames < 2 || frame >= frames) return;
  Call(c, field, kSetFrame, {creature, (frame + 1) % frames});
  Call(c, field, kSetFrame, {creature, frame});
}

uint32_t Sprite(uint32_t mover) {
  const uint32_t p = Rd32(mover + 0x34);
  return p ? p - 0xC : 0;
}

void SetOffset(::recomp::Ctx& c, uint32_t sprite, int32_t x, int32_t y) {
  const gaddr pt = Scratch();
  Wr32(pt, uint32_t(x));
  Wr32(pt + 4, uint32_t(y));
  Call(c, sprite, kSetOffset, {pt});
}

void PutBack(::recomp::Ctx& c, uint32_t mover) {
  auto it = g_ahead.find(mover);
  if (it == g_ahead.end()) return;
  const Ahead& a = it->second;
  if (a.field)
    Place(c, a.field, a.object, a.x, a.y, a.z);
  else
    SetOffset(c, a.object, a.x, a.y);
  g_ahead.erase(it);
}

int32_t Now(::recomp::Ctx& c) { return int32_t(Call(c, c.ecx, kTime, {})); }

// A walking combat creature the elapsed part of its next step ahead.
void WalkAhead(::recomp::Ctx& c, uint32_t t, int64_t part, int32_t interval) {
  const uint32_t creature = Rd32(t + 0x48), field = Rd32(t + 0x24), path = Rd32(t + 0x64);
  const int32_t index = int32_t(Rd32(t + 0x74)), div = int32_t(Rd32(t + 0x7C));
  const int32_t left = int32_t(Rd32(t + 0x54));
  if (!creature || !field || !path || index < 0 || div <= 0 || left <= 0) return PutBack(c, t);
  // The next tick steps on only along this waypoint's direction (else it turns).
  const uint32_t flags = Rd32(path + uint32_t(index) * 36 + 8);
  const int32_t dir = int32_t(Rd32(creature + 0x6C));
  if ((int32_t(flags << 14) >> 27) != dir || (flags & 0x20)) return PutBack(c, t);
  const int64_t step = (int64_t(int32_t(Rd32(t + 0x78))) + int32_t(Rd32(t + 0x5C))) / div;
  // Past the waypoint only if the path goes straight on there (else the
  // tick snaps to it and turns).
  int32_t reach = left;
  if (index > 0) {
    const uint32_t next = Rd32(path + uint32_t(index - 1) * 36 + 8);
    if ((int32_t(next << 14) >> 27) == dir && !(next & 0x20)) reach += 16;
  }
  const int32_t ahead = int32_t(std::min<int64_t>(step * part / interval, reach));
  if (ahead <= 0) return PutBack(c, t);
  const uint32_t d = Call(c, c.ecx, kDirection, {uint32_t(dir)});
  // From where the game put it (it may already be drawn ahead).
  auto it = g_ahead.find(t);
  const bool known = it != g_ahead.end() && it->second.object == creature;
  const int32_t x = known ? it->second.x : int32_t(Rd32(creature + 0xC));
  const int32_t y = known ? it->second.y : int32_t(Rd32(creature + 0x10));
  const int32_t z = known ? it->second.z : int32_t(Rd32(creature + 0x14));
  const int32_t ax = x + int32_t(Rd32(d)) * ahead, ay = y + int32_t(Rd32(d + 4)) * ahead;
  const gaddr pt = Scratch();
  Wr32(pt, uint32_t(ax));
  Wr32(pt + 4, uint32_t(ay));
  const int32_t az = int32_t(Call(c, field, kGroundZ, {creature, pt}));
  g_ahead[t] = {creature, field, x, y, z};
  Place(c, field, creature, ax, ay, az);
}

// A flying missile the elapsed part of its next step ahead.
void MissileAhead(::recomp::Ctx& c, uint32_t t, int64_t part, int32_t interval) {
  const uint32_t missile = Rd32(t + 0x70), field = Rd32(t + 0x24);
  const int32_t div = int32_t(Rd32(t + 0x5C));
  const int64_t speed = int32_t(Rd32(t + 0x74)), left = int32_t(Rd32(t + 0x60));
  if (!missile || !field || div <= 0 || speed <= 0 || left <= 0) return PutBack(c, t);
  // Units flown by now, a late tick's next step included, never past the end.
  const int64_t units = std::min(speed * part / interval, left);
  auto it = g_ahead.find(t);
  const bool known = it != g_ahead.end() && it->second.object == missile;
  const int32_t x = known ? it->second.x : int32_t(Rd32(missile + 0xC));
  const int32_t y = known ? it->second.y : int32_t(Rd32(missile + 0x10));
  const int32_t z = known ? it->second.z : int32_t(Rd32(missile + 0x14));
  auto along = [&](uint32_t v) {
    return int32_t(int64_t(int32_t(Rd32(t + v))) * units / div);
  };
  int32_t az = z + along(0x58);
  if (GPtr(t + 0x68)[0]) az += int32_t((int64_t(int32_t(Rd32(t + 0x84))) - 2) * part / interval);
  g_ahead[t] = {missile, field, x, y, z};
  Place(c, field, missile, x + along(0x50), y + along(0x54), az);
}

// A flying creature (and its shadow) the elapsed part of its next step ahead.
void FlightAhead(::recomp::Ctx& c, uint32_t t, int64_t part, int32_t interval) {
  const uint32_t creature = Rd32(t + 0x48), field = Rd32(t + 0x24), shadow = Rd32(t + 0x8C);
  const int32_t den = int32_t(Rd32(t + 0x9C));
  const int32_t state = int32_t(Rd32(creature ? creature + 0x68 : t + 0x68));
  if (!creature || !field || den <= 0 || int32_t(Rd32(t + 0x74)) <= 0 || state == 0xD ||
      state == 0xE) {
    PutBack(c, t);
    return PutBack(c, t + 1);
  }
  auto it = g_ahead.find(t);
  const bool known = it != g_ahead.end() && it->second.object == creature;
  const int32_t p[3] = {known ? it->second.x : int32_t(Rd32(creature + 0xC)),
                        known ? it->second.y : int32_t(Rd32(creature + 0x10)),
                        known ? it->second.z : int32_t(Rd32(creature + 0x14))};
  int32_t a[3];
  for (int i = 0; i < 3; i++) {
    const int32_t target = int32_t(Rd32(t + 0x68 + 4 * i));
    const int64_t v = int64_t(int32_t(Rd32(t + 0x5C + 4 * i))) * int32_t(Rd32(t + 0x50 + 4 * i));
    a[i] = p[i] + int32_t(v * part / interval / den);
    if ((target - p[i]) * int64_t(target - a[i]) < 0) a[i] = target;  // not past it
  }
  g_ahead[t] = {creature, field, p[0], p[1], p[2]};
  Place(c, field, creature, a[0], a[1], a[2]);
  if (!shadow) return;
  // Where the tick puts it: under the creature, by its height (vcall +8).
  const uint32_t model = creature + 8;
  const int32_t k = int32_t(Call(c, model, Rd32(Rd32(model) + 8), {})) * 8;
  if (!g_ahead.count(t + 1))
    g_ahead[t + 1] = {shadow, field, int32_t(Rd32(shadow + 0xC)), int32_t(Rd32(shadow + 0x10)),
                      int32_t(Rd32(shadow + 0x14))};
  Place(c, field, shadow, a[0] + k + 2, a[1] + k + 2, a[2]);
}

// Every walking army's sprite (and combat creature) the elapsed part of its next step ahead.
void DrawAhead(::recomp::Ctx& c, int32_t now) {
  const uint32_t mgr = Rd32(kTimers), head = mgr ? Rd32(mgr + 0xC) : 0;
  if (!head) return;
  int i = 0;
  for (uint32_t n = Rd32(head); n && n != head && i < 256; n = Rd32(n), i++) {
    const uint32_t t = Rd32(n + 8);
    if (!kDrawCombatAhead && t &&
        (Rd32(t) == kCombatWalkVtable || Rd32(t) == kFlightVtable || Rd32(t) == kMissileVtable))
      continue;
    if (t && Rd32(t) == kCombatWalkVtable) {
      const int32_t interval = int32_t(Rd32(t + 4));
      const int32_t since = now - (int32_t(Rd32(t + 0x14)) - interval);
      if (interval > 0 && since > 0) WalkAhead(c, t, std::min(since, 2 * interval - 1), interval);
      continue;
    }
    if (t && Rd32(t) == kFlightVtable) {
      const int32_t interval = int32_t(Rd32(t + 4));
      const int32_t since = now - (int32_t(Rd32(t + 0x14)) - interval);
      if (interval > 0 && since > 0) FlightAhead(c, t, std::min(since, 2 * interval - 1), interval);
      continue;
    }
    if (t && Rd32(t) == kMissileVtable) {
      const int32_t interval = int32_t(Rd32(t + 4));
      const int32_t since = now - (int32_t(Rd32(t + 0x14)) - interval);
      if (interval > 0 && since > 0) MissileAhead(c, t, std::min(since, 2 * interval - 1), interval);
      continue;
    }
    if (!t || Rd32(t) != kArmyMoverVtable) continue;
    const uint32_t sprite = Sprite(t);
    const int32_t interval = int32_t(Rd32(t + 4)), div = int32_t(Rd32(t + 0x50));
    // Animated (+0x5F) with steps left (+0x58): the next tick adds a step.
    if (!sprite || !GPtr(t + 0x5F)[0] || int32_t(Rd32(t + 0x58)) <= 0 || interval <= 0 ||
        div <= 0)
      continue;
    const int32_t since = now - (int32_t(Rd32(t + 0x14)) - interval);
    if (since <= 0) continue;
    // A late tick: on into the next step (still one step left after it).
    const int64_t part = std::min(since, int32_t(Rd32(t + 0x58)) >= 2 ? 2 * interval - 1 : interval - 1);
    const int32_t x = int32_t(Rd32(sprite + 0x64)), y = int32_t(Rd32(sprite + 0x68));
    const int32_t ax = x + int32_t(int64_t(int32_t(Rd32(t + 0x40))) * part / interval / div);
    const int32_t ay = y + int32_t(int64_t(int32_t(Rd32(t + 0x44))) * part / interval / div);
    if (ax == x && ay == y) continue;
    g_ahead[t] = {sprite, 0, x, y, 0};
    SetOffset(c, sprite, ax, ay);
  }
}
}  // namespace

// The timer list's pass (from the app's idle): the sprites go back to the
// game's positions for the timers and ahead again after them: once per frame
// the display shows (so in step with it: updates on a clock of their own beat
// against the refresh, and several per refresh made battles crawl, each one
// repainting the field), every kFrame ms while nothing is shown (a hidden
// window), and always around an army mover's own tick.
// It returns how long the app may wait (ms); while sprites are ahead, short,
// to catch the next shown frame.
void f_00748970(::recomp::Ctx& c) {
  static int32_t last = 0, prev_pass = 0, last_hitch = 0;
  const int32_t now = Now(c);
  // Gaps between passes while something walks: logged (one line a second).
  if (!g_ahead.empty() && now - prev_pass > 40 && now - last_hitch > 1000) {
    WarnF("h4: %d ms without a timer pass while units move\n", now - prev_pass);
    last_hitch = now;
  }
  prev_pass = now;
  static uint64_t shown = 0;
  const uint64_t frames = hle::ShownFrames();
  bool due = frames != shown || now - last >= kFrame;
  for (auto& [t, a] : g_ahead) due = due || (!a.field && now - int32_t(Rd32(t + 0x14)) >= 0);
  if (due) {
    for (auto it = g_ahead.begin(); it != g_ahead.end();) {
      const uint32_t t = (it++)->first;
      if (!g_ahead[t].field) PutBack(c, t);
    }
    f_00748970_lifted(c);
    const uint32_t eax = c.eax;
    DrawAhead(c, now);
    c.eax = eax;
    last = now;
    shown = frames;
  } else {
    f_00748970_lifted(c);
  }
  if (!g_ahead.empty())
    c.eax = std::min(c.eax, 1u);
}

// t_army_mover's destructor: its sprite back first.
void f_005438F0(::recomp::Ctx& c) {
  PutBack(c, c.ecx);
  f_005438F0_lifted(c);
}

// t_play_combat_walk's destructor (ecx = the walk; its timer at +8).
void f_007DEB60(::recomp::Ctx& c) {
  PutBack(c, c.ecx + 8);
  f_007DEB60_lifted(c);
}

// t_play_combat_walk's tick (ecx = its timer): from where the game put the
// creature.
void f_007DEFD0(::recomp::Ctx& c) {
  PutBack(c, c.ecx);
  f_007DEFD0_lifted(c);
}

// t_play_combat_flight's tick and destructor (ecx = the timer; the flight
// at -8); the shadow is filed under the timer + 1.
void f_007DD8D0(::recomp::Ctx& c) {
  PutBack(c, c.ecx);
  PutBack(c, c.ecx + 1);
  f_007DD8D0_lifted(c);
}
void f_007DD690(::recomp::Ctx& c) {
  PutBack(c, c.ecx + 8);
  PutBack(c, c.ecx + 9);
  f_007DD690_lifted(c);
}

// t_move_missile's tick and destructor (ecx = the timer; the missile mover
// at -8).
void f_007A3B00(::recomp::Ctx& c) {
  PutBack(c, c.ecx);
  f_007A3B00_lifted(c);
}
void f_007A3A40(::recomp::Ctx& c) {
  PutBack(c, c.ecx + 8);
  f_007A3A40_lifted(c);
}

// t_adventure_frame scroll tick (fastcall: ecx = the timer).
void f_004BE240(::recomp::Ctx& c) {
  const uint32_t timer = c.ecx;
  const int32_t speed = int32_t(Rd32(timer + 0x220));
  Split& s = g_scroll[timer];
  const int32_t total = speed + s.remainder;
  const int32_t part = total / 3;
  s.remainder = total - part * 3;
  Wr32(timer + 0x220, uint32_t(part));
  f_004BE240_lifted(c);
  Wr32(timer + 0x220, uint32_t(speed));
  // 17 + 17 + 16 ms: three ticks per original 50 ms.
  Wr32(timer + 4, s.phase == 2 ? 16 : 17);
  s.phase = (s.phase + 1) % 3;
}

}  // namespace recomp::gen

