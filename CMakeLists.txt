cmake_minimum_required(VERSION 3.22)

option(QUEST "Build for quest" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_compile_options(-frtti -fexceptions -fvisibility=hidden -fPIE -fPIC)

# QPM defines - sets up NDK, RECURSE_FILES macro, EXTERN_DIR, SHARED_DIR
include(${CMAKE_CURRENT_SOURCE_DIR}/qpm_defines.cmake)

if(${CMAKE_BUILD_TYPE} STREQUAL "RELEASE" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo" OR ${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")
	add_compile_options(-O3)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
	add_compile_options(-flto)
endif()

if(${CMAKE_BUILD_TYPE} STREQUAL "DEBUG" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo")
	add_compile_options(-g)
endif()

# project() only takes a numeric version; a test suffix such as "-naomi.1"
# stays in MOD_VERSION, which is what the mod reports.
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" MOD_VERSION_NUMERIC "${MOD_VERSION}")
project(Vivify
	VERSION ${MOD_VERSION_NUMERIC})

# The commit this build came from, so builds can be told apart in
# VivifySession.txt. "-dirty" marks uncommitted changes. Re-read on every
# configure, which scripts/build.ps1 runs on every build.
execute_process(
	COMMAND git rev-parse --short=10 HEAD
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	OUTPUT_VARIABLE VIVIFY_GIT_COMMIT
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(NOT VIVIFY_GIT_COMMIT)
	set(VIVIFY_GIT_COMMIT "unknown")
else()
	execute_process(
		COMMAND git status --porcelain --untracked-files=no
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE VIVIFY_GIT_DIRTY
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET)
	if(VIVIFY_GIT_DIRTY)
		set(VIVIFY_GIT_COMMIT "${VIVIFY_GIT_COMMIT}-dirty")
	endif()
endif()

add_compile_definitions(MOD_ID="${CMAKE_PROJECT_NAME}")
add_compile_definitions(VERSION="${MOD_VERSION}")
add_compile_definitions(VIVIFY_GIT_COMMIT="${VIVIFY_GIT_COMMIT}")
# Linked into the library by src/VivifyAudioLinkBundle.cpp (.incbin).
add_compile_definitions(VIVIFY_AUDIOLINK_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/assets/audiolink/AudioLink.bundle")

set(COMPILE_ID ${CMAKE_PROJECT_NAME})

# Recursively get all src files
RECURSE_FILES(cpp_file_list ${SOURCE_DIR}/*.cpp)
RECURSE_FILES(c_file_list ${SOURCE_DIR}/*.c)

add_library(
	${CMAKE_PROJECT_NAME}
	SHARED
	${cpp_file_list}
	${c_file_list}
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE -llog -lz)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${INCLUDE_DIR})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SHARED_DIR})

# beatsaber hook inline hook
RECURSE_FILES(src_inline_hook_beatsaber_hook_local_extra_c ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.c)
RECURSE_FILES(src_inline_hook_beatsaber_hook_local_extra_cpp ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.cpp)

target_sources(${COMPILE_ID} PRIVATE ${src_inline_hook_beatsaber_hook_local_extra_c})
target_sources(${COMPILE_ID} PRIVATE ${src_inline_hook_beatsaber_hook_local_extra_cpp})

# Strip debug symbols in release
if(${CMAKE_BUILD_TYPE} STREQUAL "RELEASE")
	add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_STRIP} -g -S -d --strip-debug
		"${CMAKE_CURRENT_BINARY_DIR}/lib${CMAKE_PROJECT_NAME}.so"
	)
endif()

# QPM extern includes and libs
include(${CMAKE_CURRENT_SOURCE_DIR}/extern.cmake)
