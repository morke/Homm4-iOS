cmake_minimum_required(VERSION 3.25)
project(h4_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# x86recomp's runtime and HLE modules are shared libraries: rebuilding one
# does not relink the game.
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)

set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/x86recomp"
    CACHE PATH "x86recomp source tree")

enable_testing()
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

# The game: runtime + HLE + lifted code (generated/, produced by recomp_lift).
set(H4_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    CACHE PATH "recomp_lift output for heroes4.exe")
recomp_add_lifted(h4_lifted "${H4_GENERATED}")
if(TARGET h4_lifted)
  option(H4_DEBUG_HOOKS "Dev instrumentation of lifted functions (needs the replace lines in lifter.manifest)" OFF)
  set(_h4_sources src/main.cpp src/hle_extra.cpp src/resolution.cpp src/smooth.cpp)
  if(H4_DEBUG_HOOKS)
    list(APPEND _h4_sources src/debug_hooks.cpp)
  endif()
  add_executable(h4 ${_h4_sources} $<TARGET_OBJECTS:h4_lifted>)
  set(_h4_hle recomp_runtime recomp_mss32 recomp_dsound recomp_bink recomp_d3d7)
  if(COMMAND recomp_link_hle)  # runtimes with static builds (iOS) link HLE modules whole
    recomp_link_hle(h4 ${_h4_hle})
  else()
    target_link_libraries(h4 PRIVATE ${_h4_hle})
  endif()
  set_target_properties(h4 PROPERTIES ENABLE_EXPORTS ON)  # symbol names in fault backtraces

  # iOS: an app bundle (tools/ios/build_app.sh configures this build and
  # puts the game, the wings and the icon into it).
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(H4_BUNDLE_ID "local.h4port.heroes4" CACHE STRING "iOS bundle identifier")
    set(H4_IOS_PLATFORM "iPhoneOS" CACHE STRING "CFBundleSupportedPlatforms: iPhoneOS or iPhoneSimulator")
    set_target_properties(h4 PROPERTIES
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
      MACOSX_BUNDLE_GUI_IDENTIFIER "${H4_BUNDLE_ID}"
      MACOSX_BUNDLE_BUNDLE_NAME "Heroes IV"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
      MACOSX_BUNDLE_BUNDLE_VERSION "1")
  endif()
endif()
