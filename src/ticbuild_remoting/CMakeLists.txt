cmake_minimum_required(VERSION 3.16)
project(ticbuild_remoting C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(ticbuild_remoting STATIC
    remoting.c
    discovery.c
    utils.c
    fps.c
)

target_include_directories(ticbuild_remoting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(WIN32)
    target_link_libraries(ticbuild_remoting PUBLIC ws2_32)
endif()

option(TICBUILD_REMOTING_BUILD_TESTS "Build remoting smoke-test harness" ON)

if(TICBUILD_REMOTING_BUILD_TESTS)
    add_executable(ticbuild_remoting_smoke smoke_test.c)
    target_link_libraries(ticbuild_remoting_smoke PRIVATE ticbuild_remoting)
    if(NOT WIN32)
        target_link_libraries(ticbuild_remoting_smoke PRIVATE m)
    endif()
endif()
