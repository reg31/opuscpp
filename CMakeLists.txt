cmake_minimum_required(VERSION 3.20)
project(opuscpp LANGUAGES CXX)

add_library(opuscpp_source OBJECT src/opus_codec.cpp)
target_compile_features(opuscpp_source PUBLIC cxx_std_23)
target_include_directories(opuscpp_source PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(opuscpp_smoke tests/smoke_roundtrip.cpp $<TARGET_OBJECTS:opuscpp_source>)
target_compile_features(opuscpp_smoke PRIVATE cxx_std_23)
target_include_directories(opuscpp_smoke PRIVATE src)
