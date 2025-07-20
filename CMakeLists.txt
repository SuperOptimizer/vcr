cmake_minimum_required(VERSION 3.20)

project(vcr C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0)
    add_compile_definitions(DEBUG)
else ()
    add_compile_options(-O2)
    add_compile_definitions(NDEBUG)
endif ()

add_compile_options(-g3 -Wall -Wextra)

set(SOURCES src/vcr.c src/vcr.h src/zarr.c src/util.c)
set(LIBRARIES -lm )

if(APPLE)
    list(APPEND SOURCES src/osx/osxentry.m src/osx/sokol_gfx.m)
endif()

add_executable(vcr ${SOURCES})

target_include_directories(vcr PUBLIC thirdparty/json.h thirdparty/sokol thirdparty/sokol/util thirdparty/Nuklear)

target_compile_options(vcr PUBLIC -std=c23)

if(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(METAL_FRAMEWORK Metal)
    find_library(METALKIT_FRAMEWORK MetalKit)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore)

    list(APPEND LIBRARIES ${COCOA_FRAMEWORK} ${METAL_FRAMEWORK} ${METALKIT_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
endif()

find_package(Blosc2 REQUIRED)

if(Blosc2_FOUND)
    list(APPEND LIBRARIES Blosc2::Blosc2)
else()
    message(FATAL_ERROR "Blosc2 not found, please install blosc2")
endif()

target_link_libraries(vcr PUBLIC ${LIBRARIES})
