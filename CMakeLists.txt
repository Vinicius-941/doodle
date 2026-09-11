cmake_minimum_required(VERSION 3.20)
project(Doodle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8 /W3)
endif()

# Doo language: compiler + VM (no platform code; this is what gets ported in Phase 3)
add_library(doo STATIC doo-compiler/compiler.cpp vm-runtime/vm.cpp)
target_include_directories(doo PUBLIC doo-compiler vm-runtime)

# Simulator: virtual Doodle hardware on Windows. Reads firmware/ and games/ from the source tree,
# so editing a .doo only needs a relaunch from the menu, not a rebuild.
add_executable(doodle simulator/main.cpp)
target_link_libraries(doodle PRIVATE doo opengl32 winmm xinput)
target_compile_definitions(doodle PRIVATE DOODLE_ROOT="${CMAKE_SOURCE_DIR}" UNICODE _UNICODE NOMINMAX)

enable_testing()
add_executable(doo_test tests/doo_test.cpp)
target_link_libraries(doo_test PRIVATE doo)
add_test(NAME doo_test COMMAND doo_test)
add_test(NAME doo_check COMMAND doodle --check)
