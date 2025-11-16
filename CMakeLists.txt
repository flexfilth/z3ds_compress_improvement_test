cmake_minimum_required(VERSION 3.20)
project(z3ds_compressor VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(Z3DS_USE_SYSTEM_ZSTD "Use a pkg-config provided zstd instead of the bundled one" ${UNIX})
option(Z3DS_ENABLE_GUI "Build the Windows GUI frontend" ON)

if(NOT WIN32)
    set(Z3DS_ENABLE_GUI OFF CACHE BOOL "" FORCE)
endif()

include(FetchContent)

if(Z3DS_USE_SYSTEM_ZSTD)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    set(Z3DS_ZSTD_TARGET PkgConfig::ZSTD)
else()
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        FIND_PACKAGE_ARGS NAMES zstd libzstd
    )
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    if(TARGET libzstd_static)
        set(Z3DS_ZSTD_TARGET libzstd_static)
    else()
        set(Z3DS_ZSTD_TARGET zstd)
    endif()
endif()

add_library(z3ds_core STATIC
    src/z3ds_compression.cpp
    src/frontend_common.cpp
)
target_link_libraries(z3ds_core PUBLIC ${Z3DS_ZSTD_TARGET})
target_include_directories(z3ds_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(Z3DS_USE_SYSTEM_ZSTD)
    target_include_directories(z3ds_core PUBLIC ${ZSTD_INCLUDE_DIRS})
    target_compile_options(z3ds_core PRIVATE ${ZSTD_CFLAGS_OTHER})
endif()

add_executable(z3ds_compressor src/cli_main.cpp)
target_link_libraries(z3ds_compressor PRIVATE z3ds_core)

if(MSVC)
    set(Z3DS_WARNING_FLAGS /permissive- /W4 /EHsc)
else()
    set(Z3DS_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

target_compile_options(z3ds_core PRIVATE ${Z3DS_WARNING_FLAGS})
target_compile_options(z3ds_compressor PRIVATE ${Z3DS_WARNING_FLAGS})

if(Z3DS_ENABLE_GUI)
    add_executable(z3ds_gui src/gui_main.cpp)
    target_link_libraries(z3ds_gui PRIVATE z3ds_core comctl32 shell32 ole32)
    target_compile_definitions(z3ds_gui PRIVATE UNICODE _UNICODE)
    target_compile_options(z3ds_gui PRIVATE ${Z3DS_WARNING_FLAGS})
endif()

include(GNUInstallDirs)
install(TARGETS z3ds_compressor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(Z3DS_ENABLE_GUI)
    install(TARGETS z3ds_gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
