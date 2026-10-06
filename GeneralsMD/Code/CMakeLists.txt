cmake_minimum_required(VERSION 3.20)

# P2: the oldest macOS the app runs on.  It has to be set before project(), and every target and
# vendored library built here inherits it (SDL3, freetype, glslang and the rest through add_subdirectory;
# FFmpeg through FFMPEG_POSIX_CFLAGS below).  Unset, the tools stamp the build machine's own version into
# every binary and the app refuses every Mac not on the newest macOS.  13.0 (Ventura): every Apple
# silicon Mac and Intel Macs from 2017 on run it, and nothing in the tree needs a newer one.  An explicit -DCMAKE_OSX_DEPLOYMENT_TARGET wins;
# an empty one (as every earlier build cache holds) takes the default.
if(CMAKE_HOST_APPLE AND "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "The oldest macOS the build runs on (P2)" FORCE)
endif()

project(GeneralsMD CXX C)

# Which operating system this build is for.  MSVC used to answer that question too, because for
# twenty years there was only one answer, and this file used the two interchangeably from its first
# line to its last.  They are different questions now.  Everything that is really about the
# platform - the CRT, the Windows SDK import libraries, MIDL, DirectX, the window - asks
# ZH_PLATFORM_WINDOWS; only a flag written in one compiler's own syntax asks which compiler is
# reading it.  PORTING.md has the rest of the design.
# Windows against POSIX, not Windows against macOS.  The port targets macOS first but is meant to
# reach Linux and other POSIX systems, so "is this not Windows" is ZH_PLATFORM_POSIX, and the two
# refinements below are for the few things that genuinely differ between POSIX systems (a window,
# audio, CPU detection) - never for "is this not Windows".  An unported platform-specific file fails
# with an #error naming what it lacks (cpudetect.cpp does this), which is louder and more useful than
# refusing to configure.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(ZH_PLATFORM_WINDOWS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
  # Android uses the POSIX engine/device implementation, but has its own window/render/input/storage
  # constraints. Keep it distinct so Android never accidentally takes Linux desktop policy.
  set(ZH_PLATFORM_POSIX TRUE)
  set(ZH_PLATFORM_ANDROID TRUE)
  # Every static library that is folded into libmain.so must use PIC on Android.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
  set(ZH_PLATFORM_POSIX TRUE)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ZH_PLATFORM_MACOS TRUE)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ZH_PLATFORM_LINUX TRUE)
  endif()
endif()

# The other half of the split.  A target that only Windows can build is gated out below; a target
# whose sources do not compile under clang *yet* is a different thing, and this is it.  Those stay
# defined on macOS, so that tasks B1-B6 have something to drive with --target and see real errors
# from, but out of the default build, so that a Mac build failure means a regression in what was
# working rather than the next task's work showing through.  Take a name off the list as its task
# lands.  On Windows this expands to no argument at all and changes no target.
if(ZH_PLATFORM_POSIX)
  set(ZH_NOT_YET_PORTABLE EXCLUDE_FROM_ALL)
else()
  set(ZH_NOT_YET_PORTABLE "")
endif()

# A compiler may assume a member function's `this` is never null and delete the checks written on it.
# EA's code depends on three: MemoryPoolObject::deleteInstance() (GameMemory.h:762, `if (this)`, called
# on null pointers all over the engine), StateMachine::deleteInstance() (StateMachine.h:356) and
# WindowLayout::destroyWindows() (WindowLayout.cpp:176, `if (this == NULL)`).  GCC 6 and later delete
# them at -O2 and up - the native Linux build segfaulted in ScriptEngine::init (W1) - and GCC's release
# notes name this switch as the way to keep such code working.  Clang keeps them today (measured: the
# Release build's out-of-line deleteInstance and destroyWindows both begin with a null test, `cbz x0`)
# but warns that it may assume otherwise, so every POSIX compiler gets the switch.  This is what MSVC
# does already: it never made the assumption.
if(ZH_PLATFORM_POSIX AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fno-delete-null-pointer-checks)
endif()

# Linux links with LLD when it is there, else mold.  The engine's static libraries reference each other in
# cycles (gameengine, ww3d2, wwsaveload, w3ddevice ...), which GNU ld resolves only in link order, so
# generals failed to link with hundreds of undefined references; Apple's linker, LLD and mold resolve
# archives in any order.  mold is for the Steam Runtime's SDK (P3's portable build), which has it and no
# LLD.  With neither, GNU ld is left as it was and the cycles still need resolving (W1 records it).
if(ZH_PLATFORM_LINUX)
  find_program(ZH_LLD NAMES ld.lld)
  find_program(ZH_MOLD NAMES ld.mold mold)
  if(ZH_LLD)
    add_link_options(-fuse-ld=lld)
  elseif(ZH_MOLD)
    add_link_options(-fuse-ld=mold)
  else()
    message(WARNING "No ld.lld or mold: GNU ld cannot resolve the engine's circular static libraries in link order")
  endif()
endif()

# compile_commands.json: every source's exact compile command, which Tools/syntax_sweep.py and
# clangd read.  Ninja can produce it on demand; a Makefiles tree has nothing else that carries each
# target's own -D flags.  POSIX only because only the POSIX tooling reads it - include_case_check
# among them - and it changes no target and no command line.
if(ZH_PLATFORM_POSIX)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# The zip goes to machines that have never installed a Visual Studio redistributable, and
# generals.exe imported MSVCP140.dll and VCRUNTIME140.dll by name.  That fails in the Windows
# loader before a line of our code runs, so there is no DebugLogFile.txt and no crash file to
# send back - the same undiagnosable failure the retail Miles and Bink DLLs are packaged to
# avoid.  Link the CRT into the exe and into d3d8.dll instead of depending on a machine having
# it.  CMAKE_POLICY_DEFAULT is what reaches the vendored subdirectories: d3d8to9 declares
# cmake_minimum_required(3.12), which predates CMP0091, and would otherwise ignore the setting
# below and keep importing the DLL.
# Both of these are Windows concerns and both are read later than this - CMAKE_MSVC_RUNTIME_LIBRARY
# when a target is created, CMAKE_POLICY_DEFAULT_CMP0091 when a vendored subdirectory is added - so
# the guard costs the Windows build nothing.
if(ZH_PLATFORM_WINDOWS)
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# x64 only.  The 32-bit build was dropped once this one was proven against it: same simulation to
# the bit, same picture to within sub-pixel sampling, and sound and video of its own that do not
# depend on the two 32-bit DLLs the game shipped with.  Nothing here is written for x86 any more.
if(NOT ZH_PLATFORM_ANDROID AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Configure for x64: cmake -A x64")
endif()

# Android starts with one native ABI. The renderer/resource path is 64-bit clean and GLES is
# intentionally developed against arm64-v8a before adding a second ABI.
if(ZH_PLATFORM_ANDROID AND NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Android build must use arm64-v8a")
endif()

set(WW ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/WWVegas)
set(COMP ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/Compression)

# Libraries/Include holds the handful of headers that belong to no library and are wanted by all
# of them: Lib/BaseType.h, Lib/Trig.h and, since B2, Lib/Clock.h.  Thirteen targets read the clock
# - the five WWVegas libraries, the four dx11 ones and three of their tests - and naming each of
# them here is a list that goes stale the first time somebody adds a target and forgets.
#
# This is a directory-wide include path and nothing else.  The warning further down about not
# defining things globally is about compile *definitions*, where a mismatch between libraries is a
# link error or a differently-shaped struct; an extra /I carries no such risk, and the three
# headers behind it are included by name and by nobody who does not want them.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include)

# The third-party sources EA stripped out, which this repository does not carry either.  build.bat
# fetches them before it configures; say so here rather than let the generator report a missing
# source file per library.
set(VENDORED
    ${COMP}/ZLib/deflate.c
    ${COMP}/LZHCompress/CompLibSource/Lzhl.cpp
    # litehtml and nanosvg are common rather than Windows-only: gameengine links litehtml and
    # compiles nanosvg into HtmlOverlay.cpp, and gameengine is what the macOS build is for.
    ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/litehtml/CMakeLists.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/nanosvg/nanosvgrast.h
    # The GameSpy SDK is built everywhere since B6 (see "gamespy" below).
    ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/GameSpy/CMakeLists.txt)
# The DirectX 8 headers are only read by targets this file does not define on macOS, and A2 decides
# what a Mac fetch pulls down.  Demanding them here would stop a Mac configure over files nothing
# portable includes.
if(ZH_PLATFORM_WINDOWS)
  list(APPEND VENDORED
    ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/DirectX/Include/d3d8.h)
  set(VENDOR_HINT "Run Tools\\vendor.ps1 (build.bat does it for you).")
else()
  # SDL3 and miniaudio are the platform layer everywhere but Windows (decision 3).
  list(APPEND VENDORED
    ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/SDL3/CMakeLists.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/miniaudio/miniaudio.c)
  set(VENDOR_HINT "Run Tools/vendor.sh (build-macos.sh and build-linux.sh do it for you).")
endif()
foreach(vendored ${VENDORED})
  if(NOT EXISTS ${vendored})
    message(FATAL_ERROR "${vendored} is missing. ${VENDOR_HINT}")
  endif()
endforeach()
# SDL3's Metal patch (Libraries/Source/sdl3-metal-windowless.patch, decision 10).  A tree fetched before
# it and never given a vendor.sh run since still builds, and -offscreen then fails on a host with no
# window server with SDL's own "no supported SDL_GPU backend", which names nothing.  Said here instead.
# Apple only: the patch is to the Metal backend.
if(APPLE)
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/SDL3/src/gpu/metal/SDL_gpu_metal.m _zh_sdl3_patched
       REGEX "ZH_SDL_GPU_METAL_WINDOWLESS")
  if(NOT _zh_sdl3_patched)
    message(FATAL_ERROR "Libraries/Source/SDL3 lacks sdl3-metal-windowless.patch. ${VENDOR_HINT}")
  endif()
endif()

# LZH-Light's one patch (Libraries/Source/lzhl-clear-history.patch), on every platform: vendor.sh and
# vendor.ps1 both apply it.  A tree fetched before it builds without it and compresses from whatever the
# allocator left in the history buffer, so it is said here instead.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source/Compression/LZHCompress/CompLibHeader/_lz.h _zh_lzhl_patched
     REGEX "Zero Hour Reforged: altered")
if(NOT _zh_lzhl_patched)
  message(FATAL_ERROR "LZH-Light lacks lzhl-clear-history.patch. ${VENDOR_HINT}")
endif()

# The _CRT_ pair only tells MSVC's own headers to stop deprecating the C library this code is
# written against.  They mean nothing to any other compiler and cost nothing there, so they stay
# unconditional.  WIN32 and _WINDOWS are statements about the operating system, and most of the
# tree reads them as exactly that, so they move into the Windows branch: defining them on a Mac
# would tell several hundred #ifdefs the wrong thing.  A few places read WIN32 to mean "not
# 16-bit" instead; those are source fixes for tracks B and C, not a reason to define it here.
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
if(ZH_PLATFORM_WINDOWS)
  add_compile_definitions(WIN32 _WINDOWS)
endif()

# EA's own switch for exactly this situation ("Good for startup problems", Debug.h):
# it turns DEBUG_LOG back on in a Release build so the hundreds of existing log
# calls in GameEngine::init land in Run/generals.exe-<machine>.log.  Without it a
# quiet failure in init leaves nothing behind at all - GameEngine::init's
# catch (ErrorCode) swallows everything that is not ERROR_INVALID_D3D, and WinMain's
# outer catch (...) swallows the rest, so the exe just returns 0.  Applied per target
# at the bottom of this file, not globally: WWVegas' wwdebug.h includes GameEngine's
# Debug.h and routes WWDEBUG_SAY into DEBUG_LOG, so a global define makes wwmath and
# friends call DebugLog(), which only exists in gameengine - every leaf test exe then
# fails to link.  The WW libraries' own chatter is not what we are after anyway.
option(RELEASE_DEBUG_LOGGING "Write the startup debug log in Release builds" ON)
if(MSVC)
  # /permissive- is off on purpose: this is 2003-era code and templates here rely on
  # pre-two-phase-lookup name binding (simplevec.h et al).
  # /EHa, not /EHsc: WinMain installs _set_se_translator(DumpExceptionInfo) to turn an
  # access violation into a C++ exception and write ReleaseCrashInfo.txt on the way.  A
  # translator only runs in code compiled with asynchronous EH, so under /EHsc the hook
  # is dead (MSVC says so: "warning C4535: _set_se_translator() requires /EHa") and a
  # faulting frame just kills the process with 0xC0000005 and no log at all.
  add_compile_options(/W3 /permissive /EHa)
  # There is no debugger on the porting machine, so ReleaseCrashInfo.txt's stack
  # dump is the only stack we get.  StackDump.cpp walks the EBP chain by hand and
  # /O2 implies /Oy (no frame pointer), which leaves that dump empty; /Zi + /DEBUG
  # then give the frames names instead of bare addresses.  Costs a PDB, buys Phase 5.
  # /FS goes with /Zi: MSBuild compiles a project's sources in parallel and they all write
  # the one PDB.  Without it the writes go through mspdbsrv.exe, and if that dies or is
  # killed - which happens when a build is interrupted - every following compile fails with
  # C1041, or worse, only some of them do and the library is left half-built against an old
  # header.  /FS serializes the writes in the compilers themselves instead.
  add_compile_options(/Oy- /Zi /FS)
  # /MP: build a project's own sources in parallel too, not just one project at a
  # time.  MSBuild already runs projects concurrently, but gameengine alone is ~580
  # sources in a single project and was the long pole.  /FS above is what makes this
  # safe - the PDB writes are serialized in the compilers.
  add_compile_options(/MP)
  # There is no /arch:SSE2 here and none is needed: x64 has no x87 code generation at all, so
  # every float is computed at its declared width and every machine gets the same bits.  That is
  # what the 32-bit build had to ask for explicitly, and why it pinned the x87 precision control.
  # See dettrig.h and FPUControl.h.
  add_link_options(/DEBUG)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # Keyed on the COMPILER, not the OS.  These flags are what a GCC or Clang build needs to compute
  # the simulation the way MSVC does, wherever it runs.  It used to read elseif(ZH_PLATFORM_MACOS), so
  # a Linux build would have configured cleanly with none of them - and GCC's default is
  # -ffp-contract=fast, and plain char is UNSIGNED on aarch64 Linux.  Both silent desyncs.
  # The clang half of the same decisions.  None of the switches above transfers: /EHa is there for
  # _set_se_translator, which is a Windows structured-exception mechanism; /Oy- and /Zi are there
  # for StackDump.cpp's hand-written EBP walk, which no arm64 build has ever had; /FS and /MP are
  # about one MSBuild project writing one PDB.  Those are Windows reasons and they do not cross.
  # What is left is the short list of places where clang's defaults differ from cl.exe's in a way
  # this code notices.
  #
  # -fexceptions: the engine throws from the bottom and catches at the top - GameEngine::init's
  # catch (ErrorCode) and WinMain's outer catch (...) are structural, not decoration.  clang turns
  # them on for C++ by itself; saying so keeps a -fno-exceptions in some future preset from
  # deleting the error handling without a diagnostic.
  add_compile_options(-fexceptions)
  # -ffp-contract=off: the most important line in this file, and not optional.  clang on arm64
  # contracts a*b+c into one fused multiply-add, which rounds once where two operations round
  # twice.  MSVC on x64 never does it.  The simulation is full of multiply-adds, so a single
  # contracted instruction inside it makes this build compute different numbers from the Windows
  # build - and that surfaces as a replay that diverges and a network game that desyncs, not as a
  # compile error.  dettrig.h went to integer arithmetic over a committed table for exactly this
  # reason; this is the other half of the same promise, and E1 is the test that holds it.
  add_compile_options(-ffp-contract=off)
  # -fno-strict-aliasing: 2003 C++.  The .big and .w3d readers cast a byte buffer to a struct
  # pointer and read through it, which the standard does not allow and an optimiser is entitled to
  # act on.  MSVC does no type-based alias analysis, so this code has never been compiled by
  # anything that would.
  add_compile_options(-fno-strict-aliasing)
  # -fsigned-char: MSVC's plain char is signed and code here compares one against zero.  Darwin on
  # arm64 already signs it, unlike the ARM EABI, so this pins a default rather than changing one -
  # which is the point, because the default is the kind of thing that is cheap to state and
  # expensive to discover.
  add_compile_options(-fsigned-char)
  # -Wno-unknown-pragmas: the tree's #pragma warning lines (67, B4) are MSVC's diagnostics controls,
  # and clang has none of those warnings to control.  Off now so that a future -Wall is usable rather
  # than 19,000 lines of them.  B4 checked every pragma for meaning first: none that clang ignores
  # changes code.
  add_compile_options(-Wno-unknown-pragmas)
  # -g for the reason /Zi is there: there is no debugger on the porting machine, so a symbolised
  # stack is the only stack anyone gets (atos reads the frames C5's crash handler writes).  No
  # -fno-omit-frame-pointer: Darwin's ABI keeps frame pointers, which the crash handler walks from the
  # signal's context, and on Linux it takes backtrace(), which unwinds by the .eh_frame -fexceptions
  # keeps, not by frame pointers.
  add_compile_options(-g)
  # ZH_SANITIZE: a sanitizer build, for finding bugs, never for playing, timing or E1 evidence.  A list
  # for -fsanitize= (address, thread, undefined, or e.g. address,undefined).  Empty by default, and then
  # nothing here applies: a normal build's command lines are what they were.  Set, it adds the flags
  # everywhere and defines ZH_SANITIZER_BUILD, under which GameMemory's global new and delete take
  # zeroed blocks from calloc and give them to free, which the sanitizer intercepts, rather than from
  # its own allocator (a system framework, Apple's Metal driver, otherwise mixes the two; GameMemory.cpp
  # says why).  Not for MSVC.
  set(ZH_SANITIZE "" CACHE STRING "-fsanitize= list for a sanitizer build (empty: a normal build)")
  if(ZH_SANITIZE)
    add_compile_options(-fsanitize=${ZH_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${ZH_SANITIZE})
    add_compile_definitions(ZH_SANITIZER_BUILD)
    message(STATUS "ZH_SANITIZE=${ZH_SANITIZE}: a sanitizer build; GameMemory's global operators use calloc and free")
  endif()
else()
  message(FATAL_ERROR
    "No determinism flags are set for compiler '${CMAKE_CXX_COMPILER_ID}'.  The simulation must be "
    "compiled without floating-point contraction and with a signed char, or it desyncs; add this "
    "compiler's spelling of -ffp-contract=off and -fsigned-char before building with it.")
endif()

# ---- wwdebug --------------------------------------------------------------
# Libraries/Include is on the WWVegas targets from B3 onward: Platform/MSVCCompat.h lives
# there, always.h includes it, and every WWVegas library reaches always.h.  Nothing in that
# directory can shadow a WWVegas header - its only headers are under Lib/, rts/ and Platform/.
add_library(wwdebug STATIC
  ${WW}/WWDebug/wwdebug.cpp
  ${WW}/WWDebug/wwmemlog.cpp
  ${WW}/WWDebug/wwprofile.cpp)
# GameEngine/Include for Common/EarlyCommandLine.h alone (-headless, which no box may wait on), as debuglib has it.
target_include_directories(wwdebug PUBLIC ${WW}/WWDebug
  PRIVATE ${WW}/WWLib ${WW}/Wwutil ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include
          ${CMAKE_CURRENT_SOURCE_DIR}/GameEngine/Include)

# ---- wwutil ---------------------------------------------------------------
add_library(wwutil STATIC
  ${WW}/Wwutil/mathutil.cpp
  ${WW}/Wwutil/miscutil.cpp)
target_include_directories(wwutil PUBLIC ${WW}/Wwutil
  PRIVATE ${WW}/WWLib ${WW}/WWMath ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include)
target_link_libraries(wwutil PRIVATE wwdebug)

# ---- wwmath ---------------------------------------------------------------
add_library(wwmath STATIC
  ${WW}/WWMath/aabox.cpp          ${WW}/WWMath/aabtreecull.cpp
  ${WW}/WWMath/cardinalspline.cpp ${WW}/WWMath/catmullromspline.cpp
  ${WW}/WWMath/colmath.cpp        ${WW}/WWMath/colmathaabox.cpp
  ${WW}/WWMath/colmathaabtri.cpp  ${WW}/WWMath/colmathfrustum.cpp
  ${WW}/WWMath/colmathline.cpp    ${WW}/WWMath/colmathobbobb.cpp
  ${WW}/WWMath/colmathobbox.cpp   ${WW}/WWMath/colmathobbtri.cpp
  ${WW}/WWMath/colmathplane.cpp   ${WW}/WWMath/colmathsphere.cpp
  ${WW}/WWMath/cullsys.cpp        ${WW}/WWMath/curve.cpp
  ${WW}/WWMath/dettrig.cpp
  ${WW}/WWMath/euler.cpp          ${WW}/WWMath/frustum.cpp
  ${WW}/WWMath/gridcull.cpp       ${WW}/WWMath/hermitespline.cpp
  ${WW}/WWMath/lineseg.cpp        ${WW}/WWMath/lookuptable.cpp
  ${WW}/WWMath/matrix3.cpp        ${WW}/WWMath/matrix3d.cpp
  ${WW}/WWMath/matrix4.cpp        ${WW}/WWMath/obbox.cpp
  ${WW}/WWMath/ode.cpp            ${WW}/WWMath/pot.cpp
  ${WW}/WWMath/quat.cpp           ${WW}/WWMath/tcbspline.cpp
  ${WW}/WWMath/tri.cpp            ${WW}/WWMath/v3_rnd.cpp
  ${WW}/WWMath/vehiclecurve.cpp   ${WW}/WWMath/vp.cpp
  ${WW}/WWMath/wwmath.cpp)
target_include_directories(wwmath PUBLIC ${WW}/WWMath
  PRIVATE ${WW}/WWLib ${WW}/WWSaveLoad ${WW}/Wwutil ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include)
target_link_libraries(wwmath PRIVATE wwdebug)

# ---- compression ----------------------------------------------------------
# zlib 1.1.4 (vendored, per README) minus maketree.c - that one is a table
# generator with its own main().  Lzhl_tcp.cpp is also skipped (socket layer,
# nothing in the game calls it).
add_library(compression STATIC
  ${COMP}/CompressionManager.cpp
  ${COMP}/EAC/btreeabout.cpp ${COMP}/EAC/btreedecode.cpp ${COMP}/EAC/btreeencode.cpp
  ${COMP}/EAC/huffabout.cpp  ${COMP}/EAC/huffdecode.cpp  ${COMP}/EAC/huffencode.cpp
  ${COMP}/EAC/refabout.cpp   ${COMP}/EAC/refdecode.cpp   ${COMP}/EAC/refencode.cpp
  ${COMP}/LZHCompress/NoxCompress.cpp
  ${COMP}/LZHCompress/CompLibSource/Huff.cpp
  ${COMP}/LZHCompress/CompLibSource/Lz.cpp
  ${COMP}/LZHCompress/CompLibSource/Lzhl.cpp
  ${COMP}/ZLib/adler32.c  ${COMP}/ZLib/compress.c ${COMP}/ZLib/crc32.c
  ${COMP}/ZLib/deflate.c  ${COMP}/ZLib/gzio.c     ${COMP}/ZLib/infblock.c
  ${COMP}/ZLib/infcodes.c ${COMP}/ZLib/inffast.c  ${COMP}/ZLib/inflate.c
  ${COMP}/ZLib/inftrees.c ${COMP}/ZLib/infutil.c  ${COMP}/ZLib/trees.c
  ${COMP}/ZLib/uncompr.c  ${COMP}/ZLib/zutil.c)
target_include_directories(compression
  PUBLIC ${COMP} ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include
  PRIVATE ${COMP}/LZHCompress/CompLibHeader)
# CompressionManager calls z_compress2/z_uncompress and BaseType.h owns 'Byte',
# so zlib must be built with its Z_PREFIX renames on.
target_compile_definitions(compression PRIVATE Z_PREFIX)
# LZH-Light's _lzhl.h says `#define UINT32 unsigned long`, which is 32 bits under Windows' LLP64
# and 64 under Darwin's LP64.  LZHASH and LZPOS are both UINT32, so at 64 bits the hash index runs
# off the end of its table and LZHLCompressor::compress walks into unmapped memory - a SIGBUS on the
# first buffer it is handed.  The header asks first (#ifndef UINT32), so saying it here fixes the
# width without patching a vendored file that A2 re-fetches.  These widths reach the compressed
# bytes that save games and network packets are made of, so this is a parity fix, not a tidy-up.
#
# Not on Windows, and the reason is not caution.  UINT32 and UINT16 are Windows SDK *typedefs* -
# basetsd.h has `typedef unsigned int UINT32, *PUINT32;` - and _lzhl.h's guard tests for a *macro*,
# so on Windows the guard is true either way and the header already defines `unsigned long`, which
# is already 32 bits there.  The define would buy nothing and cost something: NoxCompress.cpp
# includes Lib/BaseType.h, which includes <windef.h>, so a command-line UINT32 macro is live when
# the SDK's typedef is preprocessed and turns it into `typedef unsigned int unsigned int, *PUINT32;`.
# clang only warns at that and limps on; MSVC is stricter about duplicate declaration specifiers and
# neither machine here can tell us how much stricter.  Found in review before it was merged.
if(ZH_PLATFORM_POSIX)
  target_compile_definitions(compression PRIVATE UINT32=unsigned\ int UINT16=unsigned\ short)
endif()

# Round-trip check over every codec CompressionManager exposes.
add_executable(compression_selfcheck ${COMP}/test_compression.cpp)
target_link_libraries(compression_selfcheck PRIVATE compression)

# DetRound::To_Long has to round half to even on every architecture, because its callers were written
# against an instruction that does.  Those callers are WW3D2's animation (hrawanim's floor idiom is
# arithmetic built on round-to-nearest) and WWMath - not GameLogic, which has none; an earlier
# version of this comment said "the simulation's callers" and was wrong.  Lib/DetRound.h is
# header-only and this links nothing, so it runs before any library compiles.  It checks DetRound
# against a hand-computed tie table on ONE architecture; it is not cross-architecture evidence.
# E3 (arch_differential) is.  See Tests/test_detround.cpp.
add_executable(detround_selfcheck ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_detround.cpp)
target_include_directories(detround_selfcheck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include)

# d3dxportable.h reproduces d3dx9_43.dll's D3DXVec4Transform bit for bit, because DumbProjectile-
# Behavior flies a shell along it and the flight is in the CRC.  Its expected values are outputs of
# Microsoft's own machine code (Tests/d3dx_golden.h), so this is not a self-consistency check.
# Header-only and links nothing, like detround_selfcheck.  See Tests/test_d3dxportable.cpp.
add_executable(d3dxportable_selfcheck ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_d3dxportable.cpp)
target_include_directories(d3dxportable_selfcheck PRIVATE ${WW}/WW3D2 ${CMAKE_CURRENT_SOURCE_DIR}/Tests)

# Guards the hand-rolled Matrix3D::Get_Inverse that replaced D3DXMatrixInverse.  It checks that the
# inverse is right, including one exact answer; it is NOT determinism evidence, and its header says
# why.  E3 is the cross-architecture evidence for DetTrig.
#
# wwlib and wwsaveload on every platform.  test_inverse.cpp needs only Matrix3D, but the link does
# not stop there: quat.cpp needs _FastAcosTable, which pulls in wwmath.cpp, and wwmath.cpp's
# WWMath::Init and Do_Force_Links reach LookupTableMgrClass (wwlib's file factory, strings, chunk
# I/O) and every spline's persist factory (wwsaveload).  ld -why_load shows the chain.  A macOS line
# with wwdebug alone was here once, on the belief that wwmath resolved against it; it had never been
# linked, and it does not.  So on macOS this waits for wwlib and wwsaveload to build.
add_executable(wwmath_selfcheck ${WW}/WWMath/test_inverse.cpp)
target_include_directories(wwmath_selfcheck PRIVATE ${WW}/WWLib ${WW}/WWSaveLoad ${WW}/Wwutil)
target_link_libraries(wwmath_selfcheck PRIVATE wwmath wwsaveload wwlib wwdebug)

# ---- wwlib ----------------------------------------------------------------
set(WWLIB_SOURCES
  ${WW}/WWLib/_convert.cpp   ${WW}/WWLib/_mono.cpp      ${WW}/WWLib/argv.cpp
  ${WW}/WWLib/b64pipe.cpp    ${WW}/WWLib/b64straw.cpp   ${WW}/WWLib/base64.cpp
  ${WW}/WWLib/blit.cpp       ${WW}/WWLib/blowfish.cpp   ${WW}/WWLib/buff.cpp
  ${WW}/WWLib/bufffile.cpp   ${WW}/WWLib/chunkio.cpp    ${WW}/WWLib/convert.cpp
  ${WW}/WWLib/cpudetect.cpp  ${WW}/WWLib/crc.cpp        ${WW}/WWLib/cstraw.cpp
  ${WW}/WWLib/data.cpp       ${WW}/WWLib/ddraw.cpp      ${WW}/WWLib/dsurface.cpp
  ${WW}/WWLib/Except.cpp     ${WW}/WWLib/FastAllocator.cpp
  ${WW}/WWLib/ffactory.cpp   ${WW}/WWLib/fixed.cpp      ${WW}/WWLib/md5.cpp
  ${WW}/WWLib/gcd_lcm.cpp    ${WW}/WWLib/hash.cpp
  ${WW}/WWLib/hsv.cpp        ${WW}/WWLib/ini.cpp        ${WW}/WWLib/int.cpp
  ${WW}/WWLib/jshell.cpp     ${WW}/WWLib/keyboard.cpp   ${WW}/WWLib/LaunchWeb.cpp
  ${WW}/WWLib/lcw.cpp        ${WW}/WWLib/load.cpp       ${WW}/WWLib/mixfile.cpp
  ${WW}/WWLib/mono.cpp       ${WW}/WWLib/mpmath.cpp     ${WW}/WWLib/mpu.cpp
  ${WW}/WWLib/msgloop.cpp    ${WW}/WWLib/multilist.cpp  ${WW}/WWLib/mutex.cpp
  ${WW}/WWLib/nstrdup.cpp    ${WW}/WWLib/palette.cpp    ${WW}/WWLib/pcx.cpp
  ${WW}/WWLib/pipe.cpp       ${WW}/WWLib/pk.cpp         ${WW}/WWLib/ramfile.cpp
  ${WW}/WWLib/random.cpp     ${WW}/WWLib/rawfile.cpp    ${WW}/WWLib/rc4.cpp
  ${WW}/WWLib/rcfile.cpp     ${WW}/WWLib/readline.cpp   ${WW}/WWLib/realcrc.cpp
  ${WW}/WWLib/refcount.cpp   ${WW}/WWLib/registry.cpp   ${WW}/WWLib/rgb.cpp
  ${WW}/WWLib/rle.cpp        ${WW}/WWLib/rndstrng.cpp   ${WW}/WWLib/sampler.cpp
  ${WW}/WWLib/sha.cpp        ${WW}/WWLib/slnode.cpp     ${WW}/WWLib/srandom.cpp
  ${WW}/WWLib/stimer.cpp     ${WW}/WWLib/straw.cpp      ${WW}/WWLib/strtok_r.cpp
  ${WW}/WWLib/systimer.cpp   ${WW}/WWLib/tagblock.cpp   ${WW}/WWLib/TARGA.CPP
  ${WW}/WWLib/textfile.cpp   ${WW}/WWLib/tgatodxt.cpp   ${WW}/WWLib/thread.cpp
  ${WW}/WWLib/trim.cpp       ${WW}/WWLib/vector.cpp     ${WW}/WWLib/verchk.cpp
  ${WW}/WWLib/widestring.cpp ${WW}/WWLib/win.cpp        ${WW}/WWLib/WWCOMUtil.cpp
  ${WW}/WWLib/wwfile.cpp     ${WW}/WWLib/wwfont.cpp     ${WW}/WWLib/wwstring.cpp
  ${WW}/WWLib/xpipe.cpp      ${WW}/WWLib/xstraw.cpp     ${WW}/WWLib/xsurface.cpp)
# Windows on Arm leaves mpu.cpp out, as the build off Windows does (the POSIX list below says why nothing
# needs it): its four functions read the x86 time-stamp counter, which ARM64 does not have.
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
  list(REMOVE_ITEM WWLIB_SOURCES ${WW}/WWLib/mpu.cpp)
endif()
# Win32 subsystem code, left out of wwlib everywhere but Windows.  Each of these is a whole file of
# one Windows API rather than a type leaking out of one, so there is nothing to port short of
# reimplementing the API.  On Windows this list is not read, and wwlib gets the same sources in the
# same order as before.  Put one back only with a caller that needs it.
#
# How "no caller" was established, because the first way was wrong: B5 searched for includers of
# each header, and that cannot see a function-local extern - verchk.cpp's, below.  Re-audited
# 2026-09-25 by symbol: every function, class and global each file defines, searched as a whole word
# through every tracked source outside Tools/, and each hit read.  Apart from RegistryClass (callers
# named below) and verchk (fixed), every hit was a comment, a string, a commented-out block, a type
# named only by pointer or reference, or code under a macro nothing defines (Except.cpp's
# DEMO_TIME_OUT).  What that cannot see: a name built by token pasting, and vendored code, which is
# not tracked and does not use WWLib.
if(ZH_PLATFORM_POSIX)
  list(REMOVE_ITEM WWLIB_SOURCES
    # DirectDraw, the pre-Direct3D 2D surface API.  The renderer never reaches it; DSurface is named
    # nowhere outside WWLib, and draw.cpp, the fourth file of the set, was already out.  ConvertClass
    # survives as a type: wwfont.cpp uses only its inline Convert_Pixel, WW3D2's txt*.cpp only
    # references to it.
    ${WW}/WWLib/convert.cpp    ${WW}/WWLib/ddraw.cpp      ${WW}/WWLib/dsurface.cpp
    # The \\.\MONO device driver for a second, monochrome debug monitor.  WW3D2's rendobj.cpp
    # includes _mono.h and names nothing from it.
    ${WW}/WWLib/_mono.cpp      ${WW}/WWLib/mono.cpp
    # Win32 resources, the message pump and its keyboard, ShellExecute, COM IDispatch and version
    # resources.  No callers at all.
    ${WW}/WWLib/data.cpp       ${WW}/WWLib/rcfile.cpp     ${WW}/WWLib/msgloop.cpp
    ${WW}/WWLib/keyboard.cpp   ${WW}/WWLib/LaunchWeb.cpp  ${WW}/WWLib/WWCOMUtil.cpp
    # verchk.cpp had one caller after all: Wwutil's Get_File_Id_String declares its
    # Get_Image_File_Header extern inside the function, so a search for verchk.h misses it.  That
    # call is Windows-only now; off Windows the timestamp is 0.
    ${WW}/WWLib/verchk.cpp
    # The HINSTANCE/HWND globals, which win.h declares only under _WINDOWS.
    ${WW}/WWLib/win.cpp
    # RegistryClass is HKEY throughout.  Its callers are dx8wrapper.cpp, W3DDisplay.cpp and
    # WWAudio.cpp, which persist device and audio settings; where those live on macOS is theirs
    # to decide, and registry.h says so to anyone who includes it.
    ${WW}/WWLib/registry.cpp
    # SecureRandomClass has no caller, and its only non-Windows seed is the _UNIX arm, which the
    # plan forbids switching on.
    ${WW}/WWLib/srandom.cpp
    # Get_CPU_Rate, Get_CPU_Clock, Get_RDTSC_CPU_Speed and RDTSC: an x86 time-stamp counter timed
    # against QueryPerformanceCounter.  Nothing calls any of the four - cpudetect.cpp and Except.cpp
    # include mpu.h and use nothing from it, and cpudetect reads __rdtsc itself.
    ${WW}/WWLib/mpu.cpp)
  # And one POSIX-only file: the engine's paths resolved against a POSIX file system, and the zh_*
  # forwarders built on it (C1, decisions D1 and D4).  WWLib because RawFileClass and GameEngine both
  # link it, and the resolver has to work before any engine subsystem exists.
  list(APPEND WWLIB_SOURCES ${WW}/WWLib/posixpath.cpp)
endif()
add_library(wwlib STATIC ${ZH_NOT_YET_PORTABLE} ${WWLIB_SOURCES})
# GameEngine/Include for Common/EarlyCommandLine.h alone, as wwdebug has it.
target_include_directories(wwlib PUBLIC ${WW}/WWLib
  PRIVATE ${WW}/WWMath ${WW}/Wwutil ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include
          ${CMAKE_CURRENT_SOURCE_DIR}/GameEngine/Include)
target_link_libraries(wwlib PRIVATE wwdebug)
if(ZH_PLATFORM_WINDOWS)
  # winmm: timeGetTime/timeBeginPeriod.  version: verchk.cpp's GetFileVersionInfo,
  # which Wwutil drags in for Get_File_Id_String.  Both are Windows import libraries; B2 replaces
  # the first with a platform clock and B3 the second.
  target_link_libraries(wwlib PUBLIC winmm version)
endif()

# ---- wwsaveload -----------------------------------------------------------
add_library(wwsaveload STATIC ${ZH_NOT_YET_PORTABLE}
  ${WW}/WWSaveLoad/definition.cpp          ${WW}/WWSaveLoad/definitionfactory.cpp
  ${WW}/WWSaveLoad/definitionfactorymgr.cpp ${WW}/WWSaveLoad/definitionmgr.cpp
  ${WW}/WWSaveLoad/parameter.cpp           ${WW}/WWSaveLoad/persistfactory.cpp
  ${WW}/WWSaveLoad/pointerremap.cpp        ${WW}/WWSaveLoad/saveload.cpp
  ${WW}/WWSaveLoad/saveloadstatus.cpp      ${WW}/WWSaveLoad/saveloadsubsystem.cpp
  ${WW}/WWSaveLoad/twiddler.cpp            ${WW}/WWSaveLoad/wwsaveload.cpp)
target_include_directories(wwsaveload PUBLIC ${WW}/WWSaveLoad
  PRIVATE ${WW}/WWLib ${WW}/WWMath ${WW}/Wwutil ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include)
target_link_libraries(wwsaveload PRIVATE wwdebug wwlib wwmath)

# ---- WW3D2's sources, on every platform ------------------------------------
# The renderer core is the same files on Windows and off it (decision 7: off Windows it speaks D3D9 to
# a device of our own).  Three are Windows's alone, and its block adds them back: FramGrab.cpp (AVI
# capture through vfw32), dx8webbrowser.cpp (the embedded browser, an MSVC #import) and dx11runtime.cpp
# (the Direct3D 11 backend's switchboard; dx11runtime_posix.cpp answers for it off Windows).
set(ZH_WW3D2_SOURCES
  ${WW}/WW3D2/aabtree.cpp ${WW}/WW3D2/aabtreebuilder.cpp ${WW}/WW3D2/agg_def.cpp
  ${WW}/WW3D2/animatedsoundmgr.cpp ${WW}/WW3D2/animobj.cpp ${WW}/WW3D2/assetmgr.cpp
  ${WW}/WW3D2/assetstatus.cpp ${WW}/WW3D2/bitmaphandler.cpp ${WW}/WW3D2/bmp2d.cpp
  ${WW}/WW3D2/boxrobj.cpp ${WW}/WW3D2/bwrender.cpp ${WW}/WW3D2/camera.cpp
  ${WW}/WW3D2/collect.cpp ${WW}/WW3D2/coltest.cpp ${WW}/WW3D2/composite.cpp
  ${WW}/WW3D2/dazzle.cpp ${WW}/WW3D2/ddsfile.cpp ${WW}/WW3D2/decalmsh.cpp
  ${WW}/WW3D2/decalsys.cpp ${WW}/WW3D2/distlod.cpp ${WW}/WW3D2/dx8caps.cpp
  ${WW}/WW3D2/dx8fvf.cpp ${WW}/WW3D2/dx8indexbuffer.cpp ${WW}/WW3D2/dx8polygonrenderer.cpp
  ${WW}/WW3D2/dx8renderer.cpp ${WW}/WW3D2/dx8rendererdebugger.cpp ${WW}/WW3D2/dx8texman.cpp
  ${WW}/WW3D2/dx8vertexbuffer.cpp ${WW}/WW3D2/dx8wrapper.cpp ${WW}/WW3D2/dynamesh.cpp
  ${WW}/WW3D2/ffprobe.cpp ${WW}/WW3D2/ffshadercache.cpp ${WW}/WW3D2/font3d.cpp
  ${WW}/WW3D2/formconv.cpp ${WW}/WW3D2/hanim.cpp ${WW}/WW3D2/hanimmgr.cpp
  ${WW}/WW3D2/hcanim.cpp ${WW}/WW3D2/hlod.cpp ${WW}/WW3D2/hmdldef.cpp
  ${WW}/WW3D2/hmorphanim.cpp ${WW}/WW3D2/hrawanim.cpp ${WW}/WW3D2/htree.cpp
  ${WW}/WW3D2/htreemgr.cpp ${WW}/WW3D2/intersec.cpp ${WW}/WW3D2/layer.cpp
  ${WW}/WW3D2/light.cpp ${WW}/WW3D2/lightenvironment.cpp ${WW}/WW3D2/line3d.cpp
  ${WW}/WW3D2/linegrp.cpp ${WW}/WW3D2/mapper.cpp ${WW}/WW3D2/matinfo.cpp
  ${WW}/WW3D2/matpass.cpp ${WW}/WW3D2/matrixmapper.cpp ${WW}/WW3D2/mesh.cpp
  ${WW}/WW3D2/meshbuild.cpp ${WW}/WW3D2/meshdam.cpp ${WW}/WW3D2/meshgeometry.cpp
  ${WW}/WW3D2/meshmatdesc.cpp ${WW}/WW3D2/meshmdl.cpp ${WW}/WW3D2/meshmdlio.cpp
  ${WW}/WW3D2/metalmap.cpp ${WW}/WW3D2/missingtexture.cpp ${WW}/WW3D2/motchan.cpp
  ${WW}/WW3D2/nullrobj.cpp ${WW}/WW3D2/part_buf.cpp ${WW}/WW3D2/part_emt.cpp
  ${WW}/WW3D2/part_ldr.cpp ${WW}/WW3D2/pivot.cpp ${WW}/WW3D2/pointgr.cpp
  ${WW}/WW3D2/polyinfo.cpp ${WW}/WW3D2/predlod.cpp ${WW}/WW3D2/prim_anim.cpp
  ${WW}/WW3D2/projector.cpp ${WW}/WW3D2/proto.cpp ${WW}/WW3D2/render2d.cpp
  ${WW}/WW3D2/render2dsentence.cpp ${WW}/WW3D2/renderobjectrecycler.cpp ${WW}/WW3D2/rendobj.cpp
  ${WW}/WW3D2/rinfo.cpp ${WW}/WW3D2/ringobj.cpp ${WW}/WW3D2/scene.cpp
  ${WW}/WW3D2/segline.cpp ${WW}/WW3D2/seglinerenderer.cpp ${WW}/WW3D2/shader.cpp
  ${WW}/WW3D2/shattersystem.cpp ${WW}/WW3D2/snappts.cpp ${WW}/WW3D2/sortingrenderer.cpp
  ${WW}/WW3D2/soundrobj.cpp ${WW}/WW3D2/sphereobj.cpp ${WW}/WW3D2/static_sort_list.cpp
  ${WW}/WW3D2/statistics.cpp ${WW}/WW3D2/streak.cpp ${WW}/WW3D2/streakRender.cpp
  ${WW}/WW3D2/stripoptimizer.cpp ${WW}/WW3D2/surfaceclass.cpp ${WW}/WW3D2/texfcach.cpp
  ${WW}/WW3D2/texproject.cpp ${WW}/WW3D2/texture.cpp ${WW}/WW3D2/texturefilter.cpp
  ${WW}/WW3D2/textureloader.cpp ${WW}/WW3D2/texturethumbnail.cpp ${WW}/WW3D2/txt.cpp
  ${WW}/WW3D2/vertmaterial.cpp ${WW}/WW3D2/visrasterizer.cpp ${WW}/WW3D2/w3d_dep.cpp
  ${WW}/WW3D2/w3d_util.cpp ${WW}/WW3D2/w3dexclusionlist.cpp ${WW}/WW3D2/ww3d.cpp
  ${WW}/WW3D2/ww3dformat.cpp
)

# ===========================================================================
# Windows only, from here down to dx9_smoke: every target in this block links d3d9, d3d11 or dxgi,
# or is built out of something that does.  Nothing is removed and nothing is stubbed - tracks D2
# through D5 abstract the backend interface and add a Metal one behind the same seam, and they
# bring these back one at a time.  Left unindented so the diff that added the guard is the guard.
# ===========================================================================
if(ZH_PLATFORM_WINDOWS)

# ---- d3dx9runtime ---------------------------------------------------------
# The D3DX9 entry points the renderer binds at run time, in a library of their own
# because GameEngine needs the math half without the renderer: BezierSegment carries
# D3DXMATRIX and DumbProjectileBehavior steers a shell with it.
# d3dx9portable.cpp is D3DX's arithmetic and d3dx9portable_texture.cpp its texture helpers, over the macOS and
# Linux surface code (PosixImage, PosixImageOps, PosixPixelCodec), which Windows on ARM64 binds in place of the
# DLL it does not have (and x64 with ZH_D3DX_PORTABLE=1); test_d3dx9portable_oracle holds both against the DLL.
set(ZH_POSIX_RENDER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/GameEngineDevice/Source/PosixDevice/Render)
add_library(d3dx9runtime STATIC
  ${WW}/WW3D2/d3dx9runtime.cpp ${WW}/WW3D2/d3d8shadertranslate.cpp ${WW}/WW3D2/d3dx9portable.cpp
  ${WW}/WW3D2/d3dx9portable_texture.cpp ${ZH_POSIX_RENDER_DIR}/PosixImage.cpp
  ${ZH_POSIX_RENDER_DIR}/PosixImageOps.cpp ${ZH_POSIX_RENDER_DIR}/PosixPixelCodec.cpp)
set_target_properties(d3dx9runtime PROPERTIES CXX_STANDARD 17)
target_include_directories(d3dx9runtime PUBLIC ${WW}/WW3D2 PRIVATE ${ZH_POSIX_RENDER_DIR})
target_link_libraries(d3dx9runtime PUBLIC d3d9)

# ---- ffshader -------------------------------------------------------------
# The fixed-function texture stages written out as HLSL, phase 2's replacement for
# combiners D3D11 does not have.  Its own library because it is string building over
# a description and nothing else: the test links this alone rather than all of WW3D2.
add_library(ffshader STATIC ${WW}/WW3D2/ffshader.cpp)
set_target_properties(ffshader PROPERTIES CXX_STANDARD 17)
target_include_directories(ffshader PUBLIC ${WW}/WW3D2)
target_link_libraries(ffshader PUBLIC d3d9)

# ---- ffvertex -------------------------------------------------------------
# The other half of the same job: the fixed-function vertex pipeline as HLSL.  It
# generates for both profiles so the D3D9 form can be compared against the pipeline
# it replaces before the D3D11 one is trusted.
add_library(ffvertex STATIC ${WW}/WW3D2/ffvertex.cpp)
set_target_properties(ffvertex PROPERTIES CXX_STANDARD 17)
target_include_directories(ffvertex PUBLIC ${WW}/WW3D2)
target_link_libraries(ffvertex PUBLIC d3d9)

# ---- engineshader ---------------------------------------------------------
# The shipped .vso and .pso files transcribed as HLSL, for the draws that bind one
# of the engine's own shaders and set no fixed-function state a generator could read.
# Its own library for the same reason ffvertex is: string building over an enum, and
# the test compiles what it writes without a device anywhere near it.
add_library(engineshader STATIC ${WW}/WW3D2/engineshader.cpp)
set_target_properties(engineshader PROPERTIES CXX_STANDARD 17)
target_include_directories(engineshader PUBLIC ${WW}/WW3D2)
target_link_libraries(engineshader PUBLIC ffshader)

# ---- dx11device -----------------------------------------------------------
# The Direct3D 11 device, its swap chain and the views on it: the bottom of phase 2.
# Its own library for the same reason ffshader is, and for one more - it links d3d11
# and dxgi, and nothing else in the tree does yet.  The test creates a real device.
add_library(dx11device STATIC ${WW}/WW3D2/dx11device.cpp)
set_target_properties(dx11device PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11device PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11device PUBLIC d3d11 dxgi)

# ---- dx11state ------------------------------------------------------------
# D3D9's render states turned into D3D11's three state objects.  Pure translation,
# no device, so the whole table is checked by a test rather than looked at on screen.
add_library(dx11state STATIC ${WW}/WW3D2/dx11state.cpp)
set_target_properties(dx11state PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11state PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11state PUBLIC d3d9 d3d11)

# ---- dx11layout -----------------------------------------------------------
# A D3D9 flexible vertex format read out as a D3D11 input layout.  The vertex buffers
# keep the bytes they have; this is what tells D3D11 where the fields are.
add_library(dx11layout STATIC ${WW}/WW3D2/dx11layout.cpp)
set_target_properties(dx11layout PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11layout PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11layout PUBLIC d3d9 d3d11)

# ---- dx11resource ---------------------------------------------------------
# The buffers and textures: D3D9's formats, pools and lock flags read as D3D11's
# formats, usages and map types.  Half a table and half what the device will accept,
# so the test checks the table and then hands the results to a real device.
add_library(dx11resource STATIC ${WW}/WW3D2/dx11resource.cpp)
set_target_properties(dx11resource PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11resource PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11resource PUBLIC d3d9 d3d11)

# ---- dx11twin -------------------------------------------------------------
# The D3D11 copy of a vertex or index buffer and the heap block both copies are
# written through.  A D3D9 buffer is write-only, so the copy cannot be built after
# the fact and the lock has to be moved instead; the test proves the same bytes end
# up on both sides.
add_library(dx11twin STATIC ${WW}/WW3D2/dx11twin.cpp)
set_target_properties(dx11twin PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11twin PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11twin PUBLIC dx11resource d3d11)

# ---- dx11texture ----------------------------------------------------------
# The D3D11 copy of a texture the engine already built in D3D9, read back off the
# managed original at the first bind and kept on it as private data.
add_library(dx11texture STATIC ${WW}/WW3D2/dx11texture.cpp)
set_target_properties(dx11texture PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11texture PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11texture PUBLIC dx11resource d3d9 d3d11 dxguid)

# ---- dx11post -------------------------------------------------------------
# The offscreen render target between the scene and the swap chain, and the full
# screen passes over it.  Depends on the device and on nothing else: it reads a
# texture and writes a render target, and knows nothing about the state
# translation the rest of the backend is.
add_library(dx11post STATIC ${WW}/WW3D2/dx11post.cpp)
set_target_properties(dx11post PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11post PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11post PUBLIC dx11device d3d11)

# ---- dx11backend ----------------------------------------------------------
# The renderer itself: the engine's D3D9-shaped calls taken as they are and resolved
# into a D3D11 pipeline at the moment of the draw.  Ties the five pieces above
# together, which is why it is the last of them and the one with a drawing test.
add_library(dx11backend STATIC ${WW}/WW3D2/dx11backend.cpp)
set_target_properties(dx11backend PROPERTIES CXX_STANDARD 17)
target_include_directories(dx11backend PUBLIC ${WW}/WW3D2)
target_link_libraries(dx11backend PUBLIC dx11device dx11state dx11layout dx11resource
  engineshader ffshader ffvertex)

# ---- ww3d2 (renderer core) ------------------------------------------------
# wwshade is header-only here on purpose: no .dsp links wwshade.lib and its
# shd*bump* sources need shdpp/NVASM-generated headers.  Revisit at exe link.
# dx8webbrowser.cpp is the embedded browser: its header sets ENABLE_EMBEDDED_BROWSER to 1, so it is
# live on Windows (the POSIX build leaves it out).
add_library(ww3d2 STATIC
  ${WW}/WW3D2/FramGrab.cpp ${WW}/WW3D2/dx8webbrowser.cpp ${WW}/WW3D2/dx11runtime.cpp
  ${ZH_WW3D2_SOURCES}
)
target_include_directories(ww3d2
  PUBLIC ${WW}/WW3D2
  PRIVATE ${WW}/WWLib ${WW}/WWMath ${WW}/WWDebug ${WW}/WWSaveLoad ${WW}/Wwutil
          ${WW}/wwshade ${WW}/WWAudio ${WW}/Miles6/include)
# RENDERER-ROADMAP.md phase 1: d3d9.h and d3d9.lib come out of the Windows SDK, so
# the vendored DirectX 8 headers are off the include path entirely - a d3d8.h left
# reachable is a file that compiles against the wrong runtime without saying so.
# D3DX9 has no import library anywhere and is bound at runtime instead: see
# WW3D2/d3dx9runtime.h.  vfw32 is FramGrab.cpp writing AVIs.
target_link_libraries(ww3d2
  PUBLIC d3d9 dxguid vfw32 d3dx9runtime ffshader dx11backend dx11post dx11twin dx11texture
  PRIVATE wwlib wwmath wwdebug wwsaveload)
# dx8webbrowser.cpp is the one file /MP cannot have: MSVC rejects #import under
# multi-process compilation (C2813) and finds the directive by a lexical scan, so
# even though ENABLE_EMBEDDED_BROWSER's block is what holds it, disabling the block
# would not help.  /MP1 puts just this file back on a single process.
set_source_files_properties(${WW}/WW3D2/dx8webbrowser.cpp PROPERTIES COMPILE_OPTIONS /MP1)

# Phase 8: 1 kHz EIP sampler that symbolizes against the linker map - the
# machine has no debugger, so this is how hotspots get names.
add_executable(eip_sampler ${CMAKE_CURRENT_SOURCE_DIR}/Tools/ProfileSampler/eip_sampler.cpp)
set_target_properties(eip_sampler PROPERTIES CXX_STANDARD 17)

# RENDERER-ROADMAP.md phase 1 checkpoint: a real device, with no translating dll
# anywhere.  It replaces dx8_smoke, which proved the same thing about the vendored
# DirectX 8 SDK and the d3d8.dll the renderer used to load; that path is gone.
# d3d9.h and d3d9.lib come from the Windows SDK, so this target never sees
# Libraries/DirectX/Include.
add_executable(dx9_smoke ${WW}/WW3D2/test_dx9_smoke.cpp)
target_link_libraries(dx9_smoke PRIVATE d3dx9runtime user32)
# Tests/ for d3dx_golden.h: dx9_smoke prints what this machine's d3dx9_43.dll computes for
# B17's golden rows, and which of its CPU-dispatched bodies ran.
target_include_directories(dx9_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Tests)

# d3dx9portable.cpp against d3dx9_43.dll itself (W-ARM64): what ARM64, macOS and Linux use in the DLL's place,
# compared with the DLL where it binds (x64): the arithmetic, and on a real Direct3D 9 device the texture helpers
# and the shader tools ARM64 binds.  Where the DLL does not bind (ARM64, or no DirectX redistributable), there
# is nothing to compare, and the test exits 77, which ctest reports as skipped rather than passed.
add_executable(test_d3dx9portable_oracle ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_d3dx9portable_oracle.cpp)
target_include_directories(test_d3dx9portable_oracle PRIVATE ${ZH_POSIX_RENDER_DIR})
target_link_libraries(test_d3dx9portable_oracle PRIVATE d3dx9runtime user32)
add_test(NAME test_d3dx9portable_oracle COMMAND test_d3dx9portable_oracle)
set_tests_properties(test_d3dx9portable_oracle PROPERTIES SKIP_RETURN_CODE 77)
endif()  # ZH_PLATFORM_WINDOWS - renderer, Direct3D 9/11 backends, dx9_smoke

# ===========================================================================
# Phase 2 - remaining support libs.  None of these touch DX8.
# ===========================================================================
set(LIBSRC ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Source)

# The next three link Windows import libraries and nothing else stands in for them yet: debug wants
# dbghelp and comctl32, profile is built on debug, and wwdownload is FTP over winsock.  C5 replaces
# the crash reporting and B6 decides what wwdownload becomes on macOS.
if(ZH_PLATFORM_WINDOWS)
# ---- debug ----------------------------------------------------------------
# EA's in-repo debug/logging lib (RTS.dsp links it as debug.lib).  Self-contained
# apart from the Win32 SDK.  The test1-6/, netserv/ and debug_dlg/ subdirs are
# separate .dsp projects, not part of the lib.
add_library(debug STATIC
  ${LIBSRC}/debug/debug.cpp        ${LIBSRC}/debug/debug_cmd.cpp
  ${LIBSRC}/debug/debug_debug.cpp  ${LIBSRC}/debug/debug_except.cpp
  ${LIBSRC}/debug/debug_getdefaultcommands.cpp
  ${LIBSRC}/debug/debug_internal.cpp
  ${LIBSRC}/debug/debug_io_con.cpp ${LIBSRC}/debug/debug_io_flat.cpp
  ${LIBSRC}/debug/debug_io_net.cpp ${LIBSRC}/debug/debug_io_ods.cpp
  ${LIBSRC}/debug/debug_purecall.cpp
  ${LIBSRC}/debug/debug_stack.cpp)
target_include_directories(debug PUBLIC ${LIBSRC}/debug)
# Common/EarlyCommandLine.h, header-only: the crash box stands aside under -headless (W2)
target_include_directories(debug PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/GameEngine/Include)
target_link_libraries(debug PUBLIC dbghelp comctl32 ws2_32)
# "debug" is also a target_link_libraries config keyword, so it can never be
# passed as a plain library name.  Link this alias instead.
add_library(debuglib ALIAS debug)
# debug.h asks the linker for this library by the build's name (#pragma comment(lib, "debugdebug.lib") under
# _DEBUG), so a Debug build names it that way.  Release keeps debug.lib, the name it asks for there.
set_target_properties(debug PROPERTIES OUTPUT_NAME_DEBUG debugdebug)

# ---- profile --------------------------------------------------------------
# EA's in-repo sampling/function profiler.  GameEngine includes profile.h, whose
# #pragma comment(lib, "profile.lib") makes this a hard link dependency even
# though almost nothing calls into it.
add_library(profile STATIC
  ${LIBSRC}/profile/profile.cpp           ${LIBSRC}/profile/profile_cmd.cpp
  ${LIBSRC}/profile/profile_funclevel.cpp ${LIBSRC}/profile/profile_highlevel.cpp
  ${LIBSRC}/profile/profile_result.cpp)
target_include_directories(profile PUBLIC ${LIBSRC}/profile)
target_link_libraries(profile PUBLIC debuglib)
# profile.h asks for it by the build's name too (profiledebug.lib under _DEBUG), as debug.h does.
set_target_properties(profile PROPERTIES OUTPUT_NAME_DEBUG profiledebug)

# ---- wwdownload -----------------------------------------------------------
# FTP patch downloader.  Its wlib/resource.h includes are all commented out in
# the shipped source, so it needs nothing but winsock.
add_library(wwdownload STATIC
  ${WW}/WWDownload/Download.cpp ${WW}/WWDownload/FTP.CPP
  ${WW}/WWDownload/registry.cpp ${WW}/WWDownload/urlBuilder.cpp)
target_include_directories(wwdownload PUBLIC ${WW}/WWDownload ${WW})
target_link_libraries(wwdownload PUBLIC wsock32 winmm)
endif()  # ZH_PLATFORM_WINDOWS - debug, profile, wwdownload

# ---- benchmark ------------------------------------------------------------
# BYTEmark sources are missing; benchmark_stub.c stands in for them.  See the
# file header for why a constant is enough and when to revisit.
add_library(benchmark STATIC ${LIBSRC}/Benchmark/benchmark_stub.c)
target_include_directories(benchmark PUBLIC ${LIBSRC}/Benchmark)

# ---- eabrowserdispatch ----------------------------------------------------
# MIDL is a Windows SDK tool and this whole library is its output, so there is nothing to generate
# on a Mac.  B6 puts a null WebBrowser in Stubs/ for the macOS side.
if(ZH_PLATFORM_WINDOWS)
# Only BrowserDispatch.idl shipped; the .h/_i.c that GameEngine's WebBrowser.h
# expects are MIDL output.  Generate them at build time (MSBuild puts cl.exe,
# MIDL's preprocessor, on PATH) into a dir laid out so the shipped
# #include "EABrowserDispatch/BrowserDispatch.h" resolves unchanged.
set(EABD_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated/EABrowserDispatch)
file(MAKE_DIRECTORY ${EABD_GEN})
# midl.exe ships with the Windows SDK, not on PATH at configure time.
file(GLOB MIDL_HINTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x86")
find_program(MIDL_EXECUTABLE midl HINTS ${MIDL_HINTS} REQUIRED)
add_custom_command(
  OUTPUT ${EABD_GEN}/BrowserDispatch.h ${EABD_GEN}/BrowserDispatch_i.c
  COMMAND ${MIDL_EXECUTABLE} /nologo /env win32 /win32
          /h ${EABD_GEN}/BrowserDispatch.h
          /iid ${EABD_GEN}/BrowserDispatch_i.c
          /proxy ${EABD_GEN}/BrowserDispatch_p.c
          /dlldata ${EABD_GEN}/dlldata.c
          /tlb ${EABD_GEN}/BrowserDispatch.tlb
          ${LIBSRC}/EABrowserDispatch/BrowserDispatch.idl
  DEPENDS ${LIBSRC}/EABrowserDispatch/BrowserDispatch.idl
  COMMENT "MIDL BrowserDispatch.idl")
add_library(eabrowserdispatch STATIC
  ${EABD_GEN}/BrowserDispatch_i.c ${EABD_GEN}/BrowserDispatch.h)
target_include_directories(eabrowserdispatch
  PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
endif()  # ZH_PLATFORM_WINDOWS - MIDL-generated browser dispatch

# ---- gamespy --------------------------------------------------------------
# Every platform (B6): the SDK is CMake-based and portable, and gameengine's online code - PeerThread,
# BuddyThread, PersistentStorageThread, MainMenuUtils - calls 89 of its entry points.  Built for real
# rather than stubbed, with Tools/vendor.sh's two patches applied (gamespy-strlwr-c-only.patch, and
# gamespy-gsi-unix.patch, which renames its _UNIX to GSI_UNIX so no engine file sees _UNIX).  Nothing
# in it runs until a player opens the online menus, and the services it would reach are gone.
# PLAN.md called for hand-written stubs behind the five GameSpy* libs, but the
# real SDK is open source (TheSuperHackers/GamespySDK, itself CMake-based), so
# vendor and build it instead: correct signatures, structs and enum values
# rather than 73 guessed prototypes.  Multiplayer stays out of scope - this only
# has to compile and link; nothing needs to reach a live GameSpy server.
# OpenSSL/HTTPS off so the SDK does not FetchContent OpenSSL for services that
# have been dead since 2014; voice and tests off for the same reason.
set(GS_OPENSSL          OFF CACHE BOOL "" FORCE)
set(GS_SECURESECONNECT  OFF CACHE BOOL "" FORCE)
set(GS_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(GS_INCLUDE_VOICE    OFF CACHE BOOL "" FORCE)
add_subdirectory(${LIBSRC}/GameSpy ${CMAKE_CURRENT_BINARY_DIR}/gamespy)

# ---- litehtml -------------------------------------------------------------
# The HTML and CSS layout engine behind the pages drawn over the battlefield (GameClient/
# HtmlOverlay.cpp).  It lays a page out and calls back to draw each box and each run of text, so
# the drawing stays the game's own.  Its own CMakeLists sets /permissive- and C++17 for itself
# alone, and brings the gumbo HTML parser with it.
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${LIBSRC}/litehtml ${CMAKE_CURRENT_BINARY_DIR}/litehtml)

# ---- SDL3 and miniaudio: the platform layer off Windows (decision 3) -------------------------------
# Both are fetched by Tools/vendor.sh and neither is fetched on Windows, which keeps Win32Device and
# Miles.  Nothing links either yet: C2/C3 take SDL3's window and events, D4 its GPU API, C4 miniaudio.
# They are built by default all the same, so that a break shows up in the build that caused it
# rather than in the first task to link them.
if(ZH_PLATFORM_POSIX)
  # SDL3 static, and only the subsystems decision 3 uses.  On: video (window, events, message boxes),
  # GPU (the renderer - Metal on macOS, Vulkan on Linux) and dialog (the first launch's folder chooser,
  # PosixMain.cpp: NSOpenPanel on macOS, the portal or zenity on Linux; with it off, SDL answers every
  # dialog at once with "not built with dialog support", and the chooser quit before showing anything).
  # Off: audio (miniaudio has that job), render (SDL's 2D renderer; D4 uses the GPU API directly),
  # camera, joystick, haptic, hidapi, power, sensor and tray, and the OpenGL/GLES video drivers, which
  # the GPU API does not use.  Tests, examples and install are off.
  foreach(_sdl_off SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_UNINSTALL
                   SDL_AUDIO SDL_RENDER SDL_CAMERA SDL_JOYSTICK SDL_HAPTIC SDL_HIDAPI SDL_POWER
                   SDL_SENSOR SDL_TRAY SDL_OPENGL SDL_OPENGLES)
    set(${_sdl_off} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  if(ZH_PLATFORM_ANDROID)
    # SDLActivity must load the same SDL instance that the engine uses. Android therefore uses
    # SDL3-shared so Java JNI and native engine state live in one library, while desktop keeps static SDL.
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  endif()
  if(ZH_PLATFORM_ANDROID)
    # The renderer uses SDL's EGL-backed OpenGL ES context directly, not SDL_GPU.
    set(SDL_OPENGL ON CACHE BOOL "" FORCE)
    set(SDL_OPENGLES ON CACHE BOOL "" FORCE)
  endif()
  foreach(_sdl_on SDL_VIDEO SDL_GPU SDL_DIALOG)
    set(${_sdl_on} ON CACHE BOOL "" FORCE)
  endforeach()
  if(NOT ZH_PLATFORM_ANDROID)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
  endif()
  if(ZH_PLATFORM_ANDROID)
    set(ZH_SDL3_TARGET SDL3::SDL3-shared)
  else()
    set(ZH_SDL3_TARGET SDL3::SDL3-static)
  endif()
  add_subdirectory(${LIBSRC}/SDL3 ${CMAKE_CURRENT_BINARY_DIR}/SDL3)

  # miniaudio: one C file.  Its own CMakeLists builds extras and examples this does not want.
  # It opens the platform's audio libraries at run time by default, so the static library needs
  # nothing at link beyond threads, libm and libdl, which are its consumers' to carry.
  find_package(Threads REQUIRED)
  add_library(miniaudio STATIC ${LIBSRC}/miniaudio/miniaudio.c)
  target_include_directories(miniaudio PUBLIC ${LIBSRC}/miniaudio)
  # libm on every POSIX system: separate from libc on Linux and the BSDs, part of libSystem on macOS,
  # where -lm is accepted and does nothing.  Not keyed on Linux, so the next platform needs no edit.
  target_link_libraries(miniaudio INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)

  # ---- The shader generators and what compiles their SDL3 GPU target (decision 4, D3) -----------
  # The generators are string building over a description; since ffstate.h they need nothing from
  # the DirectX SDK, so off Windows they are the same three libraries without d3d9.
  add_library(ffshader STATIC ${WW}/WW3D2/ffshader.cpp)
  target_include_directories(ffshader PUBLIC ${WW}/WW3D2)
  add_library(ffvertex STATIC ${WW}/WW3D2/ffvertex.cpp)
  target_include_directories(ffvertex PUBLIC ${WW}/WW3D2)
  add_library(engineshader STATIC ${WW}/WW3D2/engineshader.cpp)
  target_include_directories(engineshader PUBLIC ${WW}/WW3D2)
  target_link_libraries(engineshader PUBLIC ffshader)

  # ---- Text off Windows (decision 6, D6) -----------------------------------------------------------
  # FreeType 2.14.3, fetched and pinned by vendor.sh, with none of its optional dependencies: the game's
  # fonts are plain TrueType files, and zlib, bzip2, PNG, HarfBuzz and Brotli only matter to compressed,
  # colour or shaped fonts.  Its gzip module uses its own zlib copy.  EXCLUDE_FROM_ALL, like the shader
  # compilers: what links it is what builds it.
  function(zh_add_freetype)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    foreach(_ft_dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
      set(FT_DISABLE_${_ft_dep} ON)
    endforeach()
    set(SKIP_INSTALL_ALL ON)
    add_subdirectory(${LIBSRC}/freetype ${CMAKE_CURRENT_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
  endfunction()
  zh_add_freetype()

  # What FontCharsClass asks GDI for, from FreeType: the fonts, their metrics, the glyphs.  Its own
  # library, outside ww3d2's list, so it builds and is tested before WW3D2 does off Windows (D6a).
  # Linux finds Liberation through fontconfig; macOS reads fixed files and needs nothing more.
  add_library(glyphrasteriser STATIC ${WW}/WW3D2/glyphrasteriser.cpp)
  target_include_directories(glyphrasteriser PUBLIC ${WW}/WW3D2)
  target_link_libraries(glyphrasteriser PRIVATE freetype)
  if(ZH_PLATFORM_LINUX)
    find_package(Fontconfig REQUIRED)
    target_link_libraries(glyphrasteriser PRIVATE Fontconfig::Fontconfig)
  endif()

  # The Miles 6.5 surface on miniaudio (C4): the same library name as the XAudio2 one on Windows, so
  # that gameenginedevice, once it builds off Windows, links milesaudio on either and
  # MilesAudioManager.cpp compiles unchanged against it.  Today only the tests below link it here.
  add_library(milesaudio STATIC ${WW}/Miles6/miniaudio/miles_miniaudio.cpp)
  target_include_directories(milesaudio PRIVATE ${WW}/Miles6/include ${WW}/WWLib)
  target_link_libraries(milesaudio PRIVATE miniaudio wwlib PUBLIC Threads::Threads)

  # What SDL3's GPU API can do on this machine: the evidence D2 and D5 need before the backend
  # interface is designed.  Needs a GPU and a display, so it is built but not registered with ctest.
  add_executable(sdl_gpu_probe ${CMAKE_CURRENT_SOURCE_DIR}/Tests/sdl_gpu_probe.cpp)
  target_link_libraries(sdl_gpu_probe PRIVATE ${ZH_SDL3_TARGET})

  # The D-spike: one real model and its DDS textures, read from the game's archives, drawn through
  # SDL3's GPU API.  Parses W3D with WWLib's chunkio and w3d_file.h; WW3D2 itself is not built.  Needs
  # a GPU and the game's data, so it is built but not registered with ctest.  Its shaders are
  # generated ahead of time by Tests/w3d_view/shaders/build_shaders.sh and checked in, so building it
  # needs no shader compiler.
  add_executable(w3d_view
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/w3d_view/w3d_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/w3d_view/w3d_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/w3d_view/dds_image.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/w3d_view/big_archive.cpp)
  target_include_directories(w3d_view PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/w3d_view
    ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include
    ${WW}/WW3D2 ${WW}/WWLib ${WW}/WWMath ${WW}/WWDebug)
  # --shaders generated draws with the game's own generated programs, compiled as the SDL3 backend
  # will compile them (D3).
  target_link_libraries(w3d_view PRIVATE ${ZH_SDL3_TARGET} wwmath wwlib wwsaveload wwdebug
    ffvertex engineshader sdl3shadercompile)
endif()

# ---- The shader compilers for the SDL3 GPU target (decision 4, D3) ------------------------------------
# They compile the generators' SDL3 GPU target for Metal and Vulkan.
if(ZH_PLATFORM_POSIX)
  # glslang (HLSL to SPIR-V) and SPIRV-Cross (SPIR-V to MSL), fetched and pinned by vendor.sh, with
  # only the parts decision 4 uses.  Their options are set inside a function so they stay out of the
  # rest of this file, and both are EXCLUDE_FROM_ALL: what gets built is what links against them.
  function(zh_add_shader_compilers)
    # Both read options with the old CMP0077 behaviour, under which option() ignores a normal
    # variable its parent set.  Without this, every setting below would silently not apply.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(ENABLE_HLSL ON)
    set(ENABLE_OPT OFF)
    set(ENABLE_SPIRV ON)
    set(ENABLE_GLSLANG_BINARIES OFF)
    set(ENABLE_PCH OFF)
    set(GLSLANG_TESTS OFF)
    set(GLSLANG_ENABLE_INSTALL OFF)
    set(BUILD_EXTERNAL OFF)
    add_subdirectory(${LIBSRC}/glslang ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
    set(SPIRV_CROSS_CLI OFF)
    set(SPIRV_CROSS_ENABLE_TESTS OFF)
    set(SPIRV_CROSS_SHARED OFF)
    set(SPIRV_CROSS_STATIC ON)
    set(SPIRV_CROSS_ENABLE_HLSL OFF)
    set(SPIRV_CROSS_ENABLE_CPP OFF)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF)
    set(SPIRV_CROSS_ENABLE_UTIL OFF)
    set(SPIRV_CROSS_ENABLE_MSL ON)
    set(SPIRV_CROSS_ENABLE_GLSL ON)
    set(SPIRV_CROSS_ENABLE_C_API ON)
    set(SPIRV_CROSS_SKIP_INSTALL ON)
    add_subdirectory(${LIBSRC}/SPIRV-Cross ${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Cross EXCLUDE_FROM_ALL)
  endfunction()
  zh_add_shader_compilers()

  # SDL_shadercross is one C file, built here rather than through its own CMakeLists, whose vendored
  # mode insists on DXC's source even with DXC off.  No SDL_SHADERCROSS_DXC: it takes the SPIR-V glslang
  # wrote.  DXIL (and so DXC) is not used.
  add_library(sdl3_shadercross STATIC ${LIBSRC}/SDL_shadercross/src/SDL_shadercross.c)
  target_include_directories(sdl3_shadercross PUBLIC ${LIBSRC}/SDL_shadercross/include)
  target_link_libraries(sdl3_shadercross PUBLIC ${ZH_SDL3_TARGET} PRIVATE spirv-cross-c)

  # HLSL to SPIR-V to what a device takes, behind the one function that names the HLSL front end.
  add_library(sdl3shadercompile STATIC ${WW}/WW3D2/sdl3shadercompile.cpp)
  target_include_directories(sdl3shadercompile PUBLIC ${WW}/WW3D2)
  target_link_libraries(sdl3shadercompile PUBLIC ${ZH_SDL3_TARGET}
    PRIVATE sdl3_shadercross glslang::glslang glslang::glslang-default-resource-limits)

endif()

# d3d8to9 is no longer built.  It was a d3d8.dll next to generals.exe that translated
# every D3D8 call to Direct3D 9, which is how the renderer ran on a modern runtime
# without touching the ~5600 D3D8 call sites.  The renderer creates its own
# IDirect3DDevice9 now (RENDERER-ROADMAP.md phase 1), so nothing loads that dll.  The
# vendored source stays in Libraries/Source/d3d8to9: WW3D2/d3d8shadertranslate.cpp is
# lifted from it, and it is the reference for how a D3D8 call was being answered.

# ===========================================================================
# Phase 3 - GameEngine.  ~580 files of game logic; no DX8, no device code.
# ===========================================================================
set(GE ${CMAKE_CURRENT_SOURCE_DIR}/GameEngine)

# The source tree is a frozen 2003 snapshot, so a glob cannot go stale the way
# it would in a live project - and 580 explicit paths would drown this file.
file(GLOB_RECURSE GAMEENGINE_SOURCES ${GE}/Source/*.cpp)
# On disk but absent from GameEngine.dsp, i.e. never in the shipping build:
# dead experiments (SimplePlayer, urlLaunch, VideoStream), an older copy of
# DrawableManager that GameClient/Drawable/ supersedes, and GameSpeech, whose
# hash uses std::hash<const char*> on strings that outlive nothing.
list(REMOVE_ITEM GAMEENGINE_SOURCES
  ${GE}/Source/Common/Audio/GameSpeech.cpp
  ${GE}/Source/Common/Audio/simpleplayer.cpp
  ${GE}/Source/Common/Audio/urllaunch.cpp
  ${GE}/Source/GameClient/DrawableManager.cpp
  ${GE}/Source/GameClient/VideoStream.cpp)
# GameNetwork/GameSpy.h and the two .cpp files that owned it were stripped from
# the release, and these two are all that include it.  Multiplayer is out of
# scope (PLAN.md), so they stay out until the exe link says otherwise.
list(REMOVE_ITEM GAMEENGINE_SOURCES
  ${GE}/Source/GameNetwork/GameSpyChat.cpp
  ${GE}/Source/GameNetwork/GameSpyGP.cpp)
# EA's own header says "#error this file is obsolete", and this .cpp is its only
# includer - the live code went to GameNetwork/GameSpy/ instead.
list(REMOVE_ITEM GAMEENGINE_SOURCES
  ${GE}/Source/GameNetwork/GameSpyGameInfo.cpp)
# Platform bodies of one interface: each side's file is left out of the other's build, so Windows
# gets exactly the sources it had.  B5, bucket (c).
#  - StackDump.cpp: dbghelp symbol lookup and a structured-exception context walk, Windows only.
#    StackDumpPosix.cpp is the same Common/StackDump.h interface from backtrace() and dladdr(), and
#    CrashHandlerPosix.cpp is what a crash signal writes (C5): ReleaseCrashInfo.txt, as Windows'
#    structured-exception path writes it.
#  - WebBrowser.cpp: an ATL/COM wrapper around Internet Explorer for the dead WOL service's pages.
#    Nothing ever creates TheWebBrowser, on Windows either (Win32GameEngine::createWebBrowser has
#    no caller), and every caller - INIWebpageURL, WOLLadderScreen, WOLLoginMenu - tests it for NULL
#    first.  WebBrowserPosix.cpp is the same interface without COM, TheWebBrowser NULL.
#  - DownloadManager.cpp: patch downloads over wwdownload (FTP over winsock; wwdownload itself is a
#    Windows-only target above) from a dead service.  Only DownloadMenuInit creates the manager,
#    and only on Windows; MainMenu and MainMenuUtils test TheDownloadManager for NULL first.
#    DownloadManagerPosix.cpp defines it NULL.  DownloadMenu.cpp itself is built everywhere, because
#    .wnd files bind its callbacks by name through FunctionLexicon, which must be the same on
#    every platform.
#  - WWDownloadRegistryPosix.cpp: WWDownload's registry functions and FormatURLFromRegistry over
#    Registry.ini.  On Windows they come from wwdownload itself.  B6.
#  - IMEManager.cpp: the IMM32 input method editor (Chinese, Japanese and Korean text entry).  Its
#    only exports are TheIMEManager and CreateIMEManagerInterface; GameClient::init handles a NULL
#    manager and every other caller tests for one, and no test calls it.  IMEManagerPosix.cpp is the
#    manager off Windows, over the device's text events (C3): every typed character reaches a text
#    field through the IME manager, on Windows too.  (IMECandidate.cpp, whose window callbacks .wnd files
#    name through FunctionLexicon, is a different file and is built everywhere.)
if(ZH_PLATFORM_WINDOWS)
  list(REMOVE_ITEM GAMEENGINE_SOURCES
    ${GE}/Source/Common/System/StackDumpPosix.cpp
    ${GE}/Source/Common/System/CrashHandlerPosix.cpp
    ${GE}/Source/GameNetwork/WOLBrowser/WebBrowserPosix.cpp
    ${GE}/Source/GameNetwork/DownloadManagerPosix.cpp
    ${GE}/Source/GameClient/GUI/IMEManagerPosix.cpp
    ${GE}/Source/Common/System/WWDownloadRegistryPosix.cpp)
else()
  list(REMOVE_ITEM GAMEENGINE_SOURCES
    ${GE}/Source/Common/System/StackDump.cpp
    ${GE}/Source/GameNetwork/WOLBrowser/WebBrowser.cpp
    ${GE}/Source/GameNetwork/DownloadManager.cpp
    ${GE}/Source/GameClient/GUI/IMEManager.cpp)
endif()

# ZH_NOT_YET_PORTABLE: defined on macOS, kept out of the default build there until B1-B6 land.
# The link list below is left exactly as it is on purpose - B6 owns it.
add_library(gameengine STATIC ${ZH_NOT_YET_PORTABLE} ${GAMEENGINE_SOURCES})
# GameMemory.cpp asks MSVC for the engine library by the build's name (#pragma comment(lib, "GameEngineDebug")
# under _DEBUG), so a Debug build names it that way.  Release keeps gameengine.lib, which "GameEngine" finds.
if(MSVC)
  set_target_properties(gameengine PROPERTIES OUTPUT_NAME_DEBUG GameEngineDebug)
endif()

# N1 (decision 5): the build fingerprint GlobalData.cpp folds into m_exeCRC in place of the executable's
# own bytes, on every platform, so two builds of the same source agree whatever compiled them.  CRC-32 over
# the files BuildFingerprint.manifest lists (the tracked sources: Tools/fingerprint-manifest.sh writes it,
# build_fingerprint_manifest checks it), line endings normalised.  The header is rewritten only when the
# value changes, so a rebuild with nothing changed runs nothing and recompiles nothing.
set(ZH_FINGERPRINT_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/BuildFingerprint.manifest)
set(ZH_FINGERPRINT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/fingerprint)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ZH_FINGERPRINT_MANIFEST})
file(STRINGS ${ZH_FINGERPRINT_MANIFEST} ZH_FINGERPRINT_FILES)
list(TRANSFORM ZH_FINGERPRINT_FILES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

if(ZH_PLATFORM_ANDROID)
  # build_fingerprint is a host-side utility. During an Android cross-build an executable created
  # here would target arm64-v8a and could not run on the x86_64 GitHub Actions host. Use the equivalent
  # host Python implementation instead; it produces the same CRC-32 and header format.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(ZH_FINGERPRINT_COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tools/fingerprint/build_fingerprint.py
      ${CMAKE_CURRENT_SOURCE_DIR} ${ZH_FINGERPRINT_MANIFEST} ${ZH_FINGERPRINT_DIR}/BuildFingerprint.h)
  set(ZH_FINGERPRINT_DEPENDS ${ZH_FINGERPRINT_MANIFEST} ${ZH_FINGERPRINT_FILES}
      ${CMAKE_CURRENT_SOURCE_DIR}/Tools/fingerprint/build_fingerprint.py)
else()
  add_executable(build_fingerprint ${CMAKE_CURRENT_SOURCE_DIR}/Tools/fingerprint/build_fingerprint.cpp)
  set(ZH_FINGERPRINT_COMMAND
      $<TARGET_FILE:build_fingerprint> ${CMAKE_CURRENT_SOURCE_DIR} ${ZH_FINGERPRINT_MANIFEST}
      ${ZH_FINGERPRINT_DIR}/BuildFingerprint.h)
  set(ZH_FINGERPRINT_DEPENDS build_fingerprint ${ZH_FINGERPRINT_MANIFEST} ${ZH_FINGERPRINT_FILES})
endif()

add_custom_command(
  OUTPUT ${ZH_FINGERPRINT_DIR}/BuildFingerprint.stamp
  BYPRODUCTS ${ZH_FINGERPRINT_DIR}/BuildFingerprint.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ZH_FINGERPRINT_DIR}
  COMMAND ${ZH_FINGERPRINT_COMMAND}
  COMMAND ${CMAKE_COMMAND} -E touch ${ZH_FINGERPRINT_DIR}/BuildFingerprint.stamp
  DEPENDS ${ZH_FINGERPRINT_DEPENDS}
  COMMENT "Build fingerprint over the tracked sources (N1)" VERBATIM)
add_custom_target(build_fingerprint_header DEPENDS ${ZH_FINGERPRINT_DIR}/BuildFingerprint.stamp)
add_dependencies(gameengine build_fingerprint_header)
target_include_directories(gameengine PRIVATE ${ZH_FINGERPRINT_DIR})
if(ZH_PLATFORM_POSIX)
  if(NOT ZH_PLATFORM_ANDROID)
    # The executable checker is a desktop test; Android uses the same host-side generator during the build.
    add_test(NAME build_fingerprint_check
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/Tools/fingerprint-check.sh $<TARGET_FILE:build_fingerprint>
                     ${ZH_FINGERPRINT_DIR}/BuildFingerprint.h)
  endif()
  # The manifest still lists exactly the tracked files (skipped without git: a tarball uses it as shipped).
  add_test(NAME build_fingerprint_manifest COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/Tools/fingerprint-manifest.sh --check)
  set_tests_properties(build_fingerprint_manifest PROPERTIES SKIP_RETURN_CODE 77)
  # The manifest script on itself, in a scratch repository: a line twice fails --check with its name, and
  # a merge stopped on a conflict (git ls-files then names a path once per stage) is refused.
  add_test(NAME fingerprint_manifest_selfcheck COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_fingerprint_manifest.sh)
  set_tests_properties(fingerprint_manifest_selfcheck PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(ZH_PLATFORM_POSIX)
  # GlobalLanguage registers Language.ini's LocalFontFile with the glyph rasteriser (D6b)
  target_link_libraries(gameengine PUBLIC glyphrasteriser)
endif()
target_include_directories(gameengine PUBLIC
  ${GE}/Include ${GE}/Include/Precompiled
  ${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Include
  ${WW} ${WW}/WWLib
  # The game includes "gamespy/peer/peer.h", the SDK's own headers include each
  # other as "gscommon.h", so both levels have to be on the path.
  ${LIBSRC}/GameSpy/include ${LIBSRC}/GameSpy/include/gamespy ${COMP}
  # WW3D2 is here for d3dx9math.h alone: BezierSegment carries D3DXMATRIX, and
  # DumbProjectileBehavior steers a shell with it.  dinput.h now comes out of the
  # Windows SDK like d3d9.h does.
  ${WW}/WW3D2
  # nanosvg's two headers, compiled into HtmlOverlay.cpp alone: the SVG pictures a page names.
  ${LIBSRC}/nanosvg)
# _RELEASE/_DEBUG pick the game's own build flavour; IG_DEBUG_STACKTRACE and
# Z_PREFIX are what GameEngine.dsp sets.
target_compile_definitions(gameengine PUBLIC
  IG_DEBUG_STACKTRACE Z_PREFIX
  $<$<CONFIG:Debug>:_DEBUG> $<$<NOT:$<CONFIG:Debug>>:_RELEASE>)
# C++17 for the std::hash_map -> unordered_map alias and string_view hashing in
# STLTypedefs.h.
set_target_properties(gameengine PROPERTIES CXX_STANDARD 17)

# setFPMode's POSIX branch must leave floating point as Windows' default MXCSR does: round to nearest,
# denormals kept, NaN payloads propagated.  Built from FPUControl.cpp alone - it lives in its own
# translation unit for exactly this - with the engine's includes and definitions, so it runs before
# gameengine links.  Armed: it first sets the state setFPMode must undo and requires every check to
# fail there.  POSIX only for now; nothing here has run it on Windows.  See Tests/test_fpucontrol.cpp.
if(ZH_PLATFORM_POSIX)
  add_executable(fpucontrol_selfcheck ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_fpucontrol.cpp
                                      ${GE}/Source/GameLogic/System/FPUControl.cpp
                                      ${CMAKE_CURRENT_SOURCE_DIR}/Tests/gcc_eager_vtable_stubs.cpp)
  target_include_directories(fpucontrol_selfcheck PRIVATE
    $<TARGET_PROPERTY:gameengine,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(fpucontrol_selfcheck PRIVATE
    $<TARGET_PROPERTY:gameengine,INTERFACE_COMPILE_DEFINITIONS>
    ZH_EAGER_VTABLE_STUB_STRINGS)		# it links no AsciiString.cpp or UnicodeString.cpp
  set_target_properties(fpucontrol_selfcheck PROPERTIES CXX_STANDARD 17)
endif()
# Two names on this line are Windows-only, and expand to nothing elsewhere - in the same positions,
# so the Windows list is the same items in the same order it always was.  B6's first two drops:
#  - debuglib: no live GameEngine code calls it (every Debug::/DLOG/DASSERT hit is commented out).
#    It still reaches Windows through profile, which does call it.
#  - dinput8: GameEngine uses the 107 DIK_ key-code constants from <dinput.h> and no DirectInput
#    function.  It is load-bearing on Windows all the same: Win32DIKeyboard.cpp and Win32DIMouse.cpp
#    in gameenginedevice call DirectInput8Create and get dinput8 only through this PUBLIC line.
#
# And seven more that exist only on Windows, whose off-Windows parts gameengine gets elsewhere (B6).
# Off Windows none is a target, so each reached the linker as -l<name> and a POSIX link failed on it:
#  - ww3d2: gameengine's references are DX8Wrapper_PreserveFPU and DX8Wrapper_IsWindowed, which
#    PosixDevice defines, and d3dx9math.h's maths, which d3dxportable.h makes header-only (B17).
#  - wwdownload: its registry API and FormatURLFromRegistry, gameengine's own off Windows (B6).
#  - eabrowserdispatch, profile, wininet, imagehlp, imm32: Windows-only code, left out of the POSIX
#    build by B5 (WebBrowserPosix, StackDumpPosix, IMEManagerPosix, ChromaKeyboard's WinINet half).
# A link of gameengine with PosixDevice and the libraries below leaves no symbol any of them defines.
if(ZH_PLATFORM_WINDOWS)
  set(ZH_GAMEENGINE_DEBUGLIB debuglib)
  set(ZH_GAMEENGINE_DINPUT8 dinput8)
  set(ZH_GAMEENGINE_WW3D2 ww3d2)
  set(ZH_GAMEENGINE_WWDOWNLOAD wwdownload)
  set(ZH_GAMEENGINE_EABROWSERDISPATCH eabrowserdispatch)
  set(ZH_GAMEENGINE_PROFILE profile)
  set(ZH_GAMEENGINE_WIN32_SDK_LIBS wininet imagehlp imm32)
endif()
target_link_libraries(gameengine PUBLIC
  ${ZH_GAMEENGINE_WW3D2} wwlib wwmath wwsaveload wwdebug wwutil compression ${ZH_GAMEENGINE_DEBUGLIB} ${ZH_GAMEENGINE_WWDOWNLOAD}
  benchmark ${ZH_GAMEENGINE_EABROWSERDISPATCH} ${ZH_GAMEENGINE_PROFILE} gamespy litehtml
  ${ZH_GAMEENGINE_DINPUT8} ${ZH_GAMEENGINE_WIN32_SDK_LIBS})

# B1's gate: the bytes a UnicodeString puts into the replay and network CRC, before and after
# WideChar becomes char16_t.  Built from the REAL Xfer, XferCRC, UnicodeString, AsciiString and