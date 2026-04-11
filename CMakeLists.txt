cmake_minimum_required(VERSION 3.10)
project(gpencl)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set output directories for multi-config generators (Visual Studio)
set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "Configuration types" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Platform-specific debug configuration
if(WIN32)
    # Use Release runtime library to match prebuilt shaderc_combined.lib
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /W4 /Zi /Od /MD")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /W4 /Zi /Od /MD")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /W4 /O2 /MD")
    
    # Set debug output directories
    foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE})
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
    endforeach()
else()
    # Debug configuration for Unix/Linux
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -Wall -Wextra -O0")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wall -Wextra -O3")
endif()

# Default to Debug if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    message(STATUS "No build type specified, defaulting to Debug")
endif()

# Print current build type
if(CMAKE_BUILD_TYPE)
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()

# -----------------------------------------------------------------------------
# Find Vulkan
find_package(Vulkan REQUIRED)

# -----------------------------------------------------------------------------
# Find shaderc (typically inside Vulkan SDK)
if(DEFINED ENV{VULKAN_SDK})
    set(VULKAN_SDK_PATH $ENV{VULKAN_SDK})
    message(STATUS "Vulkan SDK found at ${VULKAN_SDK_PATH}")

    # shaderc include directory
    set(SHADERC_INCLUDE_DIR ${VULKAN_SDK_PATH}/include)
    
    # Determine correct library name depending on OS
    if(WIN32)
        set(SHADERC_LIB_NAMES shaderc_combined shaderc)
    else()
        set(SHADERC_LIB_NAMES shaderc_combined shaderc libshaderc)
    endif()
    
    find_library(SHADERC_LIB
        NAMES ${SHADERC_LIB_NAMES}
        PATHS 
            ${VULKAN_SDK_PATH}/lib
            ${VULKAN_SDK_PATH}/lib/x86_64-linux-gnu
            ${VULKAN_SDK_PATH}/Lib
        NO_DEFAULT_PATH
    )
else()
    message(FATAL_ERROR "Vulkan SDK not found. Please set VULKAN_SDK environment variable.")
endif()

if(NOT SHADERC_LIB)
    message(FATAL_ERROR "Could not find shaderc library. Ensure Vulkan SDK is installed and the library exists.")
endif()

message(STATUS "shaderc include: ${SHADERC_INCLUDE_DIR}")
message(STATUS "shaderc library: ${SHADERC_LIB}")

# -----------------------------------------------------------------------------
# Add library
add_library(gpencl STATIC libgpencl.cpp)

target_include_directories(gpencl PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${SHADERC_INCLUDE_DIR}
)

target_link_libraries(gpencl
    ${Vulkan_LIBRARIES}
    ${SHADERC_LIB}
)

# Enable debug info in all builds
if(WIN32)
    target_compile_options(gpencl PRIVATE /Zi)
    target_link_options(gpencl PRIVATE /DEBUG:FULL)
else()
    target_compile_options(gpencl PRIVATE -g)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(gpencl pthread dl)
endif()

if(WIN32)
    target_compile_definitions(gpencl PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

# -----------------------------------------------------------------------------
# Add executable
add_executable(gpencl_exe main.cpp)

target_include_directories(gpencl_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gpencl_exe gpencl)

# Enable debug info in all builds
if(WIN32)
    target_compile_options(gpencl_exe PRIVATE /Zi)
    target_link_options(gpencl_exe PRIVATE /DEBUG:FULL)
else()
    target_compile_options(gpencl_exe PRIVATE -g)
endif()

# Rename executable to gpencl
set_target_properties(gpencl_exe PROPERTIES OUTPUT_NAME gpencl)

# -----------------------------------------------------------------------------
# Add tests
add_executable(test_gpencl tests/test_gpencl.cpp)

target_include_directories(test_gpencl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_gpencl gpencl)

if(WIN32)
    target_compile_options(test_gpencl PRIVATE /Zi)
else()
    target_compile_options(test_gpencl PRIVATE -g)
endif()

add_test(NAME gpencl_roundtrip COMMAND test_gpencl)
set_tests_properties(gpencl_roundtrip PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
