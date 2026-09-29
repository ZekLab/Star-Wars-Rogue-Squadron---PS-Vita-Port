cmake_minimum_required(VERSION 3.20)

project(librecomp)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

# Define the library
add_library(librecomp STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cont.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eep.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/euc-jp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/extensions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/files.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/flash.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/heap.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/math_routines.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mods.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_events.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_hooks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_config_api.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/overlays.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pak.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/patcher.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/print.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fault_guard.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rsp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ultra_stubs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ultra_translation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/o1heap/o1heap/o1heap.c"
)

target_include_directories(librecomp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/../ultramodern/include"
    "${PROJECT_SOURCE_DIR}/../thirdparty"
    "${PROJECT_SOURCE_DIR}/../thirdparty/concurrentqueue"
    "${PROJECT_SOURCE_DIR}/../thirdparty/o1heap"
)
target_include_directories(librecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/librecomp"
)

target_compile_options(librecomp PRIVATE
#    -Wall
#    -Wextra
    -Wno-unused-parameter
)

if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

add_subdirectory(${PROJECT_SOURCE_DIR}/../thirdparty/miniz ${CMAKE_CURRENT_BINARY_DIR}/miniz)

if (VITA)
    # PS Vita: no dynamic libraries, no executable memory for the live recompiler, no POSIX signals.
    # Mod support is replaced by no-op stubs and the recompiler library is not linked.
    set(LIBRECOMP_VITA_EXCLUDED
        "${CMAKE_CURRENT_SOURCE_DIR}/src/mods.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_events.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_hooks.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_manifest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_config_api.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/fault_guard.cpp"
    )
    get_target_property(LIBRECOMP_SOURCES librecomp SOURCES)
    list(REMOVE_ITEM LIBRECOMP_SOURCES ${LIBRECOMP_VITA_EXCLUDED})
    list(APPEND LIBRECOMP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/mods_disabled.cpp")
    set_target_properties(librecomp PROPERTIES SOURCES "${LIBRECOMP_SOURCES}")
    target_include_directories(librecomp PUBLIC
        "${PROJECT_SOURCE_DIR}/../N64Recomp/include"
        "${PROJECT_SOURCE_DIR}/../N64Recomp/lib/rabbitizer/include"
        "${PROJECT_SOURCE_DIR}/../N64Recomp/lib/rabbitizer/cplusplus/include")
    target_link_libraries(librecomp PRIVATE ultramodern)
else()
    add_subdirectory(${PROJECT_SOURCE_DIR}/../N64Recomp ${CMAKE_CURRENT_BINARY_DIR}/N64Recomp EXCLUDE_FROM_ALL)
    target_link_libraries(librecomp PRIVATE ultramodern N64Recomp LiveRecomp)
endif()
target_link_libraries(librecomp PUBLIC miniz)
