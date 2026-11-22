# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemHFSPlus VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
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

# This ELF uses the identical source-owned reader as the hosted module.
set(HFSPLUS_FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware")
set(HFSPLUS_FIRMWARE_ELF "${HFSPLUS_FIRMWARE_DIR}/HFSPLUS.so")
add_custom_command(OUTPUT "${HFSPLUS_FIRMWARE_ELF}"
  COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
    -B "${HFSPLUS_FIRMWARE_DIR}" -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
  COMMAND "${CMAKE_COMMAND}" --build "${HFSPLUS_FIRMWARE_DIR}" --parallel 2
  DEPENDS module/firmware/CMakeLists.txt module/firmware/runtime.c
    module/hfsplus_module.c native/hfsplus_catalog_v2.c
    native/hfsplus_catalog_v2.h
    "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
  VERBATIM)
add_custom_target(hfsplus-firmware-elf DEPENDS "${HFSPLUS_FIRMWARE_ELF}")
if(NOT CMAKE_OBJCOPY)
  find_program(CMAKE_OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
endif()
add_dependencies(hfsplus-driver hfsplus-firmware-elf)
set_property(TARGET hfsplus-driver APPEND PROPERTY LINK_DEPENDS "${HFSPLUS_FIRMWARE_ELF}")
add_custom_command(TARGET hfsplus-driver POST_BUILD
  COMMAND "${CMAKE_OBJCOPY}" --add-section
    ".infiltrator_boot=${HFSPLUS_FIRMWARE_ELF}"
    --set-section-flags .infiltrator_boot=readonly
    "$<TARGET_FILE:hfsplus-driver>"
  VERBATIM)

if(BUILD_TESTING)
  add_executable(hfsplus-real-media-test tests/test_real_media.c)
  target_include_directories(hfsplus-real-media-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include")
  target_compile_options(hfsplus-real-media-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(hfsplus-real-media-test PRIVATE
    hfsplus-driver InfiltratrCommon::Common)
  option(HFSPLUS_REAL_MEDIA_TESTS
    "Require independently created and populated HFS+ media in CTest" OFF)
  if(HFSPLUS_REAL_MEDIA_TESTS)
    find_program(HFSPLUS_MKFS mkfs.hfsplus REQUIRED)
    find_program(HFSPLUS_MOUNT hpmount REQUIRED)
    find_program(HFSPLUS_COPY hpcopy REQUIRED)
    find_program(HFSPLUS_UNMOUNT hpumount REQUIRED)
    add_test(NAME hfsplus-v2-independent-real-media
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/real-hfsplus-v2.sh"
      "$<TARGET_FILE:hfsplus-real-media-test>")
    set_tests_properties(hfsplus-v2-independent-real-media
      PROPERTIES TIMEOUT 120)
  endif()
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
  # The regression compiles the same native reader used in both V2 profiles.
  add_executable(hfsplus-duplicate-catalogue-test
    tests/test_duplicate_catalogue.c)
  target_include_directories(hfsplus-duplicate-catalogue-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(hfsplus-duplicate-catalogue-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME hfsplus-v2-duplicate-catalogue-corruption
    COMMAND hfsplus-duplicate-catalogue-test)
endif()
install(TARGETS hfsplus-driver LIBRARY DESTINATION /System/Filesystems)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/HFSPlus.pdf"
  DESTINATION /System/Resources/Documentation
  RENAME Filesystem-HFSPlus.pdf)
