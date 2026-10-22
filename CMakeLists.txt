cmake_minimum_required(VERSION 3.20)
project(pyre VERSION 0.1.0 LANGUAGES CXX)

# pyre: the codec is header-only C++20 with the standard library only
# (include/pyre). The tool and the tests are built here.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PYRE_BUILD_TOOL "Build the pyre command-line tool" ON)
option(PYRE_BUILD_TESTS "Build the tests" ON)
option(PYRE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(PYRE_REFERENCE_DIR "" CACHE PATH "Folder of the reference heightmap.hpp (read only), for the comparison test and tool")
set(PYRE_REFERENCE_NAMESPACE "" CACHE STRING "The C++ namespace of the reference heightmap.hpp")

add_library(pyre INTERFACE)
target_include_directories(pyre INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pyre INTERFACE cxx_std_20)

set(PYRE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(PYRE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

set(PYRE_HAVE_REFERENCE OFF)
if(PYRE_REFERENCE_NAMESPACE AND EXISTS "${PYRE_REFERENCE_DIR}/heightmap.hpp")
  set(PYRE_HAVE_REFERENCE ON)
endif()

if(PYRE_BUILD_TOOL)
  find_package(ZLIB REQUIRED)
  # The tool's parts, shared by the tool and the tests.
  add_library(pyretool STATIC tools/pyre/grid.cpp tools/pyre/commands.cpp tools/pyre/compare.cpp tools/pyre/tiff.cpp
              tools/pyre/tiff_codec.cpp tools/pyre/tiff_write.cpp tools/pyre/resample.cpp tools/pyre/alt.cpp
              tools/pyre/tile_epsilon.cpp tools/pyre/source.cpp tools/pyre/pyre_source.cpp tools/pyre/overlay.cpp)
  target_include_directories(pyretool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools/pyre)
  target_link_libraries(pyretool PUBLIC pyre ZLIB::ZLIB)
  target_compile_options(pyretool PRIVATE ${PYRE_WARNINGS})
  find_package(Threads REQUIRED)
  target_link_libraries(pyretool PUBLIC Threads::Threads)

  add_executable(pyre-tool tools/pyre/main.cpp tools/pyre/bench.cpp)
  set_target_properties(pyre-tool PROPERTIES OUTPUT_NAME pyre)
  target_link_libraries(pyre-tool PRIVATE pyretool)
  target_compile_options(pyre-tool PRIVATE ${PYRE_WARNINGS})
  if(PYRE_HAVE_REFERENCE)
    # bench --reference: the reference codec on the same windows (read only).
    target_compile_definitions(pyre-tool PRIVATE PYRE_HAVE_REFERENCE=1 PYRE_REFERENCE_NS=${PYRE_REFERENCE_NAMESPACE})
    target_include_directories(pyre-tool SYSTEM PRIVATE ${PYRE_REFERENCE_DIR})
  endif()

  # pyre-resample: geographic sources onto a local metre grid round a centre point.
  add_executable(pyre-resample tools/pyre-resample/main.cpp)
  target_link_libraries(pyre-resample PRIVATE pyretool)
  target_compile_options(pyre-resample PRIVATE ${PYRE_WARNINGS})
endif()

if(PYRE_BUILD_TESTS)
  enable_testing()
  add_executable(test_codec tests/test_codec.cpp)
  target_link_libraries(test_codec PRIVATE pyre)
  target_compile_options(test_codec PRIVATE ${PYRE_WARNINGS})
  add_test(NAME codec COMMAND test_codec)

  add_executable(test_container tests/test_container.cpp)
  target_link_libraries(test_container PRIVATE pyre)
  target_compile_options(test_container PRIVATE ${PYRE_WARNINGS})
  target_compile_definitions(test_container PRIVATE PYRE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  add_test(NAME container COMMAND test_container)

  if(PYRE_BUILD_TOOL)
    add_executable(test_tiff tests/test_tiff.cpp)
    target_link_libraries(test_tiff PRIVATE pyretool)
    target_include_directories(test_tiff PRIVATE tests)
    target_compile_options(test_tiff PRIVATE ${PYRE_WARNINGS})
    add_test(NAME tiff COMMAND test_tiff)

    add_executable(test_resample tests/test_resample.cpp)
    target_link_libraries(test_resample PRIVATE pyretool)
    target_include_directories(test_resample PRIVATE tests)
    target_compile_options(test_resample PRIVATE ${PYRE_WARNINGS})
    add_test(NAME resample COMMAND test_resample)
  endif()

  if(PYRE_HAVE_REFERENCE)
    add_executable(test_reference tests/test_reference.cpp)
    target_link_libraries(test_reference PRIVATE pyre)
    target_include_directories(test_reference SYSTEM PRIVATE ${PYRE_REFERENCE_DIR})
    target_compile_definitions(test_reference PRIVATE PYRE_REFERENCE_NS=${PYRE_REFERENCE_NAMESPACE})
    add_test(NAME reference COMMAND test_reference)
  else()
    message(STATUS "pyre: no reference (set PYRE_REFERENCE_DIR and PYRE_REFERENCE_NAMESPACE); test_reference is not built")
  endif()
endif()
