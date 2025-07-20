cmake_minimum_required(VERSION 3.20)

project(vcr C)


set(SOURCES src/vcr.c src/vcr.h)


if(APPLE)
    list(APPEND SOURCES
            src/osx/osxentry.m
            src/osx/sokol_gfx.m
    )
endif()

add_executable(vcr ${SOURCES})

target_include_directories(vcr PUBLIC
        thirdparty/json.h
        thirdparty/sokol
        thirdparty/sokol/util
        thirdparty/Nuklear
)

target_compile_options(vcr PUBLIC -std=c23)

if(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(METAL_FRAMEWORK Metal)
    find_library(METALKIT_FRAMEWORK MetalKit)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore)

    target_link_libraries(vcr
            ${COCOA_FRAMEWORK}
            ${METAL_FRAMEWORK}
            ${METALKIT_FRAMEWORK}
            ${QUARTZCORE_FRAMEWORK}
    )
endif()