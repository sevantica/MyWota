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
#include "MyWota_Command_Processor.h"
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
static CLI_Command_Status_t cmd_dispenser(const CLI_Channel_t* channel, int argc, char** argv);
static CLI_Command_Status_t cmd_clean(const CLI_Channel_t* channel, int argc, char** argv);
static CLI_Command_Status_t cmd_fault(const CLI_Channel_t* channel, int argc, char** argv);

/* Private variables ---------------------------------------------------------*/
static const CLI_Command_Adapter_Entry_t adapter_commands[] = {
    {"dispenser", cmd_dispenser, "Dispenser commands (start, stop, topup)", "dispenser <cmd>"},
    {"clean",     cmd_clean,     "Self-clean (start [vol_ml] [max_sec] | stop | status)", "clean <cmd>"},
    {"fault",     cmd_fault,     "Fault state machine (status | clear)",                   "fault <cmd>"},
};

static const size_t adapter_command_count = sizeof(adapter_commands) / sizeof(adapter_commands[0]);

static CLI_Command_Status_t usb_status_from_system(System_Command_Status_t status)
{
    switch (status) {
        case SYSTEM_CMD_STATUS_OK:
            return CLI_CMD_OK;
        case SYSTEM_CMD_STATUS_UNKNOWN_COMMAND:
            return CLI_CMD_ERROR_UNKNOWN_COMMAND;
        case SYSTEM_CMD_STATUS_INVALID_PARAM:
            return CLI_CMD_ERROR_INVALID_PARAM;
        default:
            return CLI_CMD_ERROR;
    }
}
/* Exported functions --------------------------------------------------------*/

const CLI_Command_Adapter_Entry_t* USB_Command_Adapter_GetCommands(void)
{
    return adapter_commands;
}

size_t USB_Command_Adapter_GetCommandCount(void)
{
    return adapter_command_count;
}

#ifdef USB_Log_Printf
#undef USB_Log_Printf
#endif
#define USB_Log_Printf(...) CLI_Printf(channel, __VA_ARGS__)

void USB_Command_Adapter_PrintStatus(const CLI_Channel_t* channel)
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
 * @return CLI_CMD_OK on success
 */
static CLI_Command_Status_t cmd_dispenser(const CLI_Channel_t* channel, int argc, char** argv)
{
    return MyWota_Command_Processor_ExecuteUSB(channel, argc, argv);
}

static CLI_Command_Status_t cmd_clean(const CLI_Channel_t* channel, int argc, char** argv)
{
    return MyWota_Command_Processor_ExecuteUSB(channel, argc, argv);
}

static CLI_Command_Status_t cmd_fault(const CLI_Channel_t* channel, int argc, char** argv)
{
    return MyWota_Command_Processor_ExecuteUSB(channel, argc, argv);
}

