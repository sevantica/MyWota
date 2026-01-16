# Pico SDK Toolchain File
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ARM)

# Specify the Pico SDK ARM toolchain
set(PICO_GCC_TRIPLE arm-none-eabi)
set(PICO_TOOLCHAIN_PATH "C:/Program Files/Raspberry Pi/Pico SDK v1.5.1/gcc-arm-none-eabi/bin")

set(CMAKE_C_COMPILER "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-gcc.exe")
set(CMAKE_CXX_COMPILER "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-g++.exe")
set(CMAKE_ASM_COMPILER "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-gcc.exe")

set(CMAKE_OBJCOPY "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-objcopy.exe" CACHE INTERNAL "")
set(CMAKE_OBJDUMP "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-objdump.exe" CACHE INTERNAL "")
set(CMAKE_SIZE "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-size.exe" CACHE INTERNAL "")
set(CMAKE_AR "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-ar.exe" CACHE INTERNAL "")
set(CMAKE_RANLIB "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-ranlib.exe" CACHE INTERNAL "")
set(CMAKE_STRIP "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-strip.exe" CACHE INTERNAL "")
set(CMAKE_NM "${PICO_TOOLCHAIN_PATH}/arm-none-eabi-nm.exe" CACHE INTERNAL "")

# Search for programs only in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Search for libraries and headers only in the target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
