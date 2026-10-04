cmake_minimum_required(VERSION 3.16)
project(pls_runtime CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Note what is NOT here: no find_package(Vulkan), no GLFW, no shader
# compilation, no PhysX. A game server has no renderer, and requiring a
# graphics stack to build one would make it impossible to build in the
# place servers actually run -- a headless container with no GPU, no
# display, and no Vulkan loader. The only dependencies are the platform's
# sockets and a thread library.

if(MSVC)
  # /utf-8         -- source and execution charset; without it MSVC reads
  #                   this codebase's UTF-8 string literals in the system
  #                   code page.
  # /permissive-   -- standards conformance.
  # /Zc:__cplusplus -- otherwise __cplusplus reports 199711L even in C++17.
  # /MP            -- parallel compilation, which MSVC doesn't do by default.
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /MP)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(pls_runtime STATIC
  src/pls_value.cpp
  src/pls_json.cpp
  src/builtins.cpp
  src/net.cpp
  src/crypto.cpp
  third_party/monocypher.c
  src/server.cpp
  src/world.cpp
  src/physics.cpp
  src/broadcast.cpp
  src/debug.cpp
)

target_include_directories(pls_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(pls_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Monocypher is C99; keep it as C so MSVC does not compile it as C++.
set_source_files_properties(third_party/monocypher.c PROPERTIES LANGUAGE C)

# Project settings from plsproject.json, injected by the generated
# CMakeLists.txt (see plscc.rs) as plain variables before add_subdirectory().
#
# These have to be defined on *this* target, not just on the game executable:
# the code that reads them (Server's field initializers in pls_server.h) is
# compiled into pls_runtime. They were previously applied only to the game
# target, so every one of them was silently ignored -- a project that set
# max_players or tickrate got the hardcoded defaults instead, with no warning.
# PUBLIC so the generated main.cpp sees the same values the runtime does.
if(NOT DEFINED PLS_DEFAULT_TICKRATE)
  set(PLS_DEFAULT_TICKRATE 60)
endif()
if(NOT DEFINED PLS_MAX_PLAYERS)
  set(PLS_MAX_PLAYERS 64)
endif()
target_compile_definitions(pls_runtime PUBLIC
  PLS_DEFAULT_TICKRATE=${PLS_DEFAULT_TICKRATE}
  PLS_MAX_PLAYERS=${PLS_MAX_PLAYERS})

find_package(Threads REQUIRED)
target_link_libraries(pls_runtime PUBLIC Threads::Threads)

if(WIN32)
  # ws2_32: the Winsock 2 implementation behind src/server.cpp.
  target_link_libraries(pls_runtime PUBLIC ws2_32)
  target_compile_definitions(pls_runtime PUBLIC
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 12/13 emit a false -Wmaybe-uninitialized from inside std::variant's
  # move path whenever a Value is constructed directly into a container
  # (e.g. `out.arrayRef().push_back(Value(id))`). The warning names the
  # std::string alternative of the variant, which is provably not the
  # engaged alternative there -- it is a known GCC issue with
  # variant + string, not a real uninitialized read.
  #
  # Scoped to this target rather than added globally, and left as the ONLY
  # suppression in the build, so it cannot quietly hide a genuine warning in
  # a project that links this library. Everything else still builds under
  # -Wall -Wextra with no output.
  target_compile_options(pls_runtime PRIVATE -Wno-maybe-uninitialized)
endif()
