# Liberator Variants as a CMake project (docs/modding/tools.md): open this folder in Rider, CLion or VS Code, or build it with
#   cmake --preset default && cmake --build --preset default
cmake_minimum_required(VERSION 3.25)
project(liberator_variants C)
include(../../../native/mod-sdk/cmake/HellForgeMod.cmake)

hellforge_add_mod(liberator_variants src/liberator_variants.c)
