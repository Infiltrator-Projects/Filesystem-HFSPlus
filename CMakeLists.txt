cmake_minimum_required(VERSION 3.20)
project(FilesystemHFSPlus VERSION 1.0.0 LANGUAGES C)
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
 GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"1.0.0\"\n")
add_library(hfsplus-driver SHARED
 native/hfsplus_native.c native/hfsplus_worker.c
 compat/legacy_services.c compat/runtime_alloc.c module/hfsplus_module.c)
set_source_files_properties(native/hfsplus_worker.c PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_hfsplus_worker_main")
set_target_properties(hfsplus-driver PROPERTIES
 PREFIX "" OUTPUT_NAME "HFSPLUS" SUFFIX ".so")
target_compile_definitions(hfsplus-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(hfsplus-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(hfsplus-driver PRIVATE -Wl,-z,defs)
target_include_directories(hfsplus-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(hfsplus-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
if(BUILD_TESTING)
 add_executable(hfsplus-module-test tests/test_module.c)
 target_include_directories(hfsplus-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
 target_link_libraries(hfsplus-module-test PRIVATE hfsplus-driver)
 add_test(NAME hfsplus-module-interface COMMAND hfsplus-module-test)
endif()
install(TARGETS hfsplus-driver LIBRARY DESTINATION /System/Filesystems)
