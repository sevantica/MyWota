/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file RS485_Command_Adapter.c
 * @brief BigYellow RS485 Command Handlers
 * @details Implements project-specific RS485 commands for car wash control
 */

/* Includes ------------------------------------------------------------------*/
#include "RS485_Command_Adapter.h"
#include "RS485_Task.h"
#include "RS485_Protocol.h"
#include "Dispenser_Controller.h"
#include "MIFARE_Transaction_Core.h"
#include "PN532_Driver.h"
#include "Application_Interface.h"
#include "USB_Logging.h"
#include "Firmware_Version.h"
#include <string.h>

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_RS485_ADAPTER_EN  1

#if LOG_DEBUG_RS485_ADAPTER_EN
    #define LOG_DEBUG_ADAPTER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_ADAPTER(...)
#endif

/* Private Function Prototypes -----------------------------------------------*/
static bool cmd_poll_status(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_get_device_info(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_get_config(const RS485_Frame_t *rx_frame, uint8_t sequence);

/* Helper Functions */
static void rs485_send_ack(uint8_t sequence);
static void rs485_send_nak(uint8_t sequence, RS485_NAK_Reason_t reason);
static void rs485_send_response(uint8_t sequence, RS485_Command_t command, 
                                 const void* payload, uint16_t length);

/* Command Table -------------------------------------------------------------*/
static const RS485_Command_Entry_t adapter_commands[] = {
    {RS485_CMD_POLL_STATUS,    cmd_poll_status,      "Poll car wash status"},
    {RS485_CMD_GET_INFO,       cmd_get_device_info,  "Get device information"},
    {RS485_CMD_GET_CONFIG,     cmd_get_config,       "Get configuration"},
};

static const RS485_Command_Interface_t command_interface = {
    .commands = adapter_commands,
    .command_count = sizeof(adapter_commands) / sizeof(adapter_commands[0]),
    .project_name = "BigYellow Car Wash"
};

/* Public Functions ----------------------------------------------------------*/

/**
 * @brief Initialize RS485 command adapter
 */
RS485_Result_t RS485_Command_Adapter_Init(void)
{
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Registering %zu BigYellow commands\r\n", 
                      command_interface.command_count);
    
    RS485_Result_t result = RS485_RegisterCommandInterface(&command_interface);
    
    if (result == RS485_OK) {
        LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Commands registered successfully\r\n");
    } else {
        LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Failed to register commands\r\n");
    }
    
    return result;
}

/* Private Functions ---------------------------------------------------------*/

/**
 * @brief Handle POLL_STATUS command
 * @details Returns current car wash status including balance, state, and card info
 */
static bool cmd_poll_status(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    (void)rx_frame;  /* No payload expected */
    
    RS485_Device_Status_t status = {0};
    
    /* Get application instance */
    const Application_Instance_t* app = Application_GetActive();
    
    if (app != NULL && app->callbacks != NULL) {
        /* Fill device type */
        status.device_type = app->type;
        
        /* Get application state */
        if (app->callbacks->get_state != NULL) {
            status.state = app->callbacks->get_state();
        }
        
        /* Get balance */
        if (app->callbacks->get_primary_balance != NULL) {
            status.balance = app->callbacks->get_primary_balance();
        }
        
        /* Check if operation active */
        if (app->callbacks->is_operation_active != NULL) {
            if (app->callbacks->is_operation_active()) {
                status.flags |= RS485_STATUS_FLAG_DISPENSING;
            }
        }
    }
    
    /* Get card information if available */
    if (MIFARE_IsCardReady()) {
        status.flags |= RS485_STATUS_FLAG_CARD_PRESENT;
        
        /* Get card info using existing API */
        PN532_CardInfo_t card_info;
        if (MIFARE_GetCurrentCardInfo(&card_info)) {
            uint8_t uid_len = (card_info.uid_length <= 7) ? card_info.uid_length : 7;
            memcpy(status.card_uid, card_info.uid, uid_len);
            status.card_uid_length = uid_len;
        }
    }
    
    /* Send status response */
    rs485_send_response(sequence, RS485_CMD_STATUS_RESPONSE, 
                        &status, sizeof(RS485_Device_Status_t));
    
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Status sent: state=%d, balance=%lu, card=%s\r\n",
                      status.state, status.balance, 
                      (status.flags & RS485_STATUS_FLAG_CARD_PRESENT) ? "present" : "absent");
    
    return true;
}

/**
 * @brief Handle GET_INFO command
 * @details Returns device information (type, firmware version, serial number)
 */
static bool cmd_get_device_info(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    (void)rx_frame;  /* No payload expected */
    
    RS485_Device_Info_t info = {0};
    
    /* Device type */
    const Application_Instance_t* app = Application_GetActive();
    if (app != NULL) {
        info.device_type = app->type;
    }
    
    /* Firmware version */
    info.fw_version_major = FW_VERSION_MAJOR;
    info.fw_version_minor = FW_VERSION_MINOR;
    info.fw_version_patch = FW_VERSION_PATCH;
    
    /* Serial number (could be from flash or unique ID) */
    info.serial_number = 0x12345678;  /* TODO: Get from unique chip ID */
    
    /* Hardware revision */
    info.hw_revision = 1;  /* TODO: Define hardware revision */
    
    /* Capabilities */
    info.capabilities = 0;
    info.capabilities |= (1 << 0);  /* MIFARE support */
    info.capabilities |= (1 << 1);  /* SD logging support */
    info.capabilities |= (1 << 2);  /* RTC support */
    
    /* Send info response */
    rs485_send_response(sequence, RS485_CMD_GET_INFO, 
                        &info, sizeof(RS485_Device_Info_t));
    
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Device info sent: FW v%d.%d.%d\r\n",
                      info.fw_version_major, info.fw_version_minor, info.fw_version_patch);
    
    return true;
}

/**
 * @brief Handle GET_CONFIG command
 * @details Returns current configuration (prices, timeouts, etc.)
 */
static bool cmd_get_config(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    (void)rx_frame;  /* No payload expected */
    
    /* TODO: Define configuration structure and implement */
    /* For now, just send ACK */
    rs485_send_ack(sequence);
    
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Config request (not implemented)\r\n");
    
    return true;
}

/* Helper Functions ----------------------------------------------------------*/

/**
 * @brief Send ACK response
 */
static void rs485_send_ack(uint8_t sequence)
{
    RS485_Frame_t ack_frame;
    RS485_BuildFrame(&ack_frame, RS485_ADDR_MASTER, RS485_CMD_ACK, sequence, NULL, 0);
    RS485_SendFrame(&ack_frame);
}

/**
 * @brief Send NAK response
 */
static void rs485_send_nak(uint8_t sequence, RS485_NAK_Reason_t reason)
{
    RS485_Frame_t nak_frame;
    uint8_t payload = (uint8_t)reason;
    RS485_BuildFrame(&nak_frame, RS485_ADDR_MASTER, RS485_CMD_NAK, sequence, &payload, 1);
    RS485_SendFrame(&nak_frame);
}

/**
 * @brief Send response with payload
 */
static void rs485_send_response(uint8_t sequence, RS485_Command_t command,
                                 const void* payload, uint16_t length)
{
    RS485_Frame_t response_frame;
    RS485_BuildFrame(&response_frame, RS485_ADDR_MASTER, command, sequence, payload, length);
    RS485_SendFrame(&response_frame);
}
