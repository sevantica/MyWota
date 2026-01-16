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

# Define build type for conditional compilation in shared drivers
add_compile_definitions(BUILD_TYPE_WATER_DISPENSER)

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
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/MyWota/lv_conf.h)

# Disable building LVGL examples and demos to save memory and build time
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
set(LV_CONF_BUILD_DISABLE_DEMOS 1)

# Add the application LVGL library BEFORE drivers (drivers needs to link against it)
add_subdirectory(../application/User_Interface/MyWota/lvgl ${CMAKE_CURRENT_BINARY_DIR}/app_lvgl)

# Set additional include directories for drivers library (needed for Hardware_Access.h, lv_conf.h, etc.)
set(PARENT_INCLUDE_DIRS 
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/MyWota
)

# Set path to tusb_config.h for drivers library
set(TUSB_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Path to the shared VS Code Common cross-project directory
set(COMMON_PROJECT_DIR "C:/Business/Cross Project/VS Code Common")

# Add the custom drivers library (includes FreeRTOS via FetchContent)
# Note: MIFARE Transaction Core now included in sevantica_drivers
add_subdirectory("${COMMON_PROJECT_DIR}/sevantica_drivers" ${CMAKE_CURRENT_BINARY_DIR}/drivers)

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
    ../application/User_Interface/MyWota  # Add this for lv_conf.h
    ../application/User_Interface/MyWota/lvgl/src
    ../application/User_Interface/MyWota/lvgl/src/drivers
    ../application/Include
    "${COMMON_PROJECT_DIR}/sevantica_drivers/Include"
    ../application/User_Interface/MyWota/ui
    ../application/User_Interface/MyWota/ui/screens
    ../application/User_Interface/MyWota/ui/images
    ../application/User_Interface/MyWota/ui/components

    ${CMAKE_CURRENT_SOURCE_DIR}/screens
)

# Add UI library after LVGL
add_subdirectory(../application/User_Interface/MyWota/ui ${CMAKE_CURRENT_BINARY_DIR}/app_ui)

# Main source file
set(MAIN_SOURCES
    main.c
    usb_descriptors.c
)

# Application source files (explicit list - no GLOB)
set(APP_SOURCES
    ../application/Source/MyWota_Module.c
    ../application/Source/MyWota_ui_driver.c
    ../application/Source/Dispenser_Controller.c
    ../application/Source/Firmware_Version.c
    ../application/Source/Hardware_Access.c
    ../application/Source/IO_Expander_Control.c
    ../application/Source/MyWota_Config_Adapter.c
    ../application/Source/MyWota_Hardware_Adapter.c
    ../application/Source/MyWota_IO_Expander_Adapter.c
    ../application/Source/MIFARE_Volume_Adapter.c
    ../application/Source/RS485_Task.c
    ../application/Source/RTC_Manager.c
    ../application/Source/RTC_Persistence_Adapter.c
    ../application/Source/RTC_Task.c
    ../application/Source/SD_Logger_Task.c
    ../application/Source/SD_Logger_Format_Adapter.c
    # System.c removed - now using shared System_Core.c from sevantica_drivers
    ../application/Source/RS485_Command_Adapter.c
    ../application/Source/System_Config.c
    ../application/Source/USB_Command_Adapter.c
    # Note: MIFARE core and USB Command Handler in sevantica_drivers, adapters in this project
)

# Combine all local sources (drivers now handled by library)
set(ALL_LOCAL_SOURCES ${MAIN_SOURCES} ${APP_SOURCES})

# Create the executable
add_executable(UI_PICO_PORT ${ALL_LOCAL_SOURCES})

# Use custom linker script for bootloader-aware build (app at 0x10008000)
pico_set_linker_script(UI_PICO_PORT ${CMAKE_CURRENT_SOURCE_DIR}/memmap_app.ld)

# Link libraries
target_link_libraries(UI_PICO_PORT
    ui    # Add UI library from ../application/User_Interface/MyWota/ui
    lvgl  # Add LVGL library
    Drivers  # Add custom drivers library (includes MIFARE core)
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
    pico_stdio_uart  # UART stdio only (USB handled by TinyUSB)
    tinyusb_device   # TinyUSB device stack for CDC+MSC composite
    tinyusb_board    # TinyUSB board support
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/MyWota  # Add path for lv_conf.h
)

# Add FatFs include path as SYSTEM to LVGL for LV_USE_FS_FATFS support
target_include_directories(lvgl SYSTEM PUBLIC
    "${COMMON_PROJECT_DIR}/sevantica_drivers/FatFs"
    "${COMMON_PROJECT_DIR}/sevantica_drivers/Include"
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/MyWota  # Add path for lv_conf.h
)

# Add include directories for UI_PICO_PORT
target_include_directories(UI_PICO_PORT PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/freertos_config
)

# Add tusb_config.h location for TinyUSB
target_include_directories(UI_PICO_PORT PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}  # For tusb_config.h
)

# Define that we're using TinyUSB device mode (for conditional compilation)
target_compile_definitions(UI_PICO_PORT PRIVATE
    LIB_TINYUSB_DEVICE=1
    CFG_TUSB_MCU=OPT_MCU_RP2040
)

# Enable UART output only (USB CDC handled by TinyUSB)
pico_enable_stdio_usb(UI_PICO_PORT 0)
pico_enable_stdio_uart(UI_PICO_PORT 0)

# Create map/bin/hex/uf2 files automatically after each build
pico_add_extra_outputs(UI_PICO_PORT)
