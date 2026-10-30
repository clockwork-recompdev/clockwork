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
# Each executable's folder is the cache variable <NAME>_GENERATED_DIR (the launcher's builder reads
# it): build/generated for yw2, build/generated-<name> for the rest. One executable per region's
# release (see KNOWN_DUMPS in runtime/src/frontend/games.h), and one more, <name>_upd, for its
# code as updated (code_binary in runtime/src/frontend/handoff.h)
set(CLOCKWORK_EXECUTABLES
    "yw1|Yo-kai Watch"
    "yw2|Yo-kai Watch 2: Psychic Specters"
    "yw2bf|Yo-kai Watch 2: Bony Spirits and Fleshy Souls"
    "blasters|Yo-kai Watch Blasters: Red Cat Corps and White Dog Squad"
    "yw3|Yo-kai Watch 3"
    "busters2|Yo-kai Watch Busters 2: Sword and Magnum (Japan)"
    "yw1_eur|Yo-kai Watch (Europe)"
    "yw2_eur|Yo-kai Watch 2: Psychic Specters (Europe)"
    "yw2bf_eur|Yo-kai Watch 2: Bony Spirits and Fleshy Souls (Europe)"
    "blasters_eur|Yo-kai Watch Blasters: Red Cat Corps and White Dog Squad (Europe)"
    "yw3_eur|Yo-kai Watch 3 (Europe)"
    "yw1_jpn|Yo-kai Watch (Japan)"
    "yw2_jpn|Yo-kai Watch 2: Shin'uchi (Japan)"
    "yw2bf_jpn|Yo-kai Watch 2: Ganso and Honke (Japan)"
    "blasters_jpn|Yo-kai Watch Busters: Red Cat Corps and White Dog Squad (Japan)"
    "yw3_jpn|Yo-kai Watch 3: Sushi and Tempura (Japan)"
    "yw3sk_jpn|Yo-kai Watch 3: Sukiyaki (Japan)"
    "yw1_kor|Yo-kai Watch (Korea)"
    "yw2_kor|Yo-kai Watch 2: Psychic Specters (Korea)"
    "yw2bf_kor|Yo-kai Watch 2: Bony Spirits and Fleshy Souls (Korea)"
    "blasters_kor|Yo-kai Watch Blasters: Red Cat Corps and White Dog Squad (Korea)")
set(CLOCKWORK_GAMES "")
foreach(entry IN LISTS CLOCKWORK_EXECUTABLES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 what)
    string(TOUPPER "${name}" var)
    if(name STREQUAL "yw2")
        set(default_dir "${CMAKE_SOURCE_DIR}/build/generated")
    else()
        set(default_dir "${CMAKE_SOURCE_DIR}/build/generated-${name}")
    endif()
    set(${var}_GENERATED_DIR "${default_dir}" CACHE PATH "Generated C for ${what} (the ${name} executable)")
    set(${var}_UPD_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated-${name}_upd" CACHE PATH
        "Generated C for ${what}, updated (the ${name}_upd executable)")
    list(APPEND CLOCKWORK_GAMES "${name}=${${var}_GENERATED_DIR}" "${name}_upd=${${var}_UPD_GENERATED_DIR}")
endforeach()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN) # windows.h's min/max macros break std::min
endif()

add_library(runtime OBJECT
    runtime/src/arm_ops.c
    runtime/src/audio/bcstm.cpp
    runtime/src/audio/dsp_hle.cpp
    runtime/src/dump.cpp
    runtime/src/frontend/autobuild.cpp
    runtime/src/game_code.cpp
    runtime/src/level5.cpp
    runtime/src/frontend/gamebanana.cpp
    runtime/src/frontend/handoff.cpp
    runtime/src/frontend/json.cpp
    runtime/src/frontend/launcher.cpp
    runtime/src/frontend/menu_music.cpp
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
    runtime/src/video/upscale.cpp
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
