cmake_minimum_required(VERSION 3.25)
project(Windfall VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

if(NOT WIN32)
  message(FATAL_ERROR "Windfall currently targets Windows x64 only")
endif()

add_subdirectory(runtime)
