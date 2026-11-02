# Configure this directory directly or through the firmware host suite (test/CMakeLists.txt).
# cmake -S lib/CrossXSync/test -B build/crossx && cmake --build build/crossx && ctest --test-dir build/crossx
cmake_minimum_required(VERSION 3.16)
project(crossx_sync_tests CXX)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

include(FetchContent)
if(NOT TARGET GTest::gtest_main)
  FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

get_filename_component(CROSSX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(crossx_sync_core STATIC
  ${CROSSX_ROOT}/src/ProtocolV1.cpp
  ${CROSSX_ROOT}/src/SyncCoordinator.cpp
  ${CROSSX_ROOT}/src/LibraryCommit.cpp
)
target_include_directories(crossx_sync_core PUBLIC ${CROSSX_ROOT}/src)
target_compile_options(crossx_sync_core PUBLIC -Wall -Wextra -Werror)

add_executable(CrossXSyncTest
  ProtocolV1Test.cpp
  SyncCoordinatorTest.cpp
  LibraryCommitTest.cpp
  SyncPowerPolicyTest.cpp
  PageTurnerMirrorTest.cpp
)
# PageTurnerMirrorTest reads the page turner's radio thresholds, which the heap gate mirrors. Those
# headers are pure C++ too, but only the test sees them: crossx_sync_core stays independent.
target_include_directories(CrossXSyncTest PRIVATE
  ${CROSSX_ROOT}/../BlePageTurner/include
  ${CROSSX_ROOT}/../BlePageTurner/src
  ${CROSSX_ROOT}/../../freeink-sdk/libs/network/BleKeyboardHost/include
)
target_link_libraries(CrossXSyncTest PRIVATE crossx_sync_core GTest::gtest_main)
gtest_discover_tests(CrossXSyncTest)
