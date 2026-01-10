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
 * @brief Firmware version information and display
 */

/* Includes ------------------------------------------------------------------*/
#include "Firmware_Version.h"
#include "USB_Logging.h"
#include "pico/unique_id.h"
#include <stdio.h>

/* Public Functions ----------------------------------------------------------*/

void FW_PrintVersionInfo(void)
{
    // Get Pico unique ID
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    
    USB_Log_Printf("\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("                    FIRMWARE INFORMATION                        \r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("Product:         %s\r\n", FW_PRODUCT_NAME);
    USB_Log_Printf("Version:         %s\r\n", FW_VERSION_STRING);
    USB_Log_Printf("Build Date:      %s %s\r\n", FW_BUILD_DATE, FW_BUILD_TIME);
    USB_Log_Printf("Hardware:        %s\r\n", FW_HARDWARE_VERSION);
    USB_Log_Printf("Board ID:        ");
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        USB_Log_Printf("%02X", board_id.id[i]);
    }
    USB_Log_Printf("\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("\r\n");
}
