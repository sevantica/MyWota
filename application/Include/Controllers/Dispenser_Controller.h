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

#ifndef APPLICATION_INCLUDE_DISPENSER_CONTROLLER_H_
#define APPLICATION_INCLUDE_DISPENSER_CONTROLLER_H_

/*Includes ----------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "Application_Interface.h"

/*Defines ------------------------------------------------------------*/
#define DISPENSE_BAY_CONTROL_PIN            15      // GPIO pin for dispense bay control (from Hardware_Access.h)
#define DISPENSE_DURATION_SECONDS           (20 * 60)  // 20 minutes in seconds

/* Water dispenser uses a single valve - no option buttons needed */

/*Typedefs -----------------------------------------------------------*/

/**
 * @brief Valve state for water dispenser
 */
typedef enum {
    VALVE_CLOSED = 0,
    VALVE_OPEN
} ValveState_t;

/**
 * @brief Dispense timer state structure (managed by Dispenser Integration layer)
 * This is NOT stored on the card - it's runtime state only
 * 
 * NEW BEHAVIOR: Card must remain present during dispense
 * - Dispense auto-starts when card is scanned (if balance > 0)
 * - Time is deducted continuously from card while dispensing
 * - Dispense stops immediately when card is removed
 */
typedef struct {
    uint32_t dispense_start_time;          // System tick when dispense started (0 = no active dispense)
    uint32_t last_deduction_time;      // System tick of last balance deduction
    bool dispense_active;                  // true if dispense in progress
    uint8_t dispense_bay_id;               // Which dispense bay is in use
    ValveState_t valve_state;              // Current valve state (open/closed)
    uint32_t balance_deducted_ml;      // Total ml deducted this session (for logging)
} Dispenser_TimerState_t;

/**
 * @brief Dispenser state machine states
 */
typedef enum {
    DISPENSER_IDLE,                            /* No card present */
    DISPENSER_DISPENSE_IN_PROGRESS,            /* Card present and dispensing (deducting time) */
    DISPENSER_WAITING_FOR_REMOVAL              /* Dispense stopped, waiting for card to be removed */
} DispenserState_t;

/**
 * @brief Dispenser operation results
 */
typedef enum {
    DISPENSER_RESULT_OK = 0,
    DISPENSER_RESULT_ERROR,
    DISPENSER_RESULT_NO_CARD,
    DISPENSER_RESULT_CARD_NOT_READY,
    DISPENSER_RESULT_INSUFFICIENT_BALANCE,
    DISPENSER_RESULT_BUSY,
    DISPENSER_RESULT_CARD_ERROR,
    DISPENSER_RESULT_CARD_REMOVED,
    DISPENSER_RESULT_TIMER_ERROR,
    DISPENSER_RESULT_COMPLETE,
    DISPENSER_RESULT_TIMER_EXPIRED
} DispenserResult_t;

/**
 * @brief Dispenser status structure
 */
typedef struct {
    uint8_t state;                      // Current dispense state
    bool dispense_active;               // Dispense in progress
    uint32_t remaining_ml;              // Water remaining on card in milliliters
    bool card_present;                  // Card presence status
    uint32_t balance_ml;                // Card balance in milliliters
    uint8_t dispense_bay_id;            // Which dispense bay is in use
    uint32_t elapsed_ms;                // Time elapsed since dispense started (this session)
    ValveState_t valve_state;           // Current valve state
} DispenserStatus_t;


/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Initialize the dispenser integration system
 * @return DispenserResult_t Initialization result
 */
DispenserResult_t MIFARE_Dispenser_Init(void);

/**
 * @brief Request dispense (scans card, deducts token, starts timer)
 * @return DispenserResult_t Request result
 * @note This function polls MIFARE_Transaction_Manager for card data,
 *       modifies token count, and manages the dispense timer
 */
DispenserResult_t MIFARE_Dispenser_StartDispense(void);

/**
 * @brief Get current dispense status
 * @param status Pointer to status structure
 * @return DispenserResult_t Query result
 */
DispenserResult_t MIFARE_Dispenser_GetStatus(DispenserStatus_t *status);

/**
 * @brief Emergency stop dispense
 * @return DispenserResult_t Stop result
 */
DispenserResult_t MIFARE_Dispenser_EmergencyStop(void);

/**
 * @brief Check if dispense is currently active
 * @return bool true if dispense in progress
 */
bool MIFARE_Dispenser_IsDispenseActive(void);

/**
 * @brief Initialize a new customer card with specified balance
 * @param initial_balance_ml Initial water volume in milliliters
 * @param customer_id Unique customer identifier (0 to auto-generate from card serial)
 * @return DispenserResult_t Operation result
 * @note This polls transaction manager, modifies user data, and writes to card
 */
DispenserResult_t MIFARE_Dispenser_InitializeNewCustomer(uint32_t initial_balance_ml, uint64_t customer_id);

/**
 * @brief Add water volume to an existing customer card (top-up)
 * @param topup_ml Number of milliliters of water to add
 * @return DispenserResult_t Operation result
 * @note This polls transaction manager, increments balance, and writes to card
 */
DispenserResult_t MIFARE_Dispenser_TopupCard(uint32_t topup_ml);

/**
 * @brief Dispenser polling task - manages dispense timer and card updates
 * @note This task polls MIFARE_Transaction_Manager for card data,
 *       manages the 20-minute dispense timer, and updates the card
 */
void MIFARE_Dispenser_Task(void* argument);

/**
 * @brief Start the dispenser polling task
 */
void Task_Start_Dispenser_Task(void);

/**
 * @brief Stop the dispenser polling task
 */
void Task_Stop_Dispenser_Task(void);

/* UI Helper Functions - Business logic layer exposes these for UI to poll */
uint32_t Dispenser_GetBalanceMl(void);            // Get current balance in milliliters from card
uint32_t Dispenser_GetDispenseVolumeRemainingMl(void);  // Get remaining dispense volume in milliliters (same as balance when dispensing)
bool Dispenser_IsDispenseActive(void);            // Returns true if dispense is active
uint32_t Dispenser_GetDispensedAmountML(void);    // Get amount dispensed in current session (ml)
uint32_t Dispenser_GetTotalDispensesCompleted(void); // Lifetime dispenses
uint32_t Dispenser_GetTotalVolumePurchasedMl(void); // Lifetime volume purchased in ml
ValveState_t Dispenser_GetValveState(void);       // Get current valve state (OPEN/CLOSED)
float Dispenser_GetFlowRateLPM(void);             // Get current flow rate in liters per minute

/**
 * @brief Manually start dispense without card (for testing/debugging)
 * @param duration_ml Dispense volume allowance in milliliters (0 = use 20 liters)
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_ManualStart(uint32_t duration_ml);

/**
 * @brief Wait for card validation and dispense up to specified limit
 * @param max_volume_ml Maximum volume to dispense in milliliters
 * @return DispenserResult_t Operation result
 * @note Waits for card to be scanned and validated, then opens valve and dispenses
 *       like normal card operation but stops at max_volume_ml limit.
 */
DispenserResult_t MIFARE_Dispenser_WaitAndDispense(uint32_t max_volume_ml);

/**
 * @brief Manually stop dispense (alias for EmergencyStop)
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_ManualStop(void);

/**
 * @brief Update UI with current card token count
 * @details Updates the cardRemaining UI element with the current token count
 */
void MIFARE_Dispenser_UpdateUI(void);

/**
 * @brief Reset test mode tokens to 10 (for testing)
 * @details Available only when TEST_MODE_ENABLED is defined
 */
void MIFARE_Dispenser_ResetTestMode(void);

/**
 * @brief Get test mode status (for debugging)
 * @details Available only when TEST_MODE_ENABLED is defined
 */
void MIFARE_Dispenser_GetTestModeStatus(void);

/**
 * @brief Get the application interface for dispenser
 * @return Pointer to the dispenser application instance
 */
const Application_Instance_t* Dispenser_GetApplicationInterface(void);

/**
 * @brief Convert DispenserResult_t to Application_Result_t
 * @param result Dispenser result
 * @return Equivalent application result
 */
Application_Result_t Dispenser_ConvertResult(DispenserResult_t result);

/**
 * @brief Get the last error code from the dispenser
 * @return Error code (uint8_t casting of RS485_App_Error_t)
 */
uint8_t Dispenser_GetLastError(void);

#endif /* APPLICATION_INCLUDE_DISPENSER_CONTROLLER_H_ */
