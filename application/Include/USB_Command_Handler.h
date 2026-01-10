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

#ifndef APPLICATION_INCLUDE_USB_COMMAND_HANDLER_H_
#define APPLICATION_INCLUDE_USB_COMMAND_HANDLER_H_

/*Includes ----------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"

/*Defines ------------------------------------------------------------*/
#define USB_CMD_MAX_LINE_LENGTH     128
#define USB_CMD_MAX_ARGS            8
#define USB_CMD_PENDING_TIMEOUT_MS  10000  /* Pending commands expire after 10 seconds */

/*Typedefs -----------------------------------------------------------*/
typedef enum {
    USB_CMD_OK = 0,
    USB_CMD_ERROR,
    USB_CMD_ERROR_UNKNOWN_COMMAND,
    USB_CMD_ERROR_INVALID_PARAM,
    USB_CMD_ERROR_NOT_INITIALIZED
} USB_Command_Status_t;

/**
 * @brief Pending card command types
 */
typedef enum {
    USB_PENDING_CMD_NONE = 0,
    USB_PENDING_CMD_CARD_INIT,
    USB_PENDING_CMD_TOPUP,
    USB_PENDING_CMD_RECOVER,        // Recover card from SD log
    USB_PENDING_CMD_DECRYPT_CARD,   // Decrypt card to factory keys
} USB_PendingCommand_t;

/**
 * @brief Pending command state structure
 */
typedef struct {
    USB_PendingCommand_t command;
    uint32_t param_value;           // For topup: ml count; for recover: balance_ml
    uint8_t target_uid[7];          // For recover: target card UID to match
    uint8_t target_uid_length;      // For recover: UID length (4 or 7)
    TickType_t expire_tick;         // When the command expires
    bool active;
} USB_PendingCommandState_t;

/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Start the USB Command Handler Task
 */
void Task_Start_USB_Command_Handler(void);

/**
 * @brief Get status string for USB command status codes
 * @param status Status code
 * @return const char* Status string
 */
const char* USB_Command_GetStatusString(USB_Command_Status_t status);

/**
 * @brief Check if there is a pending card command
 * @return Pointer to pending command state, or NULL if none
 */
USB_PendingCommandState_t* USB_Command_GetPendingCommand(void);

/**
 * @brief Clear the pending card command (call after execution or expiry)
 */
void USB_Command_ClearPendingCommand(void);

/**
 * @brief Execute a pending card command (called by MIFARE task when card is ready)
 * @param pending Pointer to pending command state
 * @return USB_CMD_OK on success, error code otherwise
 */
USB_Command_Status_t USB_Command_ExecutePendingCommand(USB_PendingCommandState_t* pending);

#endif /* APPLICATION_INCLUDE_USB_COMMAND_HANDLER_H_ */
