// h4: Heroes of Might and Magic IV, natively.
//
//   h4 [--game-dir DIR] [-- GAME ARGS...]
//
// DIR is the game install (the folder with heroes4.exe, Data/, maps/).
// Defaults to $H4_GAME_DIR. Set H4_TRACE=1 to log every HLE call.
//
// iOS: the game is in the app bundle (Game/) or in its Documents/Game, the
// wings in the bundle's wings/ (tools/ios/build_app.sh); what it writes
// (saves, settings) and the log (h4-log.txt) go to the app's Documents.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/log.h"
#include "runtime/runtime.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if TARGET_OS_IOS
// SDL runs UIApplicationMain and calls main (as SDL_main) on the main thread.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

namespace recomp::h4 {
void RegisterHleOverrides();
}

// The ANSI code page for Cyrillic text (Windows-1251), on runtimes that have code pages.
template <typename Config>
void SetCyrillicCodePage(Config& game) {
  if constexpr (requires { game.ansi_code_page; }) game.ansi_code_page = 1251;
}

int main(int argc, char** argv) {
  std::string game_dir, game_args;
  if (const char* e = std::getenv("H4_GAME_DIR")) game_dir = e;
#if TARGET_OS_IOS
  {
    const char* home = std::getenv("HOME");
    const std::string docs = std::string(home ? home : ".") + "/Documents";
    // The log, unbuffered so it survives a hang or a kill; H4_LOG_CONSOLE=1
    // keeps it on the console (simctl launch --console).
    const char* console = std::getenv("H4_LOG_CONSOLE");
    if (!console || *console != '1') std::freopen((docs + "/h4-log.txt").c_str(), "a", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    setenv("XDG_DATA_HOME", docs.c_str(), 1);  // DataDir(): saves, registry, crash reports
    const char* base = SDL_GetBasePath();     // the app bundle, ending in '/'
    const std::string bundle = base ? base : "./";
    // The game in the bundle (simulator builds), else copied into
    // Documents/Game once (tools/ios/install_device.py --game).
    struct stat bst;
    if (game_dir.empty())
      game_dir = stat((bundle + "Game/heroes4.exe").c_str(), &bst) == 0 ? bundle + "Game" : docs + "/Game";
    // The Mac app's defaults (tools/install_app.sh), unless set.
    setenv("H4_RESOLUTION", "wide", 0);
    setenv("H4_FILTER", "fsr", 0);
    setenv("H4_DEBAND", "1", 0);
    setenv("H4_SHARPNESS", "0.2", 0);
    setenv("H4_WINGS_DIR", (bundle + "wings").c_str(), 0);
    // Touches near the screen's edges go to the game at once; the home
    // gesture takes a second swipe.
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    std::fprintf(stderr, "h4: starting, pid %d, game dir %s\n", int(getpid()), game_dir.c_str());
  }
#endif
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--game-dir") && i + 1 < argc) {
      game_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--")) {
      for (i++; i < argc; i++) {
        if (!game_args.empty()) game_args += ' ';
        game_args += argv[i];
      }
    } else {
      std::fprintf(stderr, "usage: %s [--game-dir DIR] [-- GAME ARGS...]\n", argv[0]);
      return 2;
    }
  }
  if (game_dir.empty()) {
    std::fprintf(stderr, "h4: pass --game-dir or set H4_GAME_DIR\n");
    return 2;
  }
  while (game_dir.size() > 1 && game_dir.back() == '/') game_dir.pop_back();
  if (const char* t = std::getenv("H4_TRACE")) recomp::g_trace = *t && *t != '0';

  std::string exe = game_dir + "/heroes4.exe";
  struct stat st;
  if (stat(exe.c_str(), &st) != 0) {
    std::fprintf(stderr, "h4: %s not found\n", exe.c_str());
    return 2;
  }

  recomp::h4::RegisterHleOverrides();  // hle_extra.cpp: over the runtime's own handlers

  recomp::GameConfig game;
  game.id = "h4";
  game.guest_dir = "C:\\Heroes4";
  game.exe_name = "heroes4.exe";
  // What the retail installer writes (the GOG install writes nothing).
  const std::string key = "HKLM\\SOFTWARE\\New World Computing\\Heroes of Might and Magic IV\\1.0";
  // The game's own settings key: without a "resolution" (the screen width)
  // it takes the desktop's size, which its interface does not support past
  // 1280x1024. 1024 is its 1024x768 mode.
  const std::string settings = "HKCU\\Software\\New World Computing\\Heroes of Might and Magic IV";
  game.registry = {{key, "AppPath", game.guest_dir},
                   {settings, "resolution", "1024", 4}};
  // The game draws its own cursor where GetCursorPos says, in exclusive
  // mode too, and never re-centres it.
  game.own_cursor = true;
  // Cyrillic translations are Windows-1251 text, file names too (the
  // autosaves): the ANSI code page Windows uses for Cyrillic.
  SetCyrillicCodePage(game);
#if TARGET_OS_IOS
  // Touch: tap a left click, drag a left drag (armies between slots), a
  // finger held still a right click (the game shows its help on the
  // release, until the next click: the help comes up under the finger), a
  // two-finger tap a right click.
  game.touch = recomp::GameConfig::Touch::Pointer;
#endif
  return recomp::RunProcess(game, exe, game_dir, game_args);
}
