cmake_minimum_required(VERSION 3.16)
project(onekvm_protocol_rtsp LANGUAGES CXX)

set(ONEKVM_EXTENSION_VERSION "0.0.0-dev" CACHE STRING "Extension version")
set(ONEKVM_EXTENSION_ARCH "unknown" CACHE STRING "Target architecture")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_C REQUIRED IMPORTED_TARGET json-c)

find_path(LIVE555_INCLUDE_DIR liveMedia.hh PATH_SUFFIXES liveMedia REQUIRED)
find_path(GROUPSOCK_INCLUDE_DIR Groupsock.hh PATH_SUFFIXES groupsock REQUIRED)
find_path(BASIC_USAGE_ENVIRONMENT_INCLUDE_DIR BasicUsageEnvironment.hh
          PATH_SUFFIXES BasicUsageEnvironment REQUIRED)
find_path(USAGE_ENVIRONMENT_INCLUDE_DIR UsageEnvironment.hh
          PATH_SUFFIXES UsageEnvironment REQUIRED)
find_library(LIVE_MEDIA_LIBRARY liveMedia REQUIRED)
find_library(GROUPSOCK_LIBRARY groupsock REQUIRED)
find_library(BASIC_USAGE_ENVIRONMENT_LIBRARY BasicUsageEnvironment REQUIRED)
find_library(USAGE_ENVIRONMENT_LIBRARY UsageEnvironment REQUIRED)

add_executable(onekvm-protocol-rtsp
  src/config.cpp
  src/main.cpp
  src/protocol_ipc.cpp
  src/rtsp_server.cpp
)
target_include_directories(onekvm-protocol-rtsp PRIVATE
  ${LIVE555_INCLUDE_DIR}
  ${GROUPSOCK_INCLUDE_DIR}
  ${BASIC_USAGE_ENVIRONMENT_INCLUDE_DIR}
  ${USAGE_ENVIRONMENT_INCLUDE_DIR}
)
target_compile_options(onekvm-protocol-rtsp PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(onekvm-protocol-rtsp PRIVATE
  PkgConfig::JSON_C
  ${LIVE_MEDIA_LIBRARY}
  ${GROUPSOCK_LIBRARY}
  ${BASIC_USAGE_ENVIRONMENT_LIBRARY}
  ${USAGE_ENVIRONMENT_LIBRARY}
  ssl
  crypto
  Threads::Threads
)

set(VERSION "${ONEKVM_EXTENSION_VERSION}")
set(ARCH "${ONEKVM_EXTENSION_ARCH}")
configure_file(manifest.json.in manifest.json @ONLY)

install(TARGETS onekvm-protocol-rtsp RUNTIME DESTINATION bin)
install(FILES web/page.json DESTINATION web)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" DESTINATION .)

option(ONEKVM_BUILD_TESTS "Build OneKVM RTSP unit tests" OFF)
if(ONEKVM_BUILD_TESTS)
  enable_testing()
  add_executable(socket-activation-test tests/socket_activation_test.cpp)
  target_include_directories(socket-activation-test PRIVATE src)
  target_compile_options(socket-activation-test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME socket-activation-test COMMAND socket-activation-test)
  add_executable(protocol-test tests/protocol_test.cpp src/protocol_ipc.cpp)
  target_include_directories(protocol-test PRIVATE src)
  target_compile_options(protocol-test PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(protocol-test PRIVATE Threads::Threads)
  add_test(NAME protocol-test COMMAND protocol-test)
endif()
