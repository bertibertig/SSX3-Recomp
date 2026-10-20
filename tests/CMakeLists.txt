# Host unit tests: none of them needs game data or generated code, so they
# build and run on a fresh clone (and in CI, .github/workflows/ci.yml).
#   cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure

set(SSX3_RT ${XBOXRECOMP_DIR}/src)

# Packet writer/reader of the LAN net core.
add_executable(test_net_wire net_wire.c)
target_include_directories(test_net_wire PRIVATE ${CMAKE_SOURCE_DIR}/src/net)
add_test(NAME net_wire COMMAND test_net_wire)

# Net core end to end on loopback: host, 11 bots, lobby, start, results,
# reconnect and the LAN browser (tools/lanbot/lanbot.c).
add_test(NAME lanbot_selftest COMMAND ssx3_lanbot selftest)
set_tests_properties(lanbot_selftest PROPERTIES TIMEOUT 180)

# x86-64 MMIO decoder over a Linux ucontext (needs the runtime patch,
# patches/xboxrecomp/0001-*).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(test_mmio_decode mmio_decode_x64_linux.c)
    target_include_directories(test_mmio_decode PRIVATE ${SSX3_RT}/platform)
    add_test(NAME mmio_decode_x64 COMMAND test_mmio_decode)
endif()

# Controller layer (src/input/ssx3_pad.c) against an SDL virtual pad. The
# Windows build of ssx3_pad.c reads XInput instead, which needs real hardware.
if(NOT WIN32)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        add_executable(test_pad_virtual pad_virtual.c
            ${CMAKE_SOURCE_DIR}/src/input/ssx3_pad.c ${SSX3_RT}/input/keyboard.c)
        target_include_directories(test_pad_virtual PRIVATE ${SSX3_RT}/input ${SSX3_RT})
        target_link_libraries(test_pad_virtual PRIVATE SDL2::SDL2)
        add_test(NAME pad_virtual COMMAND test_pad_virtual)
        set_tests_properties(pad_virtual PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    endif()
endif()
