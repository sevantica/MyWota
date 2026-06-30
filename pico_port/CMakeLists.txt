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
set(PICO_STDIO_UART 0 CACHE BOOL "Disable UART stdio; UART0 is used for RS485" FORCE)

cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK import
include(pico_sdk_import.cmake)

project(UI_PICO_PORT C CXX ASM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Set up shared picotool directory to avoid repeated downloads
set(PICOTOOL_FETCH_FROM_GIT_PATH "C:/Users/bchir/.pico-sdk/picotool" CACHE STRING "Shared directory for picotool builds")

# Set build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build" FORCE)
endif()
message(STATUS "Build type is ${CMAKE_BUILD_TYPE}")

# Define build type for conditional compilation in shared drivers
add_compile_definitions(BUILD_TYPE_WATER_DISPENSER)

set(PICO_MBEDTLS_CONFIG_FILE "RS485_Firmware_mbedtls_config.h")

# Initialize the Pico SDK
set(PICO_NO_PICOTOOL 1)
pico_sdk_init()

# Set up FreeRTOS configuration (must be done BEFORE adding drivers library)
set(FREERTOS_PORT "GCC_RP2040" CACHE STRING "" FORCE)
# FREERTOS_HEAP disabled - using fully static allocation (no heap needed)
# set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/Include/Adapters
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/Include/Controllers
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/Include/System
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/Include/Tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/../application/User_Interface/MyWota
)

# Set path to tusb_config.h for drivers library
set(TUSB_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Path to the shared VS Code Common cross-project directory
set(COMMON_PROJECT_DIR "C:/Business/Cross Project/VS Code Common")

target_include_directories(pico_mbedtls_headers INTERFACE
    "${COMMON_PROJECT_DIR}/sevantica_drivers/Include/Crypto"
)

# Add the custom drivers library (includes FreeRTOS via FetchContent)
# Note: MIFARE Transaction Core now included in sevantica_drivers
add_subdirectory("${COMMON_PROJECT_DIR}/sevantica_drivers" ${CMAKE_CURRENT_BINARY_DIR}/drivers)
target_compile_definitions(Drivers PRIVATE USB_LOG_ONLINE_TERMINAL_MIRROR=1)

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
    ../application/Include/Adapters
    ../application/Include/Controllers
    ../application/Include/System
    ../application/Include/Tasks
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
    ../application/Source/System/MyWota_Module.c
    ../application/Source/System/mywota_ui_driver.c
    ../application/Source/System/MyWota_Command_Processor.c
    ../application/Source/Controllers/Dispenser_Controller.c
    ../application/Source/Controllers/Fault_Manager.c
    ../application/Source/System/Firmware_Version.c
    ../application/Source/System/Hardware_Access.c
    # IO_Expander_Control.c removed - replaced by IO_Expander_Service in sevantica_drivers
    ../application/Source/Adapters/MyWota_Config_Adapter.c
    ../application/Source/Adapters/MyWota_Hardware_Adapter.c
    ../application/Source/Adapters/MyWota_IO_Expander_Adapter.c
    ../application/Source/Adapters/MyWota_Command_Catalog.c
    ../application/Source/Tasks/Feedback_Task.c
    ../application/Source/Adapters/MIFARE_Volume_Adapter.c
    ../application/Source/Tasks/RTC_Task.c
    ../application/Source/Tasks/SD_Logger_Task.c
    ../application/Source/Adapters/SD_Logger_Format_Adapter.c
    # System.c removed - now using shared System_Core.c from sevantica_drivers
    ../application/Source/Adapters/RS485_Command_Adapter.c
    # System_Config.c removed - now part of Drivers library
    ../application/Source/Adapters/USB_Command_Adapter.c
    # Note: MIFARE core and USB Command Handler in sevantica_drivers, adapters in this project
)

# Combine all local sources (drivers now handled by library)
set(ALL_LOCAL_SOURCES ${MAIN_SOURCES} ${APP_SOURCES})

# Create the executable
add_executable(UI_PICO_PORT ${ALL_LOCAL_SOURCES})

# Force Firmware_Version.c to always recompile (updates __DATE__ and __TIME__)
add_custom_command(
    TARGET UI_PICO_PORT
    PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E touch_nocreate "${CMAKE_CURRENT_SOURCE_DIR}/../application/Source/System/Firmware_Version.c"
    COMMENT "Forcing recompilation of Firmware_Version.c for fresh timestamp"
)

# Use custom linker script for bootloader-aware build (app at 0x10010000)
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
    pico_mbedtls_crypto
    pico_mbedtls_headers
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
    USE_DRIVERS_USB=1
    USB_LOG_ONLINE_TERMINAL_MIRROR=1
)

# USB CDC is handled by TinyUSB; UART0 is reserved for RS485.
pico_enable_stdio_usb(UI_PICO_PORT 0)
pico_enable_stdio_uart(UI_PICO_PORT 0)

# Create map/bin/hex/uf2 files automatically after each build
pico_add_extra_outputs(UI_PICO_PORT)

set(FIRMWARE_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../application/Include/System/Firmware_Version.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FIRMWARE_VERSION_HEADER}")
file(READ "${FIRMWARE_VERSION_HEADER}" FW_VERSION_HEADER_CONTENT)
foreach(FW_VERSION_COMPONENT MAJOR MINOR PATCH)
    if(NOT FW_VERSION_HEADER_CONTENT MATCHES "#define[ \t]+FW_VERSION_${FW_VERSION_COMPONENT}[ \t]+([0-9]+)")
        message(FATAL_ERROR "Could not parse FW_VERSION_${FW_VERSION_COMPONENT} from ${FIRMWARE_VERSION_HEADER}")
    endif()
    set(FW_VERSION_${FW_VERSION_COMPONENT}_VALUE "${CMAKE_MATCH_1}")
endforeach()
set(FW_ROLLOUT_VERSION "${FW_VERSION_MAJOR_VALUE}.${FW_VERSION_MINOR_VALUE}.${FW_VERSION_PATCH_VALUE}")

set(RS485_SIGN_FIRMWARE_SCRIPT
    "C:/Business/Central Control Hub/Central Control Hub_FW/Central-Control-Hub/scripts/sign-firmware.py"
    CACHE FILEPATH "Path to the RS485 firmware signing tool"
)
if(NOT EXISTS "${RS485_SIGN_FIRMWARE_SCRIPT}")
    message(FATAL_ERROR "RS485 firmware signing tool not found: ${RS485_SIGN_FIRMWARE_SCRIPT}")
endif()

set(FW_ROLLOUT_BIN "${CMAKE_CURRENT_BINARY_DIR}/mywota.bin")
set(FW_ROLLOUT_INPUT_BIN "${CMAKE_CURRENT_BINARY_DIR}/UI_PICO_PORT.bin")
math(EXPR FW_ROLLOUT_MAX_TOTAL_SIZE "(2 * 1024 * 1024) - (16 * 1024) - 4096 - (64 * 1024) - (980 * 1024)")

# Version is read from the header at build time (not configure time) so incremental
# builds pick up patch bumps made by bump-version.ps1 without a full reconfigure.
set(FW_VERSION_READ_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/read-fw-version.py")

add_custom_command(
    OUTPUT "${FW_ROLLOUT_BIN}"
    COMMAND ${Python3_EXECUTABLE} "${FW_VERSION_READ_SCRIPT}" "${FIRMWARE_VERSION_HEADER}"
            --sign "${RS485_SIGN_FIRMWARE_SCRIPT}" "${FW_ROLLOUT_INPUT_BIN}" "${FW_ROLLOUT_BIN}"
            --product mywota --max-total-size "${FW_ROLLOUT_MAX_TOTAL_SIZE}"
    DEPENDS UI_PICO_PORT "${FW_ROLLOUT_INPUT_BIN}" "${RS485_SIGN_FIRMWARE_SCRIPT}" "${FIRMWARE_VERSION_HEADER}" "${FW_VERSION_READ_SCRIPT}"
    COMMENT "Generating MyWota signed firmware rollout container: mywota.bin"
    VERBATIM
)
add_custom_target(UI_PICO_PORT_fw_rollout ALL DEPENDS "${FW_ROLLOUT_BIN}")
