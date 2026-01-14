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

#ifndef MIFARE_VOLUME_ADAPTER_H_
#define MIFARE_VOLUME_ADAPTER_H_

/* Includes ------------------------------------------------------------------*/
#include "MIFARE_Card_Types.h"

/**
 * @brief Initialize Volume adapter for MyWota water dispenser system
 * @details Configures MIFARE core for volume-based operation:
 *          - Continuous balance updates during dispense (100ms intervals)
 *          - Card stays in field during entire transaction
 *          - Fast write mode with final backup write
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_Volume_Adapter_Init(void);

#endif /* MIFARE_VOLUME_ADAPTER_H_ */
