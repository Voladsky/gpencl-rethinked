cmake_minimum_required(VERSION 3.10)
project(vk_chacha20poly1305)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
# Add executable
add_executable(vk_chacha20poly1305 main.cpp)

# Include directories
target_include_directories(vk_chacha20poly1305 PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${SHADERC_INCLUDE_DIR}
)

# Link libraries
target_link_libraries(vk_chacha20poly1305
    ${Vulkan_LIBRARIES}
    ${SHADERC_LIB}
)

# Linux-specific libraries
if(UNIX AND NOT APPLE)
    target_link_libraries(vk_chacha20poly1305 pthread dl)
endif()

# Optional: for Windows, you may need to define VK_USE_PLATFORM_WIN32_KHR
if(WIN32)
    target_compile_definitions(vk_chacha20poly1305 PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()