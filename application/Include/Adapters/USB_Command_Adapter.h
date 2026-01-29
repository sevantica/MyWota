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

#ifndef USB_COMMAND_ADAPTER_H_
#define USB_COMMAND_ADAPTER_H_

/* Includes ------------------------------------------------------------------*/
#include "USB_Command_Handler.h"
#include <stdint.h>

/**
 * @brief BigYellow USB Command Adapter
 * @details Provides project-specific USB command implementations for car wash system:
 *          - washstart/washstop commands
 *          - Token-based balance display
 *          - Car wash controller integration
 */

/* Exported types ------------------------------------------------------------*/
typedef struct {
    const char* name;
    USB_Command_Status_t (*handler)(int argc, char** argv);
    const char* description;
    const char* usage;
} USB_Command_Adapter_Entry_t;

/* Exported functions --------------------------------------------------------*/

/**
 * @brief Get project-specific command table
 * @return Pointer to command table array
 */
const USB_Command_Adapter_Entry_t* USB_Command_Adapter_GetCommands(void);

/**
 * @brief Get number of project-specific commands
 * @return Number of commands in adapter table
 */
size_t USB_Command_Adapter_GetCommandCount(void);

/**
 * @brief Print project-specific status information
 * @details Called from cmd_status to display balance and controller state
 */
void USB_Command_Adapter_PrintStatus(void);

/**
 * @brief Get project-specific includes string for display
 * @return String listing project-specific headers
 */
const char* USB_Command_Adapter_GetIncludesInfo(void);

#endif /* USB_COMMAND_ADAPTER_H_ */
