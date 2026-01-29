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

#ifndef APPLICATION_INCLUDE_MYWOTA_CONFIG_ADAPTER_H_
#define APPLICATION_INCLUDE_MYWOTA_CONFIG_ADAPTER_H_

/**
 * @file MyWota_Config_Adapter.h
 * @brief MyWota System Configuration Adapter
 * @details Provides MyWota-specific configuration schema and defaults
 *          using the generic System_Config_Interface
 */

#include "System_Config_Interface.h"
#include "System_Config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MyWota configuration adapter
 * @details Registers the MyWota config schema and defaults with the interface
 * @return CONFIG_INTERFACE_OK on success
 */
Config_Interface_Result_t MyWota_Config_Adapter_Init(void);

/**
 * @brief Get MyWota config schema for external use
 * @param count Output: number of entries
 * @return Pointer to config key entry array
 */
const Config_Key_Entry_t* MyWota_Config_GetSchema(size_t* count);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_INCLUDE_MYWOTA_CONFIG_ADAPTER_H_ */
