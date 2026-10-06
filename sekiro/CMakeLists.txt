cmake_minimum_required(VERSION 3.25)
project(sekicraft C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: self-contained DLLs

function(sekicraft_target name)
	target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
	if(MSVC)
		target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8)
	endif()
endfunction()

# What me3 loads. Small and stable: it only hot-reloads the core.
add_library(sekicraft SHARED src/loader.cpp)
sekicraft_target(sekicraft)

# MinHook (BSD-2, vendored in extern/minhook).
add_library(minhook STATIC
	extern/minhook/src/buffer.c extern/minhook/src/hook.c extern/minhook/src/trampoline.c extern/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC extern/minhook/include)
set_target_properties(minhook PROPERTIES LINKER_LANGUAGE C)

# Everything else. Rebuild it while the game runs and the loader swaps it in.
add_library(sekicraft_core SHARED src/core.cpp src/game.cpp src/input.cpp src/collision.cpp src/overlay.cpp)
sekicraft_target(sekicraft_core)
target_link_libraries(sekicraft_core PRIVATE minhook dinput8 dxguid d3d11 dxgi d3dcompiler)
