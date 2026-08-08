cmake_minimum_required(VERSION 3.20)

project(TurboBuild
  VERSION 0.1.0
  DESCRIPTION "Measured C and C++ build optimization tool"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(turbobuild
  src/main.cpp
  src/turbobuild.cpp
  src/turbobuild.h)

target_compile_features(turbobuild PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(turbobuild PRIVATE /W4 /permissive-)
else()
  target_compile_options(turbobuild PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS turbobuild RUNTIME DESTINATION bin)
