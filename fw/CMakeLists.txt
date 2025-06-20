cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# USER CODE BEGIN Compile Definitions
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(DEBUG)
    add_compile_definitions(ULOG_ENABLED)
    add_compile_definitions(ULOG_COLOR)
    add_compile_definitions(ULOG_LOG_FILENAME)
endif()

add_compile_definitions(BIT_ZERO=PHYSYMBOL_A)
add_compile_definitions(BIT_ONE=PHYSYMBOL_B)
# USER CODE END

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME wake-mod-firmware)

# Include toolchain file
include("cmake/gcc-arm-none-eabi.cmake")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# USER CODE BEGIN Include directories
# rtt logging
add_subdirectory(Drivers/logging)

# include FH101RF driver
add_subdirectory(Drivers/fh101rf/c)

# include OOK driver
add_subdirectory(Drivers/ook)

# include ring buffer
add_subdirectory(Drivers/lwrb)
# USER CODE END

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/utils.c
    Core/Src/i2c_slave.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
    # USER CODE BEGIN
    logging
    fh101rf_driver
    ook_driver
    lwrb

    # USER CODE END
)
