cmake_minimum_required(VERSION 3.20)

# PS Vita build (VitaSDK). The desktop build lives in ../CMakeLists.txt and is untouched by this file.
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "Set VITASDK (e.g. export VITASDK=$HOME/vitasdk)")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
endif()

project(RogueSquadronVita LANGUAGES C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Newlib hides POSIX functions (setenv, posix_memalign) under strict -std=c++20; glibc's g++ defines this implicitly.
add_compile_definitions(_GNU_SOURCE)

set(RS64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(RECOMP_FUNCS_DIR "${RS64_ROOT}/RecompiledFuncs")

# ---- N64ModernRuntime (ultramodern + librecomp) ----
add_subdirectory(${RS64_ROOT}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# ---- RecompiledFuncs ----
add_library(RecompiledFuncs STATIC)
target_compile_definitions(RecompiledFuncs PRIVATE main=rs_main)
target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-label
    -Wno-implicit-function-declaration
    -include "${RS64_ROOT}/include/rsp_dpc_macros.h"
)
target_include_directories(RecompiledFuncs PRIVATE
    ${RS64_ROOT}/include
    ${RS64_ROOT}/lib/N64ModernRuntime/ultramodern/include
    ${RS64_ROOT}/lib/N64ModernRuntime/librecomp/include
    ${RS64_ROOT}/lib/N64ModernRuntime/N64Recomp/include
)
file(GLOB RECOMP_C_SOURCES   CONFIGURE_DEPENDS ${RECOMP_FUNCS_DIR}/*.c)
file(GLOB RECOMP_CXX_SOURCES CONFIGURE_DEPENDS ${RECOMP_FUNCS_DIR}/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${RECOMP_C_SOURCES} ${RECOMP_CXX_SOURCES})

# ---- Generated inputs shared with the desktop build ----
# The RSP ucode C files and the game-state table are produced by the desktop pipeline (RSPRecomp + tools/, see docs/vita).
set(FACTOR5_BOOT_C  ${RS64_ROOT}/build/factor5_ucode/factor5_boot_recompiled.c)
set(FACTOR5_UCODE_C ${RS64_ROOT}/build/factor5_ucode/factor5_ucode_recompiled.c)
set(MUSYX_AUDIO_C   ${RS64_ROOT}/build/factor5_ucode/musyx_audio_recompiled.c)
set(STATE_TABLE_INL ${RS64_ROOT}/src/main/state_table.inl)
foreach(generated ${FACTOR5_BOOT_C} ${FACTOR5_UCODE_C} ${MUSYX_AUDIO_C} ${STATE_TABLE_INL})
    if (NOT EXISTS ${generated})
        message(FATAL_ERROR "Missing generated file ${generated}; run the desktop configure/RSPRecomp steps first (docs/vita/progress.md)")
    endif()
endforeach()
set_source_files_properties(${FACTOR5_BOOT_C} ${FACTOR5_UCODE_C} ${MUSYX_AUDIO_C} PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "-include;${RS64_ROOT}/include/rsp_dpc_macros.h;-DSET_DPC_START=RSP_DPC_START;-DSET_DPC_END=RSP_DPC_END")

# ---- Dear ImGui core (main.cpp's debug panels compile against it; nothing draws them on Vita) ----
set(IMGUI_DIR ${RS64_ROOT}/lib/rt64/src/contrib/imgui)
add_library(imgui_core STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(imgui_core PUBLIC ${IMGUI_DIR})

# ---- Renderer: RT64's Factor 5 display-list parser over the Vita state shim (src/vita_gfx) ----
set(RT64_SRC ${RS64_ROOT}/lib/rt64/src)
add_library(vita_gfx STATIC
    ${RT64_SRC}/gbi/rt64_gbi_f3d.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f3dex.cpp
    ${RT64_SRC}/gbi/rt64_gbi_rdp.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f3dfactor5.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f5_rdpstate.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f5_diag.cpp
    ${RT64_SRC}/common/rt64_common.cpp
    ${RT64_SRC}/common/rt64_math.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_state.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_rsp.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_rdp.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_renderer.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_shader.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_texture.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_backend_gl.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_submit.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_prof.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_controls_panel.cpp
)
# The shim directory comes first so RT64's "hle/rt64_state.h" etc. resolve to the Vita versions.
target_include_directories(vita_gfx PUBLIC
    ${RS64_ROOT}/src/vita_gfx/shim
    ${RS64_ROOT}/src/vita_gfx
    ${RS64_ROOT}/lib/N64ModernRuntime/thirdparty
    ${RT64_SRC}
    ${RT64_SRC}/contrib
    ${RT64_SRC}/contrib/hlslpp/include
)
# hlslpp's scalar path (forced on GCC) uses labs() without <stdlib.h>.
target_compile_options(vita_gfx PRIVATE -fno-strict-aliasing -include stdlib.h)
# Link-time optimization inlines RT64's small out-of-line helpers (DisplayList::p0/p1, State, Renderer) into the parser's hot paths.
# Fat objects keep a plain link working; the executable's link runs LTO over these objects only. ROGUESQ_VITA_LTO=OFF disables.
option(ROGUESQ_VITA_LTO "LTO for the Vita renderer" ON)
if (ROGUESQ_VITA_LTO)
    target_compile_options(vita_gfx PRIVATE -flto=auto -ffat-lto-objects)
endif()
# The parser runs on one thread; thread-safe static guards cost a dmb per access on ARMv7 (several per display-list command).
set_source_files_properties(
    ${RT64_SRC}/gbi/rt64_gbi_f3d.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f3dex.cpp
    ${RT64_SRC}/gbi/rt64_gbi_rdp.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f3dfactor5.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f5_rdpstate.cpp
    ${RT64_SRC}/gbi/rt64_gbi_f5_diag.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_state.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_rsp.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_rdp.cpp
    ${RS64_ROOT}/src/vita_gfx/vg_renderer.cpp
    PROPERTIES COMPILE_OPTIONS -fno-threadsafe-statics)
# SLP packs per-vertex float stores into a stack round trip (4 single stores reloaded as 2 doubles), which stalls the A9's store forwarding.
set_property(SOURCE ${RS64_ROOT}/src/vita_gfx/vg_rsp.cpp ${RS64_ROOT}/src/vita_gfx/vg_renderer.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-tree-slp-vectorize)
# RT64's shared/ headers are dual HLSL/C++; HLSL_CPU selects the C++ (hlslpp) side, as in the desktop build.
# RT64_VITA_SHIM selects the Vita fast paths in RT64's Factor 5 parser (the desktop build never defines it).
target_compile_definitions(vita_gfx PUBLIC HLSL_CPU RT64_VITA_SHIM)

# ---- Game executable ----
find_package(SDL2 REQUIRED)

add_executable(RogueSquadronVita
    ${RS64_ROOT}/src/main/main.cpp
    ${RS64_ROOT}/src/main/game_state.cpp
    ${RS64_ROOT}/src/main/nav_sequencer.cpp
    ${RS64_ROOT}/src/main/input_bindings.cpp
    ${RS64_ROOT}/src/main/rumble.cpp
    ${RS64_ROOT}/src/main/touch_input.cpp
    ${RS64_ROOT}/src/main/touch_menu.cpp
    ${RS64_ROOT}/src/main/touch_config.cpp
    ${RS64_ROOT}/src/main/menu_config.cpp
    ${RS64_ROOT}/src/main/register_overlays.cpp
    ${RS64_ROOT}/src/main/upstream_compat.cpp
    ${RS64_ROOT}/src/main/hook_helpers.cpp
    ${RS64_ROOT}/src/main/vita_host.cpp
    ${RS64_ROOT}/src/main/vita_render_context.cpp
    ${RS64_ROOT}/src/main/vita_bench.cpp
    ${RS64_ROOT}/src/main/vita_controls_ui.cpp
    ${RS64_ROOT}/src/main/vg_capture.cpp
    ${RS64_ROOT}/src/rsp/aspMain.cpp
    ${RS64_ROOT}/src/rsp/dpc_bridge.cpp
    ${RS64_ROOT}/src/rsp/musyx_hle_runner.cpp
    ${RS64_ROOT}/src/rsp/musyx_hle/musyx.c
    ${RS64_ROOT}/src/rsp/musyx_hle/audio.c
    ${RS64_ROOT}/src/rsp/musyx_hle/memory.c
    ${FACTOR5_BOOT_C}
    ${FACTOR5_UCODE_C}
    ${MUSYX_AUDIO_C}
)

target_include_directories(RogueSquadronVita PRIVATE
    ${RS64_ROOT}/include
    ${RS64_ROOT}/src/main
    ${RECOMP_FUNCS_DIR}
    ${RS64_ROOT}/lib/N64ModernRuntime/N64Recomp/include
    ${RS64_ROOT}/lib/N64ModernRuntime/ultramodern/include
    ${RS64_ROOT}/lib/N64ModernRuntime/librecomp/include
    ${RS64_ROOT}/lib/N64ModernRuntime/thirdparty
    ${RS64_ROOT}/lib/rt64/src/contrib
)
target_compile_options(RogueSquadronVita PRIVATE -fno-strict-aliasing -fms-extensions)

# src/main redefines some runtime libultra functions (upstream_compat.cpp); command-line objects win, as on Linux.
target_link_options(RogueSquadronVita PRIVATE -Wl,--allow-multiple-definition)
# Attribute-less pthread_create (std::thread) gets a 1 MB stack instead of VitaSDK's 32 KB default (vita_host.cpp).
target_link_options(RogueSquadronVita PRIVATE -Wl,--wrap=pthread_create)
# vita-elf-create appends ~4.4 KB of SCE data after the text segment; without headroom it fails whenever text ends near a 64 KB boundary.
target_link_options(RogueSquadronVita PRIVATE -Wl,--defsym=__sce_headroom=0x2000)
if (ROGUESQ_VITA_LTO)
    target_link_options(RogueSquadronVita PRIVATE -flto=auto)
endif()

target_link_libraries(RogueSquadronVita PRIVATE
    vita_gfx
    RecompiledFuncs
    librecomp
    ultramodern
    imgui_core
    SDL2::SDL2
    # vitaGL/vitashark reference SceShaccCgExt, which references taihen; GNU ld resolves left to right.
    vitaGL
    vitashark
    SceShaccCgExt
    taihen_stub
    # Whole archive: libstdc++ checks weak pthread symbols to decide threads are active, otherwise std::thread throws.
    -Wl,--whole-archive pthread -Wl,--no-whole-archive
    ScePower_stub
    SceIofilemgr_stub
)

# ---- SELF + VPK ----
set(VITA_TITLEID "RSQD64001")
set(VITA_VERSION "00.01")
# ATTRIBUTE2=12: extended memory budget for the app.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

vita_create_self(eboot.bin RogueSquadronVita UNSAFE)
# Bubble icon and LiveArea are built locally from copyrighted box art (tools/vita/livearea_from_art.py); without them the VPK gets the system defaults.
set(LIVEAREA_FILES)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png)
    foreach(file icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml)
        list(APPEND LIVEAREA_FILES FILE sce_sys/${file} sce_sys/${file})
    endforeach()
else()
    message(STATUS "No LiveArea images in vita/sce_sys; run tools/vita/livearea_from_art.py to add them")
endif()
vita_create_vpk(RogueSquadronVita.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME "Rogue Squadron"
    ${LIVEAREA_FILES}
)
