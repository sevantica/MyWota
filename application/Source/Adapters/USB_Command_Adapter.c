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
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "System_Command.h"
#include "Fault_Manager.h"
#include "System_Config.h"
#include "RS485_Discovery.h"
#include "RS485_Protocol.h"
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/watchdog.h"

/* Private function prototypes -----------------------------------------------*/
static CLI_Command_Status_t cmd_dispenser(const CLI_Channel_t* channel, int argc, char** argv);
static CLI_Command_Status_t cmd_clean(const CLI_Channel_t* channel, int argc, char** argv);
static CLI_Command_Status_t cmd_fault(const CLI_Channel_t* channel, int argc, char** argv);
static CLI_Command_Status_t cmd_rs485id(const CLI_Channel_t* channel, int argc, char** argv);

/* Private variables ---------------------------------------------------------*/
static const CLI_Command_Adapter_Entry_t adapter_commands[] = {
    {"dispenser", cmd_dispenser, "Dispenser commands (start, stop, topup)", "dispenser <cmd>"},
    {"clean",     cmd_clean,     "Self-clean (start [vol_ml] [max_sec] | stop | status)", "clean <cmd>"},
    {"fault",     cmd_fault,     "Fault state machine (status | clear)",                   "fault <cmd>"},
    {"rs485id",   cmd_rs485id,   "Query or reset RS485 identity (UID + Device ID)",        "rs485id [reset]"},
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
    if (Dispenser_IsCardReady()) {
        USB_Log_Printf("Balance:           %lu L\r\n", Dispenser_GetBalanceMl() / 1000u);
    }
    
    /* Display dispenser status */
    if (Dispenser_IsDispenseActive()) {
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

/**
 * @brief Print this device's RS485 identity (minted UID + derived Device ID).
 */
static void print_rs485_identity(const CLI_Channel_t* channel)
{
    const uint8_t* uid = RS485_Discovery_Slave_GetUID();
    if (uid == NULL) {
        USB_Log_Printf("RS485 identity not initialized\r\n");
        return;
    }

    uint8_t dtype = RS485_Discovery_Slave_GetDeviceType();
    uint8_t addr  = RS485_Discovery_Slave_GetSavedAddress();

    USB_Log_Printf("RS485 Identity:\r\n");
    USB_Log_Printf("  UID:         %02X%02X%02X%02X%02X%02X%02X%02X\r\n",
                   uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7]);
    USB_Log_Printf("  Device Type: %u (%s)\r\n", dtype,
                   (dtype == RS485_DEVICE_TYPE_WATER_DISPENSER) ? "Water Dispenser" : "Unknown");
    if (addr == 0) {
        USB_Log_Printf("  RS485 Addr:  unassigned (pending discovery)\r\n");
        USB_Log_Printf("  Device ID:   (unassigned)\r\n");
    } else {
        USB_Log_Printf("  RS485 Addr:  0x%02X (unit %u)\r\n", addr, RS485_ADDR_GET_UNIT(addr));
        USB_Log_Printf("  Device ID:   MW-%02u\r\n", RS485_ADDR_GET_UNIT(addr));
    }
}

/**
 * @brief RS485 identity command.
 * @details "rs485id" prints the UID + Device ID; "rs485id reset" re-mints a fresh
 *          random UID, clears the assigned address, and reboots so the master
 *          re-discovers this device with a brand-new identity.
 */
static CLI_Command_Status_t cmd_rs485id(const CLI_Channel_t* channel, int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        USB_Log_Printf("[\u2192] Resetting RS485 identity...\r\n");
        if (!RS485_Discovery_Slave_ResetIdentity()) {
            USB_Log_Printf("[\u2717] RS485 identity not initialized\r\n");
            return CLI_CMD_ERROR;
        }
        print_rs485_identity(channel);
        USB_Log_Printf("[\u2192] Rebooting to apply new identity...\r\n");
        vTaskDelay(pdMS_TO_TICKS(150));
        watchdog_reboot(0, 0, 0);
        while (1) { }
        return CLI_CMD_OK;  /* not reached */
    }

    print_rs485_identity(channel);
    return CLI_CMD_OK;
}

