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
#include "System.h"
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

/* Private function prototypes -----------------------------------------------*/
static USB_Command_Status_t cmd_dispenser(int argc, char** argv);

/* Private variables ---------------------------------------------------------*/
static const USB_Command_Adapter_Entry_t adapter_commands[] = {
    {"dispenser", cmd_dispenser, "Dispenser commands (start, stop, topup)", "dispenser <cmd>"},
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
    /* Display token balance if card is ready */
    if (MIFARE_IsCardReady()) {
        USB_Log_Printf("Token Balance:     %lu ml\r\n", MIFARE_GetBalance());
    }
    
    /* Display dispenser status */
    if (MIFARE_Dispenser_IsDispenseActive()) {
        USB_Log_Printf("Dispenser:         ACTIVE (Dispensed: %lu ml)\r\n", Dispenser_GetDispensedAmountML());
    } else {
        USB_Log_Printf("Dispenser:         IDLE\r\n");
    }
}

const char* USB_Command_Adapter_GetIncludesInfo(void)
{
    return "Dispenser_Controller.h";
}

/* Private functions ---------------------------------------------------------*/

/**
 * @brief Dispenser command handler
 * @param argc Argument count
 * @param argv Arguments: [start|stop|topup] [params...]
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_dispenser(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("Usage: dispenser <command>\r\n");
        USB_Log_Printf("Commands:\r\n");
        USB_Log_Printf("  start [L]    - Start manual dispense (min 1L)\r\n");
        USB_Log_Printf("  stop         - Stop manual dispense\r\n");
        USB_Log_Printf("  topup <L>    - Topup card balance (min 1L)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }

    const char* subcmd = argv[1];

    if (strcmp(subcmd, "start") == 0) {
        uint32_t volume_l = 0;  /* 0 = default/unlimited if no arg provided? User said min is 1L. Assuming no arg means default/unlimited still. */
        uint32_t volume_ml = 0;
        
        /* Parse optional volume argument */
        if (argc >= 3) {
            volume_l = (uint32_t)atoi(argv[2]);
            if (volume_l < 1) {
                USB_Log_Printf("[✗] Invalid volume (min 1L)\r\n");
                return USB_CMD_ERROR_INVALID_PARAM;
            }
            if (volume_l > 1000) { /* 1000L reasonable safety limit? previously 20000ml = 20L */
                USB_Log_Printf("[✗] Volume too large (max 1000 L)\r\n");
                return USB_CMD_ERROR_INVALID_PARAM;
            }
            volume_ml = volume_l * 1000;
        }
        
        DispenserResult_t result = MIFARE_Dispenser_ManualStart(volume_ml);
        
        if (result == DISPENSER_RESULT_OK) {
            if (volume_ml == 0) {
                USB_Log_Printf("[✓] Manual dispense started (unlimited)\r\n");
            } else {
                USB_Log_Printf("[✓] Manual dispense started for %lu L (%lu ml)\r\n", volume_l, volume_ml);
            }
            return USB_CMD_OK;
        } else {
            USB_Log_Printf("[✗] Failed to start dispense (already running?)\r\n");
            return USB_CMD_ERROR;
        }
    }
    else if (strcmp(subcmd, "stop") == 0) {
        DispenserResult_t result = MIFARE_Dispenser_ManualStop();
        
        if (result == DISPENSER_RESULT_OK) {
            USB_Log_Printf("[✓] Dispense stopped\r\n");
            return USB_CMD_OK;
        } else {
            USB_Log_Printf("[✗] Failed to stop dispense\r\n");
            return USB_CMD_ERROR;
        }
    }
    else if (strcmp(subcmd, "topup") == 0) {
        if (argc < 3) {
            USB_Log_Printf("[✗] Usage: dispenser topup <amount_L>\r\n");
            return USB_CMD_ERROR_INVALID_PARAM;
        }
        
        uint32_t amount_l = (uint32_t)atoi(argv[2]);
        if (amount_l < 1) {
            USB_Log_Printf("[✗] Invalid amount (min 1L)\r\n");
            return USB_CMD_ERROR_INVALID_PARAM;
        }
        
        uint32_t amount_ml = amount_l * 1000;
        
        USB_Log_Printf("[→] Topping up card with %lu L (%lu ml)...\r\n", amount_l, amount_ml);
        
        /* Attempt to topup directly via Dispenser Controller */
        DispenserResult_t result = MIFARE_Dispenser_TopupCard(amount_ml);
        
        if (result == DISPENSER_RESULT_OK) {
            USB_Log_Printf("[✓] Topup successful. New balance: %lu ml\r\n", MIFARE_GetBalance());
            return USB_CMD_OK;
        } else if (result == DISPENSER_RESULT_CARD_NOT_READY) {
            USB_Log_Printf("[✗] Card not ready. Please present card.\r\n");
            return USB_CMD_ERROR;
        } else {
            USB_Log_Printf("[✗] Topup failed (Result: %d)\r\n", result);
            return USB_CMD_ERROR;
        }
    }

    USB_Log_Printf("[✗] Unknown dispenser command: %s\r\n", subcmd);
    return USB_CMD_ERROR_UNKNOWN_COMMAND;
}


