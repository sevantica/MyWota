/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 * ******************************************************************************
 */

#ifndef MYWOTA_COMMAND_PROCESSOR_H
#define MYWOTA_COMMAND_PROCESSOR_H

#include "System_Command.h"
#include "CLI_Processor.h"
#include "RS485_Protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unified command execution and dispatching interface
 * @param request Pointer to the command request structure
 * @param response Optional pointer to receive status/message response
 * @return System_Command_Status_t command execution status
 */
System_Command_Status_t MyWota_Command_Processor_Execute(const System_Command_Request_t *request, System_Command_Response_t *response, const CLI_Channel_t* channel);

/**
 * @brief Parses and executes raw USB command line arguments centrally
 * @param argc Count of arguments
 * @param argv String array of arguments
 * @return CLI_Command_Status_t status code for CLI shell
 */
CLI_Command_Status_t MyWota_Command_Processor_ExecuteUSB(const CLI_Channel_t* channel, int argc, char** argv);

/**
 * @brief Parses and executes raw RS485 frames centrally
 * @param rx_frame Pointer to the received RS485 frame
 * @param sequence Sequence number for response frames
 * @return true if command was handled, false if unknown
 */
bool MyWota_Command_Processor_ExecuteRS485(const RS485_Frame_t *rx_frame, uint8_t sequence);

#ifdef __cplusplus
}
#endif

#endif /* MYWOTA_COMMAND_PROCESSOR_H */
