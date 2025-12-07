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

#ifndef APPLICATION_INCLUDE_MIFARE_DISPENSER_INTEGRATION_H_
#define APPLICATION_INCLUDE_MIFARE_DISPENSER_INTEGRATION_H_

/*Includes ----------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>

/*Defines ------------------------------------------------------------*/
#define FLOW_SENSOR_PIN                 22      // GPIO pin for flow sensor
#define VALVE_CONTROL_PIN               15      // GPIO pin for valve control (from Hardware_Access.h)

/*Typedefs -----------------------------------------------------------*/

/**
 * @brief Dispenser operation results
 */
typedef enum {
    DISPENSER_RESULT_OK = 0,
    DISPENSER_RESULT_ERROR,
    DISPENSER_RESULT_NO_CARD,
    DISPENSER_RESULT_CARD_NOT_READY,
    DISPENSER_RESULT_INSUFFICIENT_BALANCE,
    DISPENSER_RESULT_INVALID_AMOUNT,
    DISPENSER_RESULT_BUSY,
    DISPENSER_RESULT_CARD_ERROR,
    DISPENSER_RESULT_CARD_REMOVED,
    DISPENSER_RESULT_SENSOR_ERROR,
    DISPENSER_RESULT_FLOW_ERROR,
    DISPENSER_RESULT_COMPLETE
} DispenserResult_t;

/**
 * @brief Dispenser status structure
 */
typedef struct {
    uint8_t state;                      // Current dispenser state
    uint16_t requested_amount_ml;       // Requested amount in mL
    uint16_t dispensed_amount_ml;       // Amount already dispensed in mL
    bool valve_open;                    // Valve open/closed status
    bool card_present;                  // Card presence status
    uint32_t balance_ml;                // Card balance in mL
    float current_flow_rate_lpm;        // Current flow rate in L/min
    bool flow_detected;                 // Flow detection status
} DispenserStatus_t;

/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Initialize the dispenser integration system
 * @return DispenserResult_t Initialization result
 */
DispenserResult_t MIFARE_Dispenser_Init(void);

/**
 * @brief Request water dispensing
 * @param amount_ml Amount to dispense in milliliters
 * @return DispenserResult_t Request result
 */
DispenserResult_t MIFARE_Dispenser_RequestWater(uint16_t amount_ml);

/**
 * @brief Get current dispensing status
 * @param status Pointer to status structure
 * @return DispenserResult_t Query result
 */
DispenserResult_t MIFARE_Dispenser_GetStatus(DispenserStatus_t *status);

/**
 * @brief Emergency stop dispensing
 * @return DispenserResult_t Stop result
 */
DispenserResult_t MIFARE_Dispenser_EmergencyStop(void);

/**
 * @brief Initialize a new customer card with specified balance
 * @param initial_balance_ml Initial balance to add to the card in milliliters
 * @param customer_id Unique customer identifier (0 to auto-generate from card serial)
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_InitializeNewCustomer(uint32_t initial_balance_ml, uint64_t customer_id);

/**
 * @brief Add balance to an existing customer card (top-up)
 * @param topup_amount_ml Amount to add to the card in milliliters
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_TopupCard(uint32_t topup_amount_ml);

/**
 * @brief Update UI with current card balance
 * @details Updates the cardRemaining UI element with the current card balance
 */
void MIFARE_Dispenser_UpdateUI(void);

/**
 * @brief Reset test mode balance to 100L (for testing)
 * @details Available only when TEST_MODE_ENABLED is defined
 */
void MIFARE_Dispenser_ResetTestMode(void);

/**
 * @brief Get test mode status (for debugging)
 * @details Available only when TEST_MODE_ENABLED is defined
 */
void MIFARE_Dispenser_GetTestModeStatus(void);

#endif /* APPLICATION_INCLUDE_MIFARE_DISPENSER_INTEGRATION_H_ */