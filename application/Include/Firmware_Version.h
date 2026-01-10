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

#ifndef APPLICATION_INCLUDE_FIRMWARE_VERSION_H_
#define APPLICATION_INCLUDE_FIRMWARE_VERSION_H_

/*Includes ----------------------------------------------------------*/
#include <stdint.h>

/*Defines ------------------------------------------------------------*/
#define FW_VERSION_MAJOR        1
#define FW_VERSION_MINOR        2
#define FW_VERSION_PATCH        0
#define FW_VERSION_STRING       "1.2.0"
#define FW_BUILD_DATE           __DATE__
#define FW_BUILD_TIME           __TIME__
#define FW_PRODUCT_NAME         "MyWota Water Dispenser"
#define FW_HARDWARE_VERSION     "Pico W + PN532"

/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Print firmware version information to USB console
 */
void FW_PrintVersionInfo(void);

#endif /* APPLICATION_INCLUDE_FIRMWARE_VERSION_H_ */
