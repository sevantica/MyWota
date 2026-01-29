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

#ifndef APPLICATION_INCLUDE_SD_LOGGER_FORMAT_ADAPTER_H_
#define APPLICATION_INCLUDE_SD_LOGGER_FORMAT_ADAPTER_H_

/**
 * @file SD_Logger_Format_Adapter.h
 * @brief BigYellow SD Logger Format Adapter
 * @details Implements log formatting for MIFARE transactions and car wash events
 */

#include "SD_Logger_Interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SD Logger format adapter
 * @details Registers BigYellow-specific log formatters
 * @return SD_LOG_OK on success
 */
SD_Log_Result_t SD_Logger_Format_Adapter_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_INCLUDE_SD_LOGGER_FORMAT_ADAPTER_H_ */
