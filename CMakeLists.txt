cmake_minimum_required(VERSION 3.25)
project(kirbyairriderecomp LANGUAGES C CXX VERSION 0.1)
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

# ---- Developer build: runtime + the game recompiled at build time (kirbyairride) -----------------
# Generate with: sa2recompile --dol orig/main.dol --rels orig/rel --names config/GKYE01/names.txt
#                --out build/gen      (nothing generated is ever committed)
set(SA2_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Recompiler output for the developer build")
file(GLOB SA2_GEN_SOURCES CONFIGURE_DEPENDS ${SA2_GEN_DIR}/*.cpp)
if (SA2_GEN_SOURCES)
    file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp)
    # SA2_EXTRA_SOURCES: work in progress kept out of the glob (docs/MORE_PLAYERS.md), added by a build tree's cache.
    set(SA2_EXTRA_SOURCES "" CACHE STRING "Extra runtime sources (not globbed)")
    add_executable(kirbyairride ${RUNTIME_SOURCES} ${SA2_GEN_SOURCES} ${SA2_EXTRA_SOURCES})
    target_include_directories(kirbyairride PRIVATE src ${SA2_GEN_DIR})
    # Generated code: one big function per guest function with gotos; no debug info (it is huge),
    # unwind tables so a guest thread can end by unwinding through it.
    set_source_files_properties(${SA2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS
        "/O2;/clang:-g0;/clang:-funwind-tables;-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable")
    target_compile_options(kirbyairride PRIVATE /EHsc)
    target_link_libraries(kirbyairride PRIVATE disc aurora::core aurora::gx aurora::vi aurora::pad aurora::mtx)
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
else ()
    message(STATUS "No recompiler output in ${SA2_GEN_DIR}; skipping the kirbyairride developer build")
endif ()

# ---- airnet_test: the online client (src/runtime/airnet.cpp) against a local master server -------
# Not part of the default build: `ninja airnet_test`. server/tests/test_cpp_client.py runs it against
# a server it starts itself (docs/ONLINE.md).
add_executable(airnet_test EXCLUDE_FROM_ALL
    server/tests/cpp/airnet_test.cpp src/runtime/airnet.cpp src/runtime/settings.cpp)
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
