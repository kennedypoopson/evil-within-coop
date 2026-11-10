cmake_minimum_required(VERSION 3.20)
project(evil_with_them LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/..")

add_library(ewcoop SHARED src/mod/dllmain.cpp)
set_target_properties(ewcoop PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
target_link_libraries(ewcoop PRIVATE iphlpapi ws2_32 dinput8 dxguid)

add_executable(inject src/inject/inject.cpp)
set_target_properties(inject PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")

foreach(T ewcoop inject)
    target_include_directories(${T} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(${T} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        $<$<CONFIG:Debug>:_DEBUG> $<$<CONFIG:Release>:NDEBUG>)
    target_compile_options(${T} PRIVATE -Wall -Wextra
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Debug>:-O0 -g3 -gcodeview>)
    target_link_options(${T} PRIVATE -fuse-ld=lld -static $<$<CONFIG:Release>:-s>)
endforeach()
