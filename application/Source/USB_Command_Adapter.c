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
 * @file USB_Command_Adapter.c
 * @brief BigYellow project-specific USB commands (car wash system)
 * @details Implements token-based wash commands and status display
 */

/* Includes ------------------------------------------------------------------*/
#include "USB_Command_Adapter.h"
#include "USB_Logging.h"
#include "Dispenser_Controller.h"
#include "MIFARE_Transaction_Core.h"
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
/* Private function prototypes -----------------------------------------------*/
static USB_Command_Status_t cmd_dispense_start(int argc, char** argv);
static USB_Command_Status_t cmd_dispense_stop(int argc, char** argv);

/* Private variables ---------------------------------------------------------*/
static const USB_Command_Adapter_Entry_t adapter_commands[] = {
    {"start_dispense", cmd_dispense_start, "Start dispense manually (no card needed)",  "start_dispense [ml]"},
    {"stop_dispense",  cmd_dispense_stop,  "Stop dispense manually",                    "stop_dispense"},
};

static const size_t adapter_command_count = sizeof(adapter_commands) / sizeof(adapter_commands[0]);

/* Exported functions --------------------------------------------------------*/

const USB_Command_Adapter_Entry_t* USB_Command_Adapter_GetCommands(void)
{
    return adapter_commands;
}

size_t USB_Command_Adapter_GetCommandCount(void)
{
    return adapter_command_count;
}

void USB_Command_Adapter_PrintStatus(void)
{
    /* Display token balance */
    if (MIFARE_IsCardReady()) {
        USB_Log_Printf("Token Balance:     %lu tokens\r\n", MIFARE_GetBalance());
    }
    
    /* Car wash status could be added here if needed */
}

const char* USB_Command_Adapter_GetIncludesInfo(void)
{
    return "Dispenser_Controller.h";
}

/* Private functions ---------------------------------------------------------*/

/**
 * @brief Start wash manually without card
 * @param argc Argument count
 * @param argv Arguments: [duration_seconds] [option]
 *             option: 1=vacuum, 2=brush, 3=pressure
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_dispense_start(int argc, char** argv)
{
    uint32_t volume_ml = 0;  /* 0 = default (20L) */
    
    /* Parse optional volume argument */
    if (argc >= 2) {
        volume_ml = (uint32_t)atoi(argv[1]);
        if (volume_ml > 20000) {
            USB_Log_Printf("[✗] Volume too large (max 20000 ml)\r\n");
            return USB_CMD_ERROR_INVALID_PARAM;
        }
    }
    
    DispenserResult_t result = MIFARE_Dispenser_ManualStart(volume_ml);
    
    if (result == DISPENSER_RESULT_OK) {
        if (volume_ml == 0) {
            USB_Log_Printf("[✓] Manual dispense started (default volume)\r\n");
        } else {
            USB_Log_Printf("[✓] Manual dispense started for %lu ml\r\n", volume_ml);
        }
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Failed to start dispense (already running?)\r\n");
        return USB_CMD_ERROR;
    }
}

/**
 * @brief Stop wash manually
 * @param argc Argument count (unused)
 * @param argv Arguments (unused)
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_dispense_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    DispenserResult_t result = MIFARE_Dispenser_ManualStop();
    
    if (result == DISPENSER_RESULT_OK) {
        USB_Log_Printf("[✓] Dispense stopped\r\n");
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Failed to stop dispense\r\n");
        return USB_CMD_ERROR;
    }
}




