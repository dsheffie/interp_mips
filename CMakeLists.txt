# interp_mips (Jazz/NT branch) -- cross-platform build (Linux + macOS/Apple Silicon).
#
#   cmake -B build -DMIPS_EL=ON            # little-endian guest (the --jazz / NT build)
#   cmake --build build -j
#   ./build/interp_mips_el --jazz ...
#
# softfloat and capstone are pulled in with FetchContent (no system install, no submodule/symlink).
# Boost (program_options + serialization) is the one system dep: `brew install boost` /
# `apt install libboost-all-dev`.  SDL2 is used if found, else fetched; set -DWITH_SDL=OFF to skip.
cmake_minimum_required(VERSION 3.19)
# find_package(Boost) via BoostConfig (FindBoost module is gone in CMake 3.30+)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
project(interp_mips CXX C)

option(MIPS_EL  "little-endian guest (-DMIPS_EL); the Jazz/NT build" OFF)
option(WITH_SDL "live SDL2 display + keyboard/mouse (--sdl)"         ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# match the Makefile's hot-path flags
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-O3> $<$<COMPILE_LANGUAGE:CXX>:-g>
                    $<$<COMPILE_LANGUAGE:CXX>:-fomit-frame-pointer>)
# Homebrew (Apple Silicon / Intel) prefixes, so find_package locates Boost/SDL2 without extra flags
if(APPLE)
  foreach(p /opt/homebrew /usr/local)
    if(EXISTS ${p})
      list(APPEND CMAKE_PREFIX_PATH ${p})
    endif()
  endforeach()
endif()

include(FetchContent)

# ---- softfloat (Berkeley SoftFloat-3e): fetched, built via its own Makefile (FAST_INT64/8086-SSE) ----
FetchContent_Declare(softfloat
  GIT_REPOSITORY https://github.com/ucb-bar/berkeley-softfloat-3
  GIT_TAG        a0c6494cdc11865811dec815d5c0049fba9d82a8)
FetchContent_MakeAvailable(softfloat)   # no CMakeLists in softfloat -> just populates
set(SF_BUILD_DIR ${softfloat_SOURCE_DIR}/build/Linux-x86_64-GCC)   # generic C; the label is cosmetic
set(SF_LIB ${SF_BUILD_DIR}/softfloat.a)
add_custom_command(OUTPUT ${SF_LIB}
  COMMAND ${CMAKE_COMMAND} -E env make -C ${SF_BUILD_DIR}
  WORKING_DIRECTORY ${SF_BUILD_DIR}
  COMMENT "Building softfloat (${SF_LIB})"
  VERBATIM)
add_custom_target(softfloat_build DEPENDS ${SF_LIB})
add_library(softfloat STATIC IMPORTED GLOBAL)
set_target_properties(softfloat PROPERTIES IMPORTED_LOCATION ${SF_LIB})
add_dependencies(softfloat softfloat_build)
target_include_directories(softfloat INTERFACE ${softfloat_SOURCE_DIR}/source/include)

# ---- capstone (disassembler): fetched, MIPS-only static ----
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_MIPS_SUPPORT ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(capstone
  GIT_REPOSITORY https://github.com/capstone-engine/capstone
  GIT_TAG        5.0.1)
FetchContent_MakeAvailable(capstone)

# ---- Boost: the one required system dep ----
find_package(Boost REQUIRED COMPONENTS program_options serialization)

# ---- SDL2: use the system one if present, else fetch it ----
set(SDL_TARGET "")
if(WITH_SDL)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    set(SDL_TARGET SDL2::SDL2)
  else()
    message(STATUS "SDL2 not found; fetching (set -DWITH_SDL=OFF to skip)")
    set(SDL_SHARED_DEFAULT OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC_DEFAULT ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
      GIT_REPOSITORY https://github.com/libsdl-org/SDL
      GIT_TAG        release-2.30.9)
    FetchContent_MakeAvailable(SDL2)
    if(TARGET SDL2::SDL2-static)
      set(SDL_TARGET SDL2::SDL2-static)
    elseif(TARGET SDL2-static)
      set(SDL_TARGET SDL2-static)
    elseif(TARGET SDL2::SDL2)
      set(SDL_TARGET SDL2::SDL2)
    endif()
  endif()
endif()

# ---- githash.cc (robust: "unknown" outside a git repo, e.g. a copied worktree) ----
set(GITHASH_CC ${CMAKE_BINARY_DIR}/githash.cc)
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse HEAD
                OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE GIT_RC)
if(NOT GIT_RC EQUAL 0)
  set(GIT_HASH "unknown")
endif()
file(WRITE ${GITHASH_CC} "const char *githash = \"${GIT_HASH}\";\n")

# ---- the simulator ----
set(SRCS
  main.cc pseudo_bios.cc loadelf.cc disassemble.cc helper.cc interpret.cc sparse_mem.cc
  cache_model.cc sgi_mc.cc sgi_hpc.cc sgi_scc.cc sgi_scsi.cc sgi_seeq.cc saveState.cc
  gdbstub.cc shmem.cc cosim.cc jazz.cc jazz_esp.cc jazz_sonic.cc jazz_sdl.cc ${GITHASH_CC})

if(MIPS_EL)
  set(EXE interp_mips_el)
else()
  set(EXE interp_mips)
endif()
add_executable(${EXE} ${SRCS})
if(MIPS_EL)
  target_compile_definitions(${EXE} PRIVATE MIPS_EL)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

target_link_libraries(${EXE} PRIVATE
  softfloat capstone
  Boost::program_options Boost::serialization
  Threads::Threads ${CMAKE_DL_LIBS})

# libunwind (helper.cc backtrace): Linux needs -lunwind; macOS resolves unw_* from libSystem
if(NOT APPLE)
  find_library(UNWIND_LIB unwind)
  if(UNWIND_LIB)
    target_link_libraries(${EXE} PRIVATE ${UNWIND_LIB})
  endif()
endif()

if(SDL_TARGET)
  target_link_libraries(${EXE} PRIVATE ${SDL_TARGET})
endif()
