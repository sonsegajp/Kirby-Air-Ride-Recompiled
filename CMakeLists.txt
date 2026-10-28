cmake_minimum_required(VERSION 3.25)
project(kirbyairriderecomp LANGUAGES C CXX VERSION 1.3.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The recompiler: main.dol + REL modules -> C++ (docs/PLAN.md). Linked into the developer CLI and,
# later, into the shipped program for first-run setup.
add_library(sa2_recompiler STATIC
    src/recompiler/image.cpp
    src/recompiler/analyze.cpp
    src/recompiler/emit.cpp
    src/recompiler/overrides.cpp)
target_include_directories(sa2_recompiler PUBLIC src)

add_executable(sa2recompile tools/recomp/sa2recompile.cpp)
target_link_libraries(sa2recompile PRIVATE sa2_recompiler)

# ---- Aurora: GX renderer (WebGPU/Dawn), window, controllers (extern/aurora, MIT) ---------------
# The disc is read by our own DVD layer and saves will go through our CARD layer, so Aurora's DVD
# (nod) and CARD components are off.
# Local changes to Aurora are kept as patches (patches/aurora) so the submodule stays pristine.
# The series is applied to a pristine checkout only; a checkout with changes is never touched (it
# is the patched tree, perhaps with work not yet saved as a patch - a reset here once wiped that).
file(GLOB AURORA_PATCHES ${CMAKE_SOURCE_DIR}/patches/aurora/*.patch)
list(SORT AURORA_PATCHES)
set(_aurora ${CMAKE_SOURCE_DIR}/extern/aurora)
execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${_aurora} RESULT_VARIABLE _dirty)
if (_dirty EQUAL 0)
    # Git for Windows' core.autocrlf=true checks the submodule out with CRLF and LF patches then
    # fail: switch it off and rewrite the (pristine) files with LF first.
    execute_process(COMMAND git config core.autocrlf false WORKING_DIRECTORY ${_aurora})
    execute_process(COMMAND git rm -r -q --cached . WORKING_DIRECTORY ${_aurora} OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND git reset -q --hard WORKING_DIRECTORY ${_aurora})
    execute_process(COMMAND git checkout-index -f -a WORKING_DIRECTORY ${_aurora})
    foreach (_patch ${AURORA_PATCHES})
        execute_process(COMMAND git apply ${_patch} WORKING_DIRECTORY ${_aurora} RESULT_VARIABLE _result)
        if (NOT _result EQUAL 0)
            message(FATAL_ERROR "Failed to apply Aurora patch ${_patch}")
        endif ()
        message(STATUS "Applied Aurora patch ${_patch}")
    endforeach ()
else ()
    message(STATUS "Aurora has local changes: taken as the patched tree (patches/aurora), left as is")
endif ()
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# ---- The official online service's address: kept out of the source. Set KAR_ONLINE_MASTER ("host:port") when
# configuring, or put it in online_master.txt beside this file (not committed). Without it the game offers dedicated
# servers only (Online Settings; docs/ONLINE.md). Only settings.cpp sees it, so changing it rebuilds that alone.
set(KAR_ONLINE_MASTER "" CACHE STRING "The official online service (host:port); empty: dedicated servers only")
set(_kar_master "${KAR_ONLINE_MASTER}")
if (NOT _kar_master AND EXISTS "${CMAKE_SOURCE_DIR}/online_master.txt")
    file(STRINGS "${CMAKE_SOURCE_DIR}/online_master.txt" _kar_master LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/online_master.txt")
endif ()
string(STRIP "${_kar_master}" _kar_master)
set_source_files_properties(src/runtime/settings.cpp PROPERTIES COMPILE_DEFINITIONS "KAR_ONLINE_MASTER=\"${_kar_master}\"")

# ---- Disc images (.iso/.gcm, .ciso, .rvz/.wia), read by the game and the launcher ---------------
add_library(disc STATIC src/disc/discimage.cpp)
target_include_directories(disc PUBLIC src)
target_link_libraries(disc PRIVATE zstd::libzstd)

# ---- The program (kirbyairride): the runtime. The game's code is not in it: it loads kirbyairride_game.dll,
# which first-run setup builds from the player's own disc (src/setup; never shipped). It exports what that code
# calls (RT_API in src/runtime/ctx.h: kirbyairride.lib) and forwards the C runtime's _setjmp to it.
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp)
# SA2_EXTRA_SOURCES: work in progress kept out of the glob (docs/MORE_PLAYERS.md), added by a build tree's cache.
set(SA2_EXTRA_SOURCES "" CACHE STRING "Extra runtime sources (not globbed)")
# What first-run setup compiles the module with (src/runtime/setup.cpp) is embedded in the program.
set(_embed_out ${CMAKE_BINARY_DIR}/generated/setup_files.inc)
add_custom_command(OUTPUT ${_embed_out}
    COMMAND ${CMAKE_COMMAND} -DOUT=${_embed_out} -DROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tools/embed.cmake
    DEPENDS tools/embed.cmake src/runtime/ctx.h src/runtime/modules.h src/runtime/abi.h src/game/glue.cpp
            config/GKYE01/names.txt config/GKYE01/patches_8p.txt
    COMMENT "Embedding the game module's sources in the program")
# What the program is, to Windows: version information, its manifest and icon (res/kirbyairride.rc.in).
set(_kar_rc)
if (WIN32)
    enable_language(RC)
    set(KAR_MANIFEST "${CMAKE_SOURCE_DIR}/res/kirbyairride.manifest")
    set(KAR_ICON "${CMAKE_SOURCE_DIR}/res/kirbyairride.ico")
    string(REPLACE "/" "\\\\" KAR_MANIFEST "${KAR_MANIFEST}")
    string(REPLACE "/" "\\\\" KAR_ICON "${KAR_ICON}")
    configure_file(res/kirbyairride.rc.in ${CMAKE_BINARY_DIR}/generated/kirbyairride.rc @ONLY)
    set(_kar_rc ${CMAKE_BINARY_DIR}/generated/kirbyairride.rc)
endif ()
add_executable(kirbyairride ${RUNTIME_SOURCES} ${SA2_EXTRA_SOURCES} ${_embed_out} ${_kar_rc})
if (WIN32)
    target_link_options(kirbyairride PRIVATE /MANIFEST:NO)  # (its own, from the resources)
endif ()
target_include_directories(kirbyairride PRIVATE src ${CMAKE_BINARY_DIR}/generated)
set_target_properties(kirbyairride PROPERTIES ENABLE_EXPORTS ON)
target_compile_options(kirbyairride PRIVATE /EHsc)
target_link_options(kirbyairride PRIVATE "/EXPORT:_setjmp=VCRUNTIME140.__intrinsic_setjmp")
target_link_libraries(kirbyairride PRIVATE disc sa2_recompiler aurora::core aurora::gx aurora::vi aurora::pad aurora::mtx)
# (the version a module built by setup is stamped with: a new version builds it again)
set_property(SOURCE src/runtime/setup.cpp APPEND PROPERTY COMPILE_DEFINITIONS "KAR_VERSION=\"${PROJECT_VERSION}\"")
# The GX bridge feeds Aurora's command FIFO directly (internal headers).
target_include_directories(kirbyairride PRIVATE ${CMAKE_SOURCE_DIR}/extern/aurora/lib)
if (WIN32)
    add_custom_command(TARGET kirbyairride POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:kirbyairride> $<TARGET_FILE_DIR:kirbyairride>
        COMMAND_EXPAND_LISTS)
    # Dawn's D3D12 backend loads the shader compiler DLLs at run time.
    file(GLOB _dawn_dx_dlls "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/dx*.dll")
    if (_dawn_dx_dlls)
        add_custom_command(TARGET kirbyairride POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dawn_dx_dlls} $<TARGET_FILE_DIR:kirbyairride>)
    endif ()
endif ()

# ---- Developer build of the game module (kirbyairride_game.dll) from the recompiler's output -------------
# Generate with: sa2recompile --dol orig/main.dol --rels orig/rel --names config/GKYE01/names.txt
#                --out build/gen      (nothing generated is ever committed; the 8-rider build: tools/build8p.ps1)
set(SA2_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Recompiler output for the developer build")
option(KAR_GAME_8P "The developer build's module is the 8-rider build's (kirbyairride_game_8p.dll)" OFF)
file(GLOB SA2_GEN_SOURCES CONFIGURE_DEPENDS ${SA2_GEN_DIR}/*.cpp)
if (SA2_GEN_SOURCES)
    add_library(kirbyairride_game SHARED ${SA2_GEN_SOURCES} src/game/glue.cpp)
    if (KAR_GAME_8P)
        set_target_properties(kirbyairride_game PROPERTIES OUTPUT_NAME kirbyairride_game_8p)
    endif ()
    target_include_directories(kirbyairride_game PRIVATE src ${SA2_GEN_DIR})
    target_compile_definitions(kirbyairride_game PRIVATE KAR_GAME_DLL)
    # Generated code: one big function per guest function with gotos; no debug info (it is huge), unwind tables
    # so a guest thread can end by unwinding through it; no C library (as setup builds it on the player's PC).
    target_compile_options(kirbyairride_game PRIVATE /O2 /clang:-g0 /clang:-ffreestanding /clang:-funwind-tables /GS- /Zl /GR- /EHs-c-
        -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
    target_link_options(kirbyairride_game PRIVATE /NOENTRY /NODEFAULTLIB)
    target_link_libraries(kirbyairride_game PRIVATE kirbyairride)
    set_target_properties(kirbyairride_game PROPERTIES MSVC_RUNTIME_LIBRARY "")
else ()
    message(STATUS "No recompiler output in ${SA2_GEN_DIR}; skipping the developer build of kirbyairride_game.dll")
endif ()

# ---- airnet_test: the online client (src/runtime/airnet.cpp) against a local master server -------
# Not part of the default build: `ninja airnet_test`. server/tests/test_cpp_client.py runs it against
# a server it starts itself (docs/ONLINE.md).
add_executable(airnet_test EXCLUDE_FROM_ALL
    server/tests/cpp/airnet_test.cpp src/runtime/airnet.cpp src/runtime/mapfiles.cpp src/runtime/settings.cpp)
target_include_directories(airnet_test PRIVATE src)
if (MSVC)
    target_compile_options(airnet_test PRIVATE /EHsc)
endif ()
if (WIN32)
    target_link_libraries(airnet_test PRIVATE ws2_32 bcrypt)
else ()
    find_package(Threads REQUIRED)
    target_link_libraries(airnet_test PRIVATE Threads::Threads)
endif ()
