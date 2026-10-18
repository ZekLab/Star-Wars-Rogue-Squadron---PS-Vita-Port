cmake_minimum_required(VERSION 3.20)

# Mac tool: replays a ROGUESQ_VG_CAPTURE frame through the Vita renderer (src/vita_gfx) with the desktop GL backend.
# cmake -S tools/vita/vg_replay -B build-vg-replay -G Ninja && ninja -C build-vg-replay
project(vg_replay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RS64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(RT64_SRC ${RS64_ROOT}/lib/rt64/src)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(vg_replay
    vg_replay.cpp
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

# Same include order as the Vita build: the shim first so RT64's hle/ headers resolve to it.
target_include_directories(vg_replay PRIVATE
    ${RS64_ROOT}/src/vita_gfx/shim
    ${RS64_ROOT}/src/vita_gfx
    ${RS64_ROOT}/lib/N64ModernRuntime/thirdparty
    ${RS64_ROOT}/src/main
    ${RT64_SRC}
    ${RT64_SRC}/contrib
    ${RT64_SRC}/contrib/hlslpp/include
)
target_compile_definitions(vg_replay PRIVATE HLSL_CPU RT64_VITA_SHIM GL_SILENCE_DEPRECATION)
# No fused multiply-add: the Vita's VFPv3/NEON has none, so the draw checksum matches the console's float results.
target_compile_options(vg_replay PRIVATE -fno-strict-aliasing -ffp-contract=off -include stdlib.h -Wno-deprecated-declarations)
target_link_libraries(vg_replay PRIVATE SDL2::SDL2 OpenGL::GL)
