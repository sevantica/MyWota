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

#ifndef APPLICATION_INCLUDE_MYWOTA_HARDWARE_ADAPTER_H_
#define APPLICATION_INCLUDE_MYWOTA_HARDWARE_ADAPTER_H_

/**
 * @file MyWota_Hardware_Adapter.h
 * @brief MyWota Hardware Configuration Adapter
 * @details Provides hardware pin mappings and initialization for MyWota controller
 */

#include "Hardware_Interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MyWota hardware adapter
 * @details Registers MyWota-specific hardware configuration
 * @return HW_INTERFACE_OK on success
 */
HW_Interface_Result_t MyWota_Hardware_Adapter_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_INCLUDE_MYWOTA_HARDWARE_ADAPTER_H_ */
