cmake_minimum_required(VERSION 3.10)
project(VulkanXorExample)

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
    # shaderc library (static combined library)
    find_library(SHADERC_LIB
        NAMES shaderc_combined
        PATHS ${VULKAN_SDK_PATH}/Lib
        NO_DEFAULT_PATH
    )
else()
    message(FATAL_ERROR "Vulkan SDK not found. Please set VULKAN_SDK environment variable.")
endif()

if(NOT SHADERC_LIB)
    message(FATAL_ERROR "Could not find shaderc_combined.lib. Ensure Vulkan SDK is installed and the library exists.")
endif()

message(STATUS "shaderc include: ${SHADERC_INCLUDE_DIR}")
message(STATUS "shaderc library: ${SHADERC_LIB}")

# -----------------------------------------------------------------------------
# Add executable
add_executable(vulkan_xor main.cpp)

# Include directories
target_include_directories(vulkan_xor PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${SHADERC_INCLUDE_DIR}
)

# Link libraries
target_link_libraries(vulkan_xor
    ${Vulkan_LIBRARIES}
    ${SHADERC_LIB}
)

# Optional: for Windows, you may need to define VK_USE_PLATFORM_WIN32_KHR
target_compile_definitions(vulkan_xor PRIVATE VK_USE_PLATFORM_WIN32_KHR)