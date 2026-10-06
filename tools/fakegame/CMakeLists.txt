cmake_minimum_required(VERSION 3.25)
project(fakegame CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fakegame main.cpp)
target_include_directories(fakegame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../protocol)
target_compile_definitions(fakegame PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
	target_compile_options(fakegame PRIVATE /W4 /permissive- /utf-8)
endif()
