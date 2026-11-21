# net_sim: the online code (src/net) tested without the game (docs/ONLINE_PLAN.md 12 step A).
# A separate small build: it never configures the game's own build.
#
#   cmake -S tools/net_sim -B build/net-sim -G Ninja -DCMAKE_C_COMPILER=<clang>
#   cmake --build build/net-sim && build/net-sim/net_sim
cmake_minimum_required(VERSION 3.20)
project(net_sim C)
set(CMAKE_C_STANDARD 11)
set(NET "${CMAKE_CURRENT_SOURCE_DIR}/../../src/net")
# Opus (third_party/opus, BSD, pinned v1.5.2) for voice: the library only
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/opus" opus EXCLUDE_FROM_ALL)
target_compile_options(opus PRIVATE -w)
add_executable(net_sim net_sim.c "${NET}/rollback.c" "${NET}/session.c" "${NET}/voice.c")
target_link_libraries(net_sim PRIVATE opus m)
target_compile_options(net_sim PRIVATE -Wall -Wextra -Wno-missing-field-initializers -O2 -g)
# the real UDP transport: compiled (and linked) here so it keeps building; net_sim itself never opens a socket
add_library(net_udp STATIC "${NET}/net_udp.c")
target_compile_options(net_udp PRIVATE -Wall -Wextra -O2 -g)
if(WIN32)
    target_link_libraries(net_udp PUBLIC ws2_32)
endif()
# the lobby (src/net/lobby.c) over real UDP on 127.0.0.1: a host and three machines in one process
add_executable(lobby_test lobby_test.c "${NET}/lobby.c" "${NET}/friends.c" "${NET}/session.c" "${NET}/rollback.c")
target_link_libraries(lobby_test PRIVATE net_udp)
target_compile_options(lobby_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers -O2 -g)
# the master server (tools/master/master.c, src/net/master.h): the internet's server list and hole punching
add_executable(tm2_master ../master/master.c ../master/accounts.c)
target_link_libraries(tm2_master PRIVATE net_udp)
target_compile_options(tm2_master PRIVATE -Wall -Wextra -O2 -g)
target_include_directories(tm2_master PRIVATE "${NET}")
# the first-time setup's readers against tools/disc's Python (tools/test/asset_check.py)
add_executable(asset_check ../asset_check/asset_check.c ../../src/assets/discfs.c ../../src/assets/tim.c)
target_compile_options(asset_check PRIVATE -Wall -Wextra -O2 -g)
add_executable(ui_build_check ../asset_check/ui_build_check.c ../../src/assets/uibuild.c ../../src/runtime/mdec.c ../../src/assets/discfs.c ../../src/assets/tim.c)
target_compile_options(ui_build_check PRIVATE -Wall -Wextra -O2 -g)
target_link_libraries(ui_build_check PRIVATE m)
add_executable(hd_audit ../asset_check/hd_audit.c ../../src/assets/discfs.c ../../src/assets/tim.c)
target_compile_options(hd_audit PRIVATE -Wall -Wextra -O2 -g)
# the master's friends and stats (tools/master/accounts.c) over UDP on 127.0.0.1: master_friends_test <tm2_master.exe>
add_executable(master_friends_test master_friends_test.c)
target_link_libraries(master_friends_test PRIVATE net_udp)
target_compile_options(master_friends_test PRIVATE -Wall -Wextra -O2 -g)
target_include_directories(master_friends_test PRIVATE "${NET}")
