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

#ifndef APPLICATION_INCLUDE_CUSTOMER_CARD_EXAMPLE_H_
#define APPLICATION_INCLUDE_CUSTOMER_CARD_EXAMPLE_H_

/*Includes ----------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "MIFARE_Dispenser_Integration.h"

/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Example function showing how to initialize a new customer card
 * @details This example demonstrates the proper usage of the customer card
 *          initialization function with different scenarios
 */
void CustomerCard_InitializationExample(void);

/**
 * @brief Initialize a card with predefined customer ID
 * @param initial_balance_ml Starting balance in milliliters
 * @param customer_id Specific customer identifier
 * @return DispenserResult_t Operation result
 */
DispenserResult_t CustomerCard_InitializeWithID(uint32_t initial_balance_ml, uint64_t customer_id);

/**
 * @brief Initialize a card with auto-generated customer ID from card serial
 * @param initial_balance_ml Starting balance in milliliters  
 * @return DispenserResult_t Operation result
 */
DispenserResult_t CustomerCard_InitializeAutoID(uint32_t initial_balance_ml);

#endif /* APPLICATION_INCLUDE_CUSTOMER_CARD_EXAMPLE_H_ */