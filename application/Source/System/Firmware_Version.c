/*
 * @attention
 * Copyright (c) Sevantica 2025.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file Firmware_Version.c
 * @brief Firmware version tracking and display
 * @details Provides version identification for runtime debugging and logging.
 *          Version is compile-time constant independent of bootloader.
 */

/*Includes ----------------------------------------------------------*/
#include "Firmware_Version.h"
#include "USB_Logging.h"
#include <stdio.h>

/*Private variables ----------------------------------------------------------*/
static char version_buffer[32];

/*Public Functions ----------------------------------------------------------*/

/**
 * @brief Get firmware version string
 * @return const char* Version string in format "Major.Minor.Patch.Build"
 */
const char* FW_GetVersionString(void) {
    snprintf(version_buffer, sizeof(version_buffer), 
             "%d.%d.%d.%d", 
             FW_VERSION_MAJOR, FW_VERSION_MINOR, 
             FW_VERSION_PATCH, FW_BUILD_NUMBER);
    return version_buffer;
}

/**
 * @brief Get build date string
 * @return const char* Build date (e.g., "Jan 10 2026")
 */
const char* FW_GetBuildDateString(void) {
    return FW_BUILD_DATE;
}

/**
 * @brief Get build time string
 * @return const char* Build time (e.g., "14:23:45")
 */
const char* FW_GetBuildTimeString(void) {
    return FW_BUILD_TIME;
}

/**
 * @brief Get firmware version as packed 32-bit number
 * @return uint32_t Version number (bits 31-24: Major, 23-16: Minor, 15-8: Patch, 7-0: Build)
 */
uint32_t FW_GetVersionNumber(void) {
    return ((uint32_t)FW_VERSION_MAJOR << 24) | 
           ((uint32_t)FW_VERSION_MINOR << 16) | 
           ((uint32_t)FW_VERSION_PATCH << 8) | 
           (uint32_t)FW_BUILD_NUMBER;
}

/**
 * @brief Print full firmware version information to USB log
 * @details Displays formatted version banner with all version details
 */
void FW_PrintVersionInfo(void) {
    USB_Log_Printf("\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("           %s FIRMWARE VERSION\r\n", FW_PROJECT_NAME);
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("  Version:        %s\r\n", FW_GetVersionString());
    USB_Log_Printf("  Build Date:     %s\r\n", FW_GetBuildDateString());
    USB_Log_Printf("  Build Time:     %s\r\n", FW_GetBuildTimeString());
    USB_Log_Printf("  Min Bootloader: %s\r\n", FW_MIN_BOOTLOADER_VERSION);
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("\r\n");
}
