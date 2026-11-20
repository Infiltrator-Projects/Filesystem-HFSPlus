# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemHFSPlus VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
  GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
  GIT_TAG d23a52693fd3212368d327e1c1302e07ae9510ec)
FetchContent_MakeAvailable(infiltratr_common)
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h"
  "#define LD_VERSION \"${PROJECT_VERSION}\"\n")

# Production ships just Common V2 and its real hosted on-disk geometry reader.
# Historical workers and V1 host callbacks are not part of the runtime ELF.
add_library(hfsplus-driver SHARED module/hfsplus_module.c native/hfsplus_catalog_v2.c)
set_target_properties(hfsplus-driver PROPERTIES
  PREFIX "" OUTPUT_NAME "HFSPLUS" SUFFIX ".so"
  C_VISIBILITY_PRESET hidden)
target_compile_features(hfsplus-driver PRIVATE c_std_11)
target_compile_options(hfsplus-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(hfsplus-driver PRIVATE
  -Wl,-z,defs
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
target_include_directories(hfsplus-driver PRIVATE
  "${infiltratr_common_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/native")
target_link_libraries(hfsplus-driver PRIVATE InfiltratrCommon::Common)

if(BUILD_TESTING)
  # Source retained and compiled for regression; never installed.
  add_library(hfsplus-historical-test-support SHARED
    native/hfsplus_native.c
    native/hfsplus_worker.c
    compat/legacy_services.c
    compat/runtime_alloc.c
    module/hfsplus_module.c
    native/hfsplus_catalog_v2.c
    fixtures/hfsplus_codec.c
    module/hfsplus_fixture_bridge.c)
  set_source_files_properties(native/hfsplus_worker.c PROPERTIES COMPILE_DEFINITIONS "main=ifs_hfsplus_worker_main")
  set_source_files_properties(fixtures/hfsplus_codec.c PROPERTIES COMPILE_DEFINITIONS "ldtm_raw_mac=ifs_hfsplus_raw_fixture")
  target_compile_definitions(hfsplus-historical-test-support PRIVATE
    _GNU_SOURCE _FILE_OFFSET_BITS=64)
  target_compile_options(hfsplus-historical-test-support PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(hfsplus-historical-test-support PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(hfsplus-historical-test-support PRIVATE
    InfiltratrCommon::Common OpenSSL::Crypto)

  add_executable(hfsplus-module-test tests/test_module.c)
  target_include_directories(hfsplus-module-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(hfsplus-module-test PRIVATE
    hfsplus-driver InfiltratrCommon::Common)
  add_test(NAME hfsplus-common-v2-probe-mount
    COMMAND hfsplus-module-test)
  add_test(NAME hfsplus-production-only-v2
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_v2_exports.sh"
      "$<TARGET_FILE:hfsplus-driver>")
endif()
install(TARGETS hfsplus-driver LIBRARY DESTINATION /System/Filesystems)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/HFSPlus.pdf"
  DESTINATION /System/Resources/Documentation
  RENAME Filesystem-HFSPlus.pdf)
