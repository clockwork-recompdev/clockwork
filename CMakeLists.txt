cmake_minimum_required(VERSION 3.24)
project(yw2_recomp C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Each game is its own executable: the shared runtime plus that game's recompiled code (the
# output of `recomp emit`, generated from the player's own dump; never committed). A game is
# built when its generated code is there. The launcher hands a dump to whichever one runs it.
set(YW2_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated" CACHE PATH
    "Generated C for Yo-kai Watch 2: Psychic Specters (the yw2 executable)")
set(YW2BF_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated-yw2bf" CACHE PATH
    "Generated C for Yo-kai Watch 2: Bony Spirits and Fleshy Souls (the yw2bf executable)")
set(BLASTERS_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated-blasters" CACHE PATH
    "Generated C for Yo-kai Watch Blasters: Red Cat Corps and White Dog Squad (the blasters executable)")
set(YW3_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated-yw3" CACHE PATH
    "Generated C for Yo-kai Watch 3 (the yw3 executable)")
set(BUSTERS2_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated-busters2" CACHE PATH
    "Generated C for Yo-kai Watch Busters 2: Sword and Magnum (the busters2 executable)")
set(YW1_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated-yw1" CACHE PATH
    "Generated C for Yo-kai Watch (the yw1 executable)")
set(CLOCKWORK_GAMES "yw1=${YW1_GENERATED_DIR}" "yw2=${YW2_GENERATED_DIR}" "yw2bf=${YW2BF_GENERATED_DIR}" "blasters=${BLASTERS_GENERATED_DIR}"
    "yw3=${YW3_GENERATED_DIR}" "busters2=${BUSTERS2_GENERATED_DIR}")

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN) # windows.h's min/max macros break std::min
endif()

add_library(runtime OBJECT
    runtime/src/arm_ops.c
    runtime/src/audio/dsp_hle.cpp
    runtime/src/dump.cpp
    runtime/src/frontend/autobuild.cpp
    runtime/src/game_code.cpp
    runtime/src/level5.cpp
    runtime/src/frontend/gamebanana.cpp
    runtime/src/frontend/handoff.cpp
    runtime/src/frontend/json.cpp
    runtime/src/frontend/launcher.cpp
    runtime/src/frontend/saves.cpp
    runtime/src/frontend/sdl.cpp
    runtime/src/frontend/settings.cpp
    runtime/src/hle/apt.cpp
    runtime/src/hle/cecd.cpp
    runtime/src/hle/cfg.cpp
    runtime/src/hle/dsp.cpp
    runtime/src/hle/err.cpp
    runtime/src/hle/frd.cpp
    runtime/src/hle/fs.cpp
    runtime/src/hle/gsp.cpp
    runtime/src/hle/hid.cpp
    runtime/src/hle/offline.cpp
    runtime/src/hle/ptm.cpp
    runtime/src/hle/service.cpp
    runtime/src/hle/srv.cpp
    runtime/src/hle/stubs.cpp
    runtime/src/hle/uds.cpp
    runtime/src/hle/y2r.cpp
    runtime/src/kernel/kernel.cpp
    runtime/src/kernel/svc.cpp
    runtime/src/loader.cpp
    runtime/src/log.cpp
    runtime/src/main.cpp
    runtime/src/memory.cpp
    runtime/src/mods.cpp
    runtime/src/net/air.cpp
    runtime/src/net/relay.cpp
    runtime/src/platform.cpp
    runtime/src/video/gpu/gpu.cpp
    runtime/src/video/lighting.cpp
    runtime/src/video/pica.cpp
    runtime/src/video/raster.cpp
    runtime/src/video/shader.cpp
    runtime/src/video/texture.cpp
    runtime/src/video/transfer.cpp
    runtime/src/video/widescreen.cpp
)
target_include_directories(runtime PUBLIC runtime/src runtime/include)

# Dear ImGui (launcher UI), vendored
add_library(imgui STATIC
    runtime/third_party/imgui/imgui.cpp
    runtime/third_party/imgui/imgui_draw.cpp
    runtime/third_party/imgui/imgui_tables.cpp
    runtime/third_party/imgui/imgui_widgets.cpp
    runtime/third_party/imgui/backends/imgui_impl_sdl3.cpp
    runtime/third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC runtime/third_party/imgui runtime/third_party/imgui/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)
target_link_libraries(runtime PUBLIC imgui)
find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3)
target_link_libraries(runtime PUBLIC Threads::Threads SDL3::SDL3)
# mods from GameBanana: HTTPS downloads, and unpacking zip/7z/rar
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
target_link_libraries(runtime PUBLIC CURL::libcurl LibArchive::LibArchive)
if(WIN32)
    target_link_libraries(runtime PUBLIC ws2_32) # local wireless over UDP (net/air.cpp)
endif()

# the relay server for Rooms: just sockets, no SDL, so it runs on any small server
add_executable(clockwork-relay runtime/src/net/relay.cpp runtime/src/net/relay_main.cpp)
target_include_directories(clockwork-relay PRIVATE runtime/src)
if(WIN32)
    target_link_libraries(clockwork-relay PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(runtime PRIVATE /W4)
else()
    target_compile_options(runtime PRIVATE -Wall -Wextra)
    target_link_libraries(runtime PUBLIC m)
endif()

# Recompiled game code. VFP multiply-accumulate rounds the product separately, so no FMA
# contraction; wrapping signed arithmetic matches the ARM instructions it came from.
set(built_games "")
foreach(entry IN LISTS CLOCKWORK_GAMES)
    string(REPLACE "=" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 dir)
    file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.c")
    if(NOT sources)
        continue()
    endif()
    add_library(${name}_code OBJECT ${sources})
    target_include_directories(${name}_code PUBLIC runtime/include ${dir})
    if(MSVC)
        target_compile_options(${name}_code PRIVATE /fp:precise /fp:contract- /w)
    else()
        target_compile_options(${name}_code PRIVATE -ffp-contract=off -fwrapv -w)
    endif()
    # which executable this is, for handing dumps to the right one, and its code
    file(WRITE "${CMAKE_BINARY_DIR}/${name}_binary.c"
         "/* generated by CMakeLists.txt */\n#include \"recomp.h\"\n"
         "const char *const clockwork_binary = \"${name}\";\n"
         "const RecompEntry *const clockwork_linked_functions = recomp_functions;\n"
         "const size_t *const clockwork_linked_function_count = &recomp_function_count;\n")
    add_executable(${name} "${CMAKE_BINARY_DIR}/${name}_binary.c")
    target_link_libraries(${name} PRIVATE runtime ${name}_code)
    list(APPEND built_games ${name})
endforeach()
message(STATUS "Games: ${built_games}")

# The clockwork program: the launcher and runtime with no game in it. It plays the game modules
# the launcher builds from the player's dumps (see recomp_module.h), and is what a packaged release
# ships. The module's own sources (runtime/module/module.c, arm_ops.c, recomp*.h) go with it
file(WRITE "${CMAKE_BINARY_DIR}/clockwork_binary.c"
     "/* generated by CMakeLists.txt */\n#include \"recomp.h\"\n"
     "const char *const clockwork_binary = \"clockwork\";\n"
     "const RecompEntry *const clockwork_linked_functions = 0;\n"
     "const size_t *const clockwork_linked_function_count = 0;\n")
add_executable(clockwork "${CMAKE_BINARY_DIR}/clockwork_binary.c")
target_include_directories(clockwork PRIVATE runtime/include)
target_link_libraries(clockwork PRIVATE runtime)
