cmake_minimum_required(VERSION 3.24)
project(deunify VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
	message(FATAL_ERROR "deunify targets Windows x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DEUNIFY_BUILD_TESTS "Build the GoogleTest suite" ON)

include(FetchContent)

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	zlib
	URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
	URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(zlib)

add_library(deunify_zlib INTERFACE)
target_include_directories(deunify_zlib SYSTEM INTERFACE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
target_link_libraries(deunify_zlib INTERFACE zlibstatic)

set(DEUNIFY_SOURCES
	src/cApi.cpp
	src/cardView.cpp
	src/census.cpp
	src/censusLedger.cpp
	src/codecs.cpp
	src/context.cpp
	src/d3d12Backend.cpp
	src/gpuBackend.cpp
	src/gpuPageTable.cpp
	src/hostError.cpp
	src/loadBook.cpp
	src/loads.cpp
	src/pages.cpp
	src/patch.cpp
	src/physicalMemory.cpp
	src/staging.cpp
	src/workerPool.cpp
)

set(DEUNIFY_HEADERS
	include/deunify.h
	include/deunify/cardView.hpp
	include/deunify/census.hpp
	include/deunify/context.hpp
	include/deunify/deunify.hpp
	include/deunify/gpuPageTable.hpp
	include/deunify/loads.hpp
	include/deunify/pages.hpp
	include/deunify/patch.hpp
	include/deunify/physicalMemory.hpp
	include/deunify/types.hpp
)

function(deunify_configure target)
	target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
	                                     PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
	target_link_libraries(${target} PRIVATE deunify_zlib PUBLIC d3d12 dxgi onecore)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
	endif()
endfunction()

add_library(deunify_static STATIC ${DEUNIFY_SOURCES} ${DEUNIFY_HEADERS})
deunify_configure(deunify_static)
target_compile_definitions(deunify_static PUBLIC DEUNIFY_STATIC)

add_library(deunify SHARED ${DEUNIFY_SOURCES} ${DEUNIFY_HEADERS})
deunify_configure(deunify)
target_compile_definitions(deunify PRIVATE DEUNIFY_BUILDING)

if(DEUNIFY_BUILD_TESTS)
	set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
	set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
	set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		googletest
		URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		EXCLUDE_FROM_ALL
		SYSTEM
	)
	FetchContent_MakeAvailable(googletest)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(gtest PRIVATE -Wno-unknown-warning-option -Wno-character-conversion)
		target_compile_options(gtest_main PRIVATE -Wno-unknown-warning-option -Wno-character-conversion)
	endif()

	add_executable(deunify_tests
		tests/cApiTest.cpp
		tests/cardViewTest.cpp
		tests/censusTest.cpp
		tests/endToEndTest.cpp
		tests/gpuPageTableTest.cpp
		tests/loadsTest.cpp
		tests/pagesTest.cpp
		tests/patchTest.cpp
		tests/physicalMemoryTest.cpp
	)
	target_compile_definitions(deunify_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
	target_include_directories(deunify_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_link_libraries(deunify_tests PRIVATE deunify_static deunify_zlib GTest::gtest_main)
	if(MSVC)
		target_compile_options(deunify_tests PRIVATE /W4 /permissive- /utf-8)
	endif()

	enable_testing()
	include(GoogleTest)
	gtest_discover_tests(deunify_tests DISCOVERY_MODE PRE_TEST)
endif()
