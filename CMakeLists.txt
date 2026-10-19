cmake_minimum_required(VERSION 3.20)

# ============================================================================
# SSX 3 (Xbox) - Static Recompilation
#
# Builds on Windows (MSVC or MinGW: D3D11/XInput/XAudio2) and on Linux/macOS
# (GCC/Clang: SDL2 + OpenGL 3.3 via libepoxy). The runtime picks its backend
# per platform; this file only adds the host-specific link libraries.
# ============================================================================

project(ssx3_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# Host game controllers: our src/input/ssx3_pad.c replaces the runtime's
# xinput_device.c in its xbox_input library (same API, xinput_xbox.h), so the
# submodule stays unmodified. Adds hotplug, the binding table and the
# Black/White fix -- see the file's header.
get_target_property(SSX3_INPUT_SOURCES xbox_input SOURCES)
list(REMOVE_ITEM SSX3_INPUT_SOURCES xinput_device.c)
set_property(TARGET xbox_input PROPERTY SOURCES
    ${SSX3_INPUT_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/src/input/ssx3_pad.c)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 250, you get recomp_0000.c, recomp_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

# WIN32 = GUI subsystem on Windows (WinMain); ignored elsewhere.
add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    # MinGW's libxinput.a imports xinput1_3.dll, which only the DirectX 2010
    # redistributable installs; xinput1_4.dll ships with Windows 8 and later.
    # MSVC's xinput.lib already means xinput1_4.
    set(SSX3_XINPUT xinput)
    if(MINGW)
        set(SSX3_XINPUT xinput1_4)
        get_target_property(SSX3_INPUT_LINK xbox_input INTERFACE_LINK_LIBRARIES)
        list(TRANSFORM SSX3_INPUT_LINK REPLACE "^xinput$" xinput1_4)
        set_property(TARGET xbox_input PROPERTY INTERFACE_LINK_LIBRARIES ${SSX3_INPUT_LINK})
    endif()
    # Windows SDK libraries needed at link time
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        ${SSX3_XINPUT}
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
else()
    # POSIX host: SDL2/epoxy come in through the runtime targets. The crash
    # handler names the faulting guest function with dladdr, which only sees
    # exported symbols -> export the executable's symbols (-rdynamic).
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)
endif()

# ── Vulkan render back end (src/render, docs/vulkan-backend.md) ──
#
# Vulkan-Headers and volk are pinned submodules under third_party/, so no
# Vulkan SDK is needed: volk loads libvulkan / vulkan-1.dll at run time.
# POSIX only for now -- the window comes from SDL2, which the Windows
# runtime does not use (a Win32 surface is milestone 4). Selected at run
# time with SSX3_RENDERER=vulkan.
option(SSX3_VULKAN "Build the Vulkan render back end" ON)
if(SSX3_VULKAN AND NOT WIN32)
    set(SSX3_VK_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include)
    set(SSX3_VOLK_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk)
    if(NOT EXISTS ${SSX3_VK_HEADERS}/vulkan/vulkan_core.h OR NOT EXISTS ${SSX3_VOLK_DIR}/volk.c)
        message(FATAL_ERROR "Vulkan submodules missing: git submodule update --init third_party/Vulkan-Headers third_party/volk "
                            "(or configure with -DSSX3_VULKAN=OFF)")
    endif()
    add_library(ssx3_volk STATIC ${SSX3_VOLK_DIR}/volk.c)
    target_include_directories(ssx3_volk PUBLIC ${SSX3_VOLK_DIR} ${SSX3_VK_HEADERS})
    target_compile_definitions(ssx3_volk PUBLIC VK_NO_PROTOTYPES)
    target_link_libraries(ssx3_volk PRIVATE ${CMAKE_DL_LIBS})

    # Shaders: GLSL -> SPIR-V as C initialiser lists, included by vk_backend.c.
    find_program(SSX3_GLSLC glslc REQUIRED)
    set(SSX3_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    set(SSX3_SHADER_INCS)
    foreach(sh nv2a.vert nv2a.frag)
        add_custom_command(
            OUTPUT  ${SSX3_SHADER_OUT}/${sh}.inc
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SSX3_SHADER_OUT}
            COMMAND ${SSX3_GLSLC} --target-env=vulkan1.3 -O -mfmt=c
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${sh}
                    -o ${SSX3_SHADER_OUT}/${sh}.inc
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${sh}
            COMMENT "glslc ${sh}")
        list(APPEND SSX3_SHADER_INCS ${SSX3_SHADER_OUT}/${sh}.inc)
    endforeach()
    add_custom_target(ssx3_shaders DEPENDS ${SSX3_SHADER_INCS})
    add_dependencies(${PROJECT_NAME} ssx3_shaders)

    find_package(SDL2 REQUIRED)
    target_sources(${PROJECT_NAME} PRIVATE src/render/vk_backend.c)
    target_include_directories(${PROJECT_NAME} PRIVATE ${SSX3_SHADER_OUT})
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${XBOXRECOMP_DIR}/src/kernel     # nv2a_backend.h
        ${XBOXRECOMP_DIR}/src/input)     # keyboard_sdl.h
    target_link_libraries(${PROJECT_NAME} PRIVATE ssx3_volk SDL2::SDL2)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SSX3_HAVE_VULKAN=1)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Guest memory is read and written through differently typed pointers
    # (MEM8/16/32, floats, structs) at the same addresses. MSVC never assumes
    # strict aliasing; GCC/Clang do at -O2, so turn it off to match.
    target_compile_options(${PROJECT_NAME} PRIVATE -fno-strict-aliasing)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
elseif(MINGW)
    # Same section limit for MinGW's assembler.
    target_compile_options(${PROJECT_NAME} PRIVATE -Wa,-mbig-obj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
