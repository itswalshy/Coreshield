cmake_minimum_required(VERSION 3.20)
project(VramHeadroomBell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(headroom_logic src/headroom_logic.cpp)
target_include_directories(headroom_logic PUBLIC include)

add_executable(vram-headroom-bell src/main.cpp)
target_link_libraries(vram-headroom-bell PRIVATE headroom_logic)

if(WIN32)
  target_compile_definitions(vram-headroom-bell PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(vram-headroom-bell PRIVATE dxgi)
else()
  message(STATUS "The collector is Windows-only; this build provides its explanatory stub")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(headroom_logic_test tests/headroom_logic_test.cpp)
  target_link_libraries(headroom_logic_test PRIVATE headroom_logic)
  add_test(NAME headroom_logic_test COMMAND headroom_logic_test)
endif()
