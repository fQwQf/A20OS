cmake_minimum_required(VERSION 3.12 FATAL_ERROR)

project(tlse VERSION 0.1.0 LANGUAGES C)

option(BUILD_EXAMPLES "Enable building examples")

message("Building with CMake version: ${CMAKE_VERSION}")

add_library(tlse STATIC
    tlse.c
    tlse.h
    libtomcrypt.c
)
add_library(tlse::tlse ALIAS tlse)
target_include_directories(tlse PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(tlse PUBLIC ${TLSE_COMPILE_DEFINITIONS})

if (BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()
