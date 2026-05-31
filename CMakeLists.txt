cmake_minimum_required(VERSION 3.28)
project(oramacore-cpp LANGUAGES CXX)

# Orama Platform — oramacore-cpp: the C++23 port of OramaCore's engine.
# First index type: BM25F full-text (see docs/bm25f-port-notes.md).
# Library-first (D16); C++23 today, C++26 target (D7); coroutines via Boost.Capy
# (D23) — async index/search ops are co_await-shaped. Lives under the data node.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Capy (the coroutine I/O foundation, D23) is added via FetchContent behind this
# option once we wire async search/index. The scaffold is Capy-free so it builds
# anywhere; the public API is designed to be coroutine-shaped (task<T>-returning).
option(ORAMACORE_CPP_CAPY "Build against Boost.Capy coroutine runtime (D23)" OFF)

# Library-first (D16). The BM25F scoring core is compiled; later subsystems
# (tokenization, posting lists, committed index, async search) join here.
add_library(oramacore-cpp STATIC
  src/bm25f.cpp
)
add_library(orama::oramacore-cpp ALIAS oramacore-cpp)
target_include_directories(oramacore-cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1)
  FetchContent_MakeAvailable(Catch2)
  add_executable(oramacore-cpp-tests tests/test_main.cpp tests/test_bm25f.cpp)
  target_link_libraries(oramacore-cpp-tests PRIVATE oramacore-cpp Catch2::Catch2WithMain)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(oramacore-cpp-tests)
endif()
