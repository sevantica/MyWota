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
 * @brief MyWota project-specific USB commands (water dispenser system)
 * @details Implements volume-based dispenser commands and status display
 */

/* Includes ------------------------------------------------------------------*/
#include "USB_Command_Adapter.h"
#include "USB_Logging.h"
#include "Dispenser_Controller.h"
#include "MIFARE_Transaction_Core.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "System_Command.h"
#include "Fault_Manager.h"
#include "System_Config.h"
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

/* Private function prototypes -----------------------------------------------*/
static USB_Command_Status_t cmd_dispenser(int argc, char** argv);
static USB_Command_Status_t cmd_clean(int argc, char** argv);
static USB_Command_Status_t cmd_fault(int argc, char** argv);

/* Private variables ---------------------------------------------------------*/
static const USB_Command_Adapter_Entry_t adapter_commands[] = {
    {"dispenser", cmd_dispenser, "Dispenser commands (start, stop, topup)", "dispenser <cmd>"},
    {"clean",     cmd_clean,     "Self-clean (start [vol_ml] [max_sec] | stop | status)", "clean <cmd>"},
    {"fault",     cmd_fault,     "Fault state machine (status | clear)",                   "fault <cmd>"},
};

static const size_t adapter_command_count = sizeof(adapter_commands) / sizeof(adapter_commands[0]);

static USB_Command_Status_t usb_status_from_system(System_Command_Status_t status)
{
    switch (status) {
        case SYSTEM_CMD_STATUS_OK:
            return USB_CMD_OK;
        case SYSTEM_CMD_STATUS_UNKNOWN_COMMAND:
            return USB_CMD_ERROR_UNKNOWN_COMMAND;
        case SYSTEM_CMD_STATUS_INVALID_PARAM:
            return USB_CMD_ERROR_INVALID_PARAM;
        default:
            return USB_CMD_ERROR;
    }
}
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
    /* Display card balance if card is ready */
    if (MIFARE_IsCardReady()) {
        USB_Log_Printf("Balance:           %lu L\r\n", MIFARE_GetBalance() / 1000u);
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
        
        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_DISPENSE_START,
            .origin = SYSTEM_CMD_ORIGIN_LOCAL_USB,
            .param1 = volume_ml,
        };
        System_Command_Status_t status = System_Command_Execute(&request, NULL);

        if (status == SYSTEM_CMD_STATUS_OK) {
            if (volume_ml == 0) {
                USB_Log_Printf("[✓] Manual dispense started (unlimited)\r\n");
            } else {
                USB_Log_Printf("[✓] Manual dispense started for %lu L (%lu ml)\r\n", volume_l, volume_ml);
            }
            return USB_CMD_OK;
        } else {
            USB_Log_Printf("[✗] Failed to start dispense: %s\r\n", System_Command_GetStatusString(status));
            return usb_status_from_system(status);
        }
    }
    else if (strcmp(subcmd, "stop") == 0) {
        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_DISPENSE_STOP,
            .origin = SYSTEM_CMD_ORIGIN_LOCAL_USB,
        };
        System_Command_Status_t status = System_Command_Execute(&request, NULL);

        if (status == SYSTEM_CMD_STATUS_OK) {
            USB_Log_Printf("[✓] Dispense stopped\r\n");
            return USB_CMD_OK;
        } else {
            USB_Log_Printf("[✗] Failed to stop dispense: %s\r\n", System_Command_GetStatusString(status));
            return usb_status_from_system(status);
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

        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_CARD_TOPUP,
            .origin = SYSTEM_CMD_ORIGIN_LOCAL_USB,
            .param1 = amount_ml,
        };
        return usb_status_from_system(System_Command_Execute(&request, NULL));
    }

    USB_Log_Printf("[✗] Unknown dispenser command: %s\r\n", subcmd);
    return USB_CMD_ERROR_UNKNOWN_COMMAND;
}

/**
 * @brief Self-clean command handler.
 *
 * Subcommands:
 *   start [vol_ml] [max_sec]   - Start a clean cycle (config defaults if 0/missing)
 *   stop                       - Abort an in-progress clean
 *   status                     - Show current clean state
 */
static USB_Command_Status_t cmd_clean(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("Usage: clean <start [vol_ml] [max_sec] | stop | status>\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    const char* sub = argv[1];

    if (strcmp(sub, "start") == 0) {
        uint32_t vol = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 0;
        uint32_t sec = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 0;
        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_CLEAN_START,
            .origin = SYSTEM_CMD_ORIGIN_LOCAL_USB,
            .param1 = vol,
            .param2 = sec,
        };
        System_Command_Status_t status = System_Command_Execute(&request, NULL);
        if (status == SYSTEM_CMD_STATUS_OK) {
            USB_Log_Printf("[→] Self-clean started\r\n");
            return USB_CMD_OK;
        }
        USB_Log_Printf("[✗] Self-clean refused: %s\r\n", System_Command_GetStatusString(status));
        return usb_status_from_system(status);
    }
    if (strcmp(sub, "stop") == 0) {
        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_CLEAN_STOP,
            .origin = SYSTEM_CMD_ORIGIN_LOCAL_USB,
        };
        System_Command_Status_t status = System_Command_Execute(&request, NULL);
        if (status != SYSTEM_CMD_STATUS_OK) {
            USB_Log_Printf("[✗] Self-clean stop refused: %s\r\n", System_Command_GetStatusString(status));
            return usb_status_from_system(status);
        }
        USB_Log_Printf("[→] Self-clean stop requested\r\n");
        return USB_CMD_OK;
    }
    if (strcmp(sub, "status") == 0) {
        USB_Log_Printf("Self-clean active: %s\r\n",
                       Dispenser_IsSelfCleaning() ? "YES" : "no");
        USB_Log_Printf("Last clean (unix): %lu\r\n",
                       (unsigned long)Dispenser_GetLastCleanUnixTime());
        return USB_CMD_OK;
    }

    USB_Log_Printf("[✗] Unknown clean subcommand: %s\r\n", sub);
    return USB_CMD_ERROR_UNKNOWN_COMMAND;
}


/**
 * @brief Fault state machine inspection / clear.
 *   fault status   - print current state and reason
 *   fault clear    - clear latched FAULT (operator action)
 */
static USB_Command_Status_t cmd_fault(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("Usage: fault <status|clear>\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    const char* sub = argv[1];

    if (strcmp(sub, "status") == 0) {
        USB_Log_Printf("Fault state:       %s\r\n",
                       Fault_Manager_GetStateString(Fault_Manager_GetState()));
        USB_Log_Printf("Fault reason:      %s\r\n",
                       Fault_Manager_GetReasonString(Fault_Manager_GetReason()));
        USB_Log_Printf("Incidents (boot):  %lu\r\n",
                       (unsigned long)Fault_Manager_GetIncidentCount());
        return USB_CMD_OK;
    }
    if (strcmp(sub, "clear") == 0) {
        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_FAULT_CLEAR,
            .origin = SYSTEM_CMD_ORIGIN_LOCAL_USB,
        };
        System_Command_Status_t status = System_Command_Execute(&request, NULL);
        if (status != SYSTEM_CMD_STATUS_OK) {
            USB_Log_Printf("[✗] Fault clear refused: %s\r\n", System_Command_GetStatusString(status));
            return usb_status_from_system(status);
        }
        USB_Log_Printf("[✓] Fault cleared\r\n");
        return USB_CMD_OK;
    }

    USB_Log_Printf("[✗] Unknown fault subcommand: %s\r\n", sub);
    return USB_CMD_ERROR_UNKNOWN_COMMAND;
}

