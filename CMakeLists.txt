cmake_minimum_required(VERSION 3.26)
project(threadpool C)

set(CMAKE_C_STANDARD 11)

include_directories(.)

add_executable(threadpool
        main.c
        threadpool.c
        threadpool.h)
