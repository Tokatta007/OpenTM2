# tm2pc: Twisted Metal 2 recompiled for PC.
#
# The game code is generated at build time from the retail executable
# (TM2_EXE, your own copy) by tools/recomp/recompile.py.
#
#   cmake -S . -B build/port -G Ninja -DCMAKE_C_COMPILER=<clang>
#   cmake --build build/port
cmake_minimum_required(VERSION 3.20)
project(tm2pc C)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# PSY-Z (third_party/psyz, MPL-2.0 core): GPU renderer, window and SPU via
# SDL3. Our fixes to it live in third_party/patches/psyz-*.patch (applied in
# name order). Each configure resets the submodule's tracked files to the
# pinned commit and applies them, so change PSY-Z only through patches.
find_package(Git REQUIRED)
# CONFIGURE_DEPENDS and the per-file dependency: a new or changed patch
# (after a pull) reconfigures, so it is applied without a manual cmake run.
file(GLOB PSYZ_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/patches/psyz-*.patch")
list(SORT PSYZ_PATCHES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PSYZ_PATCHES})
set(PSYZ_DIR "${CMAKE_SOURCE_DIR}/third_party/psyz")
execute_process(COMMAND "${GIT_EXECUTABLE}" checkout HEAD -- . WORKING_DIRECTORY "${PSYZ_DIR}" RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "cannot reset third_party/psyz")
endif()
foreach(patch ${PSYZ_PATCHES})
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}" WORKING_DIRECTORY "${PSYZ_DIR}" RESULT_VARIABLE failed)
    if(failed)
        message(FATAL_ERROR "cannot apply ${patch} to third_party/psyz")
    endif()
    message(STATUS "Applied ${patch}")
endforeach()
add_subdirectory(third_party/psyz/psyz EXCLUDE_FROM_ALL)

set(TM2_EXE "${CMAKE_SOURCE_DIR}/references/TM2-Decomp/bin/SCUS_94306R.EXE"
    CACHE FILEPATH "Retail SCUS-94306 executable")
set(TM2_INVENTORY "${CMAKE_SOURCE_DIR}/docs/recomp_inventory.json"
    CACHE FILEPATH "The executable's function inventory (the decomp's own for a build from its source)")
set(RECOMP "${CMAKE_SOURCE_DIR}/tools/recomp")
set(GEN "${CMAKE_BINARY_DIR}/gen")

set(TM2_ALT_EXE "" CACHE FILEPATH "A/B check: the decomp's build from its source, same layout (src/runtime/abcheck.c)")
set(TM2_ALT_FUNCS "" CACHE STRING "A/B check: its functions to compare (hex addresses, comma separated, or @file)")
set(TM2_ADDRMAP "" CACHE FILEPATH "The decomp's addrmap.tsv when TM2_EXE is at a new layout (its port variant: docs/HYBRID.md)")
set(TM2_PORT_JSON "" CACHE FILEPATH "The decomp's TM2_SOURCE_PORT.json with TM2_ADDRMAP: its port sections")
set(GEN_SOURCES "${GEN}/dispatch.c" "${GEN}/funcs_alt.c" "${GEN}/addrmap.c" "${GEN}/exe_image.c")
set(REBASE_ARGS --rebase "${CMAKE_SOURCE_DIR}/src/rebase.json")
if(TM2_ADDRMAP)
    set(REBASE_ARGS --addrmap "${TM2_ADDRMAP}") # (its arrays are sized for 4 players already: no rebase of our own)
    if(TM2_PORT_JSON)
        list(APPEND REBASE_ARGS --port-json "${TM2_PORT_JSON}")
    endif()
endif()
set(ALT_ARGS "")
if(TM2_ALT_EXE AND TM2_ALT_FUNCS)
    set(ALT_ARGS --alt-exe "${TM2_ALT_EXE}" --alt-funcs "${TM2_ALT_FUNCS}")
endif()
foreach(i RANGE 0 15)
    if(i LESS 10)
        list(APPEND GEN_SOURCES "${GEN}/funcs_0${i}.c")
    else()
        list(APPEND GEN_SOURCES "${GEN}/funcs_${i}.c")
    endif()
endforeach()

add_custom_command(
    OUTPUT ${GEN_SOURCES} "${GEN}/funcs.h"
    COMMAND Python3::Interpreter "${RECOMP}/recompile.py" --exe "${TM2_EXE}" --out "${GEN}"
            --inventory "${TM2_INVENTORY}"
            --overrides "${CMAKE_SOURCE_DIR}/src/overrides.json" --files 16
            ${REBASE_ARGS} ${ALT_ARGS}
    DEPENDS "${RECOMP}/recompile.py" "${RECOMP}/mips.py" "${CMAKE_SOURCE_DIR}/src/overrides.json"
            "${CMAKE_SOURCE_DIR}/src/rebase.json"
            "${TM2_INVENTORY}" "${TM2_EXE}"
    COMMENT "Recompiling SCUS-94306 to C")

add_executable(tm2pc
    src/main.c
    src/runtime/core.c
    src/runtime/abcheck.c
    src/runtime/addrmap.c
    src/runtime/env_cache.c
    src/runtime/bios.c
    src/runtime/hw.c
    src/runtime/gpu.c
    src/runtime/gpu_null.c
    src/runtime/gpu_psyz.c
    src/runtime/input.c
    src/runtime/cd.c
    src/runtime/cd_audio.c
    src/runtime/mdec.c
    src/runtime/movie.c
    src/runtime/hle.c
    src/runtime/gte.c
    src/vplus/vplus.c
    src/vplus/fixes.c
    src/vplus/hd_textures.c
    src/vplus/four.c
    src/vplus/mirrors.c
    src/vplus/port_config.c
    src/vplus/cars.c
    src/vplus/interp.c
    src/vplus/wide.c
    src/vplus/precise.c
    src/vplus/hud_layout.c
    src/vplus/hud_styles.c
    src/vplus/world_marks.c
    src/vplus/hill_spots.c
    src/vplus/secret_routes.c
    src/vplus/killcam.c
    src/vplus/podium.c
    src/vplus/humans.c
    src/vplus/render_stats.c
    src/vplus/mod_cars.c
    src/vplus/mod_sounds.c
    src/music/music.c
    src/music/decoders.c
    src/settings.c
    src/profile/profile.c
    src/profile/profile_hooks.c
    src/profile/feats.c
    src/profile/feat_hooks.c
    src/profile/battle.c
    src/controls/bindings.c
    src/ui/draw.c
    src/ui/text.c
    src/ui/options.c
    src/ui/menu_fiber.c
    src/ui/photo.c
    src/ui/gamefont.c
    src/net/rollback.c
    src/net/rb_state.c
    src/net/session.c
    src/net/net_udp.c
    src/net/game_tm2.c
    src/net/lobby.c
    src/net/lobby_mods.c
    src/net/friends.c
    src/net/voice.c
    src/net/voice_io.c
    src/runtime/update.c
    src/runtime/update_ui.c
    src/runtime/setup.c
    src/assets/uibuild.c
    src/assets/discfs.c
    src/assets/tim.c
    src/mods/mods.c
    src/mods/mod_files.c
    src/mods/mod_wav.c
    src/mods/mod_video.c
    src/mods/mod_arena.c
    third_party/miniz/miniz.c
    ${GEN_SOURCES})
target_include_directories(tm2pc PRIVATE src src/runtime "${GEN}")
target_link_libraries(tm2pc PRIVATE psyz)
# Opus (third_party/opus, BSD, pinned v1.5.2): voice chat (net/voice.c), the library only
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/opus" opus EXCLUDE_FROM_ALL)
target_compile_options(opus PRIVATE -w)
target_link_libraries(tm2pc PRIVATE opus)
if(WIN32)
    target_link_libraries(tm2pc PRIVATE ws2_32 winhttp) # net/net_udp.c; runtime/update.c
endif()
# The version (runtime/update.c compares it with the newest release's tag); the updater's zips (miniz, MIT, 3.0.2)
set(TM2PC_VERSION "v0.9.0")
target_compile_definitions(tm2pc PRIVATE TM2PC_VERSION="${TM2PC_VERSION}")
target_include_directories(tm2pc PRIVATE third_party/miniz)
set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
if(WIN32)
    target_link_options(tm2pc PRIVATE -mwindows) # no console window (main.c: setup_output)
endif()
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
# getenv from a table read once (src/runtime/env_cache.h): every source of the game, the generated code too
target_compile_options(tm2pc PRIVATE "SHELL:-include ${CMAKE_SOURCE_DIR}/src/runtime/env_cache.h")
# Speed (the 2026-11-15 performance pass): -O3 and ThinLTO (the GTE's and memory's helpers inlined into the generated
# code). -ffp-contract=off: no fused multiply-adds, so every PC computes the same floats (online's rollback compares
# the machines' states); never -ffast-math or -march=native.
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(tm2pc PRIVATE -O3 -ffp-contract=off -flto=thin)
    target_link_options(tm2pc PRIVATE -flto=thin)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
# Music decoders (public domain single-file libraries, pinned in COMMIT files)
target_include_directories(tm2pc PRIVATE third_party/dr_libs third_party/stb third_party/pl_mpeg)
set_source_files_properties(src/music/decoders.c src/ui/text.c PROPERTIES COMPILE_OPTIONS "-w")
# tools/font/make_atlas.c: the menu lettering's glyph atlas from an installed font (only when asked for)
add_executable(font_atlas EXCLUDE_FROM_ALL tools/font/make_atlas.c)
target_include_directories(font_atlas PRIVATE third_party/stb)
set_source_files_properties(tools/font/make_atlas.c PROPERTIES COMPILE_OPTIONS "-w")
# The menu fonts (assets/fonts: Averia, SIL Open Font License; flexure.atlas, by permission of T26) go next to the
# executable
add_custom_command(TARGET tm2pc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:tm2pc>/fonts"
    # the port's own pictures (assets/art: the Challenge Match modes' symbols, game-icons.net, CC BY 3.0)
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/art" "$<TARGET_FILE_DIR:tm2pc>/art"
    # the modding guides and templates (mods/README.txt, each kind's _TEMPLATE; a player's own mods are left alone)
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/mods" "$<TARGET_FILE_DIR:tm2pc>/mods")
