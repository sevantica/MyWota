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

#ifndef MYWOTA_CONFIG_ADAPTER_H_
#define MYWOTA_CONFIG_ADAPTER_H_

#include "System_Config.h"
#include "System_Config_Interface.h"

/* Public Functions ----------------------------------------------------------*/

Config_Interface_Result_t MyWota_Config_Adapter_Init(void);
const Config_Key_Entry_t* MyWota_Config_GetSchema(size_t* count);

#endif /* MYWOTA_CONFIG_ADAPTER_H_ */
