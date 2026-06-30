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

#ifndef APPLICATION_INCLUDE_RS485_COMMAND_ADAPTER_H_
#define APPLICATION_INCLUDE_RS485_COMMAND_ADAPTER_H_

/**
 * @file RS485_Command_Adapter.h
 * @brief MyWota-specific RS485 command handlers
 * @details Implements car wash control and status commands for RS485 protocol
 */

#include "RS485_Command_Interface.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize RS485 command adapter
 * @details Registers MyWota-specific RS485 command handlers with the core RS485 task
 * @return RS485_OK on success
 */
RS485_Result_t RS485_Command_Adapter_Init(void);

bool RS485_Command_Adapter_GetFirmwareProgress(uint8_t *phase_out,
											   uint8_t *progress_percent_out,
											   uint32_t *completed_work_out,
											   uint32_t *total_work_out);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_INCLUDE_RS485_COMMAND_ADAPTER_H_ */
