cmake_minimum_required(VERSION 3.25)

project(MaintenanceFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral maintenance eligibility and lifecycle runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Maintenance-Fabric"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MF_BUILD_APPS      "Build mfctl / mfd command line tools"          ON)
option(MF_BUILD_TESTS     "Build the validation suite"                    ON)
option(MF_BUILD_EXAMPLES  "Build examples"                                ON)
option(MF_BUILD_BENCH     "Build benchmarks"                              ON)
option(MF_WERROR          "Treat first-party warnings as errors"          ON)
option(MF_SANITIZE        "Enable Address/UB sanitizers where available"  OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warnings -- first-party warning count must be zero
# ---------------------------------------------------------------------------
add_library(mf_warnings INTERFACE)
add_library(MaintenanceFabric::mf_warnings ALIAS mf_warnings)
if(MSVC)
  target_compile_options(mf_warnings INTERFACE
    /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor /Zc:ternary /MP)
  target_compile_definitions(mf_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
  if(MF_WERROR)
    target_compile_options(mf_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(mf_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wcast-qual -Wpointer-arith -Wnon-virtual-dtor -Wold-style-cast
    -Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wnull-dereference
    -Wmisleading-indentation -Wduplicated-cond -Wlogical-op)
  if(MF_WERROR)
    target_compile_options(mf_warnings INTERFACE -Werror)
  endif()
endif()

if(MF_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(MF_CORE_SOURCES
  src/core/bytes.cpp
  src/core/hash.cpp
  src/core/id.cpp
  src/core/log.cpp
  src/core/result.cpp
  src/core/time.cpp
)

set(MF_DOMAIN_SOURCES
  src/domain/identity.cpp
  src/domain/target.cpp
  src/domain/topology.cpp
  src/domain/policy.cpp
  src/domain/evidence.cpp
  src/domain/window.cpp
  src/domain/drain.cpp
  src/domain/authority.cpp
  src/domain/job.cpp
  src/domain/lifecycle.cpp
)

set(MF_ENGINE_SOURCES
  src/engine/readiness.cpp
  src/engine/authority.cpp
  src/engine/impact.cpp
  src/engine/preconditions.cpp
  src/engine/arbitrator.cpp
  src/engine/scheduler.cpp
  src/engine/explain.cpp
)

set(MF_STORE_SOURCES
  src/store/records.cpp
  src/store/journal.cpp
  src/store/store.cpp
)

set(MF_RUNTIME_SOURCES
  src/runtime/fencing.cpp
  src/runtime/controller.cpp
)

set(MF_CONFIG_SOURCES
  src/config/text.cpp
)

set(MF_TRANSPORT_SOURCES
  src/transport/frame.cpp
  src/transport/socket.cpp
  src/transport/server.cpp
  src/transport/client.cpp
  src/transport/protocol.cpp
)

set(MF_DRAIN_SOURCES
  src/drain/local.cpp
  src/drain/remote.cpp
)

set(MF_SOURCES
  ${MF_CORE_SOURCES}
  ${MF_DOMAIN_SOURCES}
  ${MF_CONFIG_SOURCES}
  ${MF_ENGINE_SOURCES}
  ${MF_STORE_SOURCES}
  ${MF_RUNTIME_SOURCES}
  ${MF_TRANSPORT_SOURCES}
  ${MF_DRAIN_SOURCES}
)

set(MF_PUBLIC_HEADERS
  include/mf/version.hpp
  include/mf/core/bytes.hpp
  include/mf/core/checked.hpp
  include/mf/core/hash.hpp
  include/mf/core/id.hpp
  include/mf/core/limits.hpp
  include/mf/core/log.hpp
  include/mf/core/result.hpp
  include/mf/core/time.hpp
)

add_library(mf ${MF_SOURCES})
add_library(MaintenanceFabric::mf ALIAS mf)
target_include_directories(mf PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mf PRIVATE mf_warnings)
target_compile_features(mf PUBLIC cxx_std_20)
set_target_properties(mf PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME mf)
if(WIN32)
  target_link_libraries(mf PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Apps / tests / examples / benchmarks
# ---------------------------------------------------------------------------
if(MF_BUILD_APPS)
  add_subdirectory(apps)
endif()
if(MF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(MF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MF_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS mf EXPORT MaintenanceFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS mf_warnings EXPORT MaintenanceFabricTargets)

if(MF_BUILD_APPS)
  install(TARGETS mfctl mfd mf-drain-sim EXPORT MaintenanceFabricTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT MaintenanceFabricTargets
  FILE MaintenanceFabricTargets.cmake
  NAMESPACE MaintenanceFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MaintenanceFabric)

configure_package_config_file(
  cmake/MaintenanceFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/MaintenanceFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MaintenanceFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/MaintenanceFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/MaintenanceFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/MaintenanceFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MaintenanceFabric)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/MaintenanceFabric)

message(STATUS "MaintenanceFabric ${PROJECT_VERSION} | build=${CMAKE_BUILD_TYPE} tests=${MF_BUILD_TESTS} werror=${MF_WERROR}")
