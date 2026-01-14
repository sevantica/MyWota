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

#ifndef APPLICATION_INCLUDE_MYWOTA_IO_EXPANDER_ADAPTER_H_
#define APPLICATION_INCLUDE_MYWOTA_IO_EXPANDER_ADAPTER_H_

/**
 * @file MyWota_IO_Expander_Adapter.h
 * @brief MyWota IO Expander Pin Mapping Adapter
 * @details Provides MyWota-specific CAT9555 pin assignments
 */

#include "IO_Expander_Interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MyWota IO Expander adapter
 * @details Registers the MyWota pin mapping with the interface
 * @return IO_EXP_INTF_OK on success
 */
IO_Expander_Interface_Result_t MyWota_IO_Expander_Adapter_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_INCLUDE_MYWOTA_IO_EXPANDER_ADAPTER_H_ */
