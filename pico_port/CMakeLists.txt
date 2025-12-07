# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK import
include(pico_sdk_import.cmake)

project(UI_PICO_PORT C CXX ASM)

# Set up shared picotool directory to avoid repeated downloads
set(PICOTOOL_FETCH_FROM_GIT_PATH "C:/Users/bchir/.pico-sdk/picotool" CACHE STRING "Shared directory for picotool builds")

# Set build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build" FORCE)
endif()
message(STATUS "Build type is ${CMAKE_BUILD_TYPE}")

# Initialize the Pico SDK
pico_sdk_init()

# Set up FreeRTOS configuration (must be done BEFORE adding drivers library)
set(FREERTOS_PORT "GCC_RP2040" CACHE STRING "" FORCE)
set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)

# Create freertos_config target BEFORE adding drivers (required by FreeRTOS)
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_config
)

# Set the path to lv_conf.h for LVGL (must be set BEFORE adding LVGL)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/miWota/lv_conf.h)

# Disable building LVGL examples and demos to save memory and build time
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
set(LV_CONF_BUILD_DISABLE_DEMOS 1)

# Add the application LVGL library BEFORE drivers (drivers needs to link against it)
add_subdirectory(../application/User_Interface/miWota/lvgl ${CMAKE_CURRENT_BINARY_DIR}/app_lvgl)

# Set additional include directories for drivers library (needed for Hardware_Access.h, lv_conf.h, etc.)
set(PARENT_INCLUDE_DIRS 
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/miWota
)

# Add the custom drivers library (includes FreeRTOS via FetchContent)
add_subdirectory(../../../Common/sevantica_drivers ${CMAKE_CURRENT_BINARY_DIR}/drivers)

# Set C/C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Use deoptimized debug for better breakpoint alignment
set(PICO_DEOPTIMIZED_DEBUG 1)

# Add compile options
add_compile_options(-Wall
    -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
    -Wno-unused-function # we have some for the docs that aren't called
    -Wno-maybe-uninitialized
)

# Enhanced debug settings with no optimization
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Remove all optimizations - use -O0 for true unoptimized debug
    string(REPLACE "-O2" "-O0" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "-O3" "-O0" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "-Og" "-O0" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "-O2" "-O0" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "-O3" "-O0" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "-Og" "-O0" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    
    # Add enhanced debug symbols and preserve debugging information
    add_compile_options(-g3 -DDEBUG=1)
    add_compile_options(-fno-omit-frame-pointer)
    add_compile_options(-fno-eliminate-unused-debug-symbols)
    
    # Additional flags for completely unoptimized debug
    add_compile_options(-fno-inline)
    add_compile_options(-fno-inline-functions)
    add_compile_options(-fno-inline-small-functions)
    add_compile_options(-fno-inline-functions-called-once)
    
    add_link_options(-g3)
    message(STATUS "Debug build: Using -O0 with no optimizations for best debugging experience")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_config
    ../application/User_Interface/miWota  # Add this for lv_conf.h
    ../application/User_Interface/miWota/lvgl/src
    ../application/User_Interface/miWota/lvgl/src/drivers
    ../application/Include
    ../../../Common/sevantica_drivers/Include
    ../application/User_Interface/miWota/ui
    ../application/User_Interface/miWota/ui/screens
    ../application/User_Interface/miWota/ui/images
    ../application/User_Interface/miWota/ui/components

    ${CMAKE_CURRENT_SOURCE_DIR}/screens
)

# Add UI library after LVGL
add_subdirectory(../application/User_Interface/miWota/ui ${CMAKE_CURRENT_BINARY_DIR}/app_ui)

# Collect source files carefully to avoid duplicates
file(GLOB MAIN_SOURCES
    "main.c"
)

# Add application sources
file(GLOB APP_SOURCES
    "../application/Source/*.c"
)

# Add local screen sources (if any)
file(GLOB LOCAL_SCREEN_SOURCES  
    "screens/*.c"
)

# Combine all local sources (drivers now handled by library)
set(ALL_LOCAL_SOURCES ${MAIN_SOURCES} ${APP_SOURCES} ${LOCAL_SCREEN_SOURCES})

# Create the executable
add_executable(UI_PICO_PORT ${ALL_LOCAL_SOURCES})

# Link libraries
target_link_libraries(UI_PICO_PORT
    ui    # Add UI library from ../application/User_Interface/miWota/ui
    lvgl  # Add LVGL library
    Drivers  # Add custom drivers library
    pico_runtime
    pico_platform
    pico_multicore
    pico_sync
    pico_bootrom
    pico_bit_ops
    pico_divider
    pico_double
    pico_float
    pico_malloc
    pico_mem_ops
    pico_printf
    pico_stdio_usb  # USB stdio for Pico SDK
    hardware_base
    hardware_claim
    hardware_sync
    hardware_irq
    hardware_gpio
    hardware_spi
    hardware_i2c
    hardware_dma
    hardware_pio
    hardware_interp
    hardware_timer
    hardware_watchdog
    hardware_clocks
    hardware_flash
    hardware_uart
    hardware_pwm
    hardware_adc
    pico_unique_id
    pico_util
    pico_time
    freertos_kernel
    lvgl
)

# Make sure LVGL has access to FreeRTOS headers and lv_conf.h
target_include_directories(lvgl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_config
    ${FREERTOS_KERNEL_PATH}/include
    ${FREERTOS_KERNEL_PATH}/portable/GCC/RP2040/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/miWota  # Add path for lv_conf.h
)

# Also need to link LVGL with FreeRTOS-Kernel and Pico SDK for headers
target_link_libraries(lvgl PUBLIC
    freertos_kernel
    pico_base_headers
    pico_platform
    pico_stdlib_headers
)

# Explicitly add the pico.h include directory to LVGL and lv_conf.h
target_include_directories(lvgl PUBLIC
    ${PICO_SDK_PATH}/src/common/pico_base_headers/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/miWota  # Add path for lv_conf.h
)

# Add include directories for UI_PICO_PORT
target_include_directories(UI_PICO_PORT PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_config
)

# Enable UART output and USB stdio (letting Pico SDK handle USB properly)
pico_enable_stdio_usb(UI_PICO_PORT 1)
pico_enable_stdio_uart(UI_PICO_PORT 1)

# Create map/bin/hex/uf2 files automatically after each build
pico_add_extra_outputs(UI_PICO_PORT)
