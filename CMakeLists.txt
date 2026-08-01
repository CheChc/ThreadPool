cmake_minimum_required(VERSION 3.16)
project(threadpool C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

set(TP_SOURCES threadpool.c threadpool.h)

add_executable(threadpool main.c ${TP_SOURCES})
target_link_libraries(threadpool PRIVATE Threads::Threads)
target_compile_options(threadpool PRIVATE -Wall -Wextra)

# 压力测试：cmake --build . && ctest（或直接运行 ./stress）
add_executable(stress stress.c ${TP_SOURCES})
target_link_libraries(stress PRIVATE Threads::Threads)
target_compile_options(stress PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME stress COMMAND stress)

# 内存/未定义行为消毒器：cmake -DTP_SANITIZE=ON .
option(TP_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(TP_SANITIZE)
    foreach(tgt threadpool stress)
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -g -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
