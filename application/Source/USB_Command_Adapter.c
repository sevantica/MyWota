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
static USB_Command_Status_t cmd_washstart(int argc, char** argv);
static USB_Command_Status_t cmd_washstop(int argc, char** argv);

/* Private variables ---------------------------------------------------------*/
static const USB_Command_Adapter_Entry_t adapter_commands[] = {
    {"washstart", cmd_washstart, "Start wash manually (no card needed)",  "washstart [seconds] [option]"},
    {"washstop",  cmd_washstop,  "Stop wash manually",                    "washstop"},
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
    return "Car_Wash_Controller.h";
}

/* Private functions ---------------------------------------------------------*/

/**
 * @brief Start wash manually without card
 * @param argc Argument count
 * @param argv Arguments: [duration_seconds] [option]
 *             option: 1=vacuum, 2=brush, 3=pressure
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_washstart(int argc, char** argv)
{
    uint32_t duration = 0;  /* 0 = use default */
    WashOption_t option = WASH_OPTION_NONE;  /* NONE = use vacuum cleaner */
    
    /* Parse optional duration argument */
    if (argc >= 2) {
        duration = (uint32_t)atoi(argv[1]);
        if (duration > 3600) {
            USB_Log_Printf("[✗] Duration too long (max 3600 seconds)\r\n");
            return USB_CMD_ERROR_INVALID_PARAM;
        }
    }
    
    /* Parse optional option argument */
    if (argc >= 3) {
        int opt = atoi(argv[2]);
        switch (opt) {
            case 1: option = WASH_OPTION_VACUUM_CLEANER; break;
            case 2: option = WASH_OPTION_WASH_BRUSH; break;
            case 3: option = WASH_OPTION_PRESSURE_WASHER; break;
            default:
                USB_Log_Printf("[✗] Invalid option: %d (use 1=vacuum, 2=brush, 3=pressure)\r\n", opt);
                return USB_CMD_ERROR_INVALID_PARAM;
        }
    }
    
    CarWashResult_t result = MIFARE_CarWash_ManualStart(option, duration);
    
    if (result == CARWASH_RESULT_OK) {
        if (duration == 0) {
            USB_Log_Printf("[✓] Manual wash started (default duration)\r\n");
        } else {
            USB_Log_Printf("[✓] Manual wash started for %lu seconds\r\n", duration);
        }
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Failed to start wash (already running?)\r\n");
        return USB_CMD_ERROR;
    }
}

/**
 * @brief Stop wash manually
 * @param argc Argument count (unused)
 * @param argv Arguments (unused)
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_washstop(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    CarWashResult_t result = MIFARE_CarWash_ManualStop();
    
    if (result == CARWASH_RESULT_OK) {
        USB_Log_Printf("[✓] Wash stopped\r\n");
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Failed to stop wash\r\n");
        return USB_CMD_ERROR;
    }
}




