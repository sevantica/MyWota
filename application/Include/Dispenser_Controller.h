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
#define WASH_BAY_CONTROL_PIN            15      // GPIO pin for wash bay control (from Hardware_Access.h)
#define WASH_DURATION_SECONDS           (20 * 60)  // 20 minutes in seconds

/* Wash Option Button Input Pins (from IO Expander) */
#define WASH_BUTTON_VACUUM_CLEANER      0       // IO0_0 - Active LOW
#define WASH_BUTTON_WASH_BRUSH          1       // IO0_1 - Active LOW
#define WASH_BUTTON_PRESSURE_WASHER     2       // IO0_2 - Active LOW

/* Wash Option Output Control Pins (from IO Expander) */
#define WASH_OUTPUT_VACUUM_CLEANER      9       // IO1_1 - Relay sense 1
#define WASH_OUTPUT_WASH_BRUSH          10      // IO1_2 - Relay sense 2
#define WASH_OUTPUT_PRESSURE_WASHER     14      // IO1_6 - Repurposed Spare Input 1

/*Typedefs -----------------------------------------------------------*/

/**
 * @brief Wash option types
 */
typedef enum {
    WASH_OPTION_NONE = 0,
    WASH_OPTION_VACUUM_CLEANER,
    WASH_OPTION_WASH_BRUSH,
    WASH_OPTION_PRESSURE_WASHER
} WashOption_t;

/**
 * @brief Wash timer state structure (managed by CarWash Integration layer)
 * This is NOT stored on the card - it's runtime state only
 */
typedef struct {
    uint32_t wash_start_time;          // System tick when wash started (0 = no active wash)
    uint32_t wash_duration_seconds;    // Configured wash duration (default 1200 = 20 minutes)
    bool wash_active;                  // true if wash in progress
    uint32_t tokens_used_this_session; // Tokens consumed in current session
    uint8_t wash_bay_id;               // Which wash bay is in use
    bool auto_start_triggered;         // Prevents multiple auto-starts for same card
    WashOption_t selected_option;      // Selected wash option (vacuum/brush/pressure)
    uint32_t card_ready_time;          // System tick when card became ready (for 1s delay)
    uint32_t card_removed_time;        // System tick when card was removed (for 1s wash start delay)
    bool token_deducted;               // Token has been deducted, waiting for removal
} CarWash_TimerState_t;

/**
 * @brief Car wash state machine states
 */
typedef enum {
    CARWASH_IDLE,
    CARWASH_CARD_READY,
    CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL,  /* Token deducted, waiting for card removal */
    CARWASH_WAITING_TO_START,                /* Card removed, waiting 1s before starting wash */
    CARWASH_WASH_IN_PROGRESS
} CarWashState_t;

/**
 * @brief Car wash operation results
 */
typedef enum {
    CARWASH_RESULT_OK = 0,
    CARWASH_RESULT_ERROR,
    CARWASH_RESULT_NO_CARD,
    CARWASH_RESULT_CARD_NOT_READY,
    CARWASH_RESULT_INSUFFICIENT_TOKENS,
    CARWASH_RESULT_BUSY,
    CARWASH_RESULT_CARD_ERROR,
    CARWASH_RESULT_CARD_REMOVED,
    CARWASH_RESULT_TIMER_ERROR,
    CARWASH_RESULT_COMPLETE,
    CARWASH_RESULT_TIMER_EXPIRED
} CarWashResult_t;

/**
 * @brief Car wash status structure
 */
typedef struct {
    uint8_t state;                      // Current wash state
    bool wash_active;                   // Wash in progress
    uint32_t remaining_seconds;         // Time remaining in current wash
    bool card_present;                  // Card presence status
    uint32_t token_count;               // Card token count
    uint8_t wash_bay_id;                // Which wash bay is in use
    uint32_t elapsed_seconds;           // Time elapsed since wash started
    WashOption_t selected_option;       // Selected wash option
} CarWashStatus_t;

/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Initialize the car wash integration system
 * @return CarWashResult_t Initialization result
 */
CarWashResult_t MIFARE_CarWash_Init(void);

/**
 * @brief Request car wash (scans card, deducts token, starts timer)
 * @return CarWashResult_t Request result
 * @note This function polls MIFARE_Transaction_Manager for card data,
 *       modifies token count, and manages the wash timer
 */
CarWashResult_t MIFARE_CarWash_StartWash(void);

/**
 * @brief Get current wash status
 * @param status Pointer to status structure
 * @return CarWashResult_t Query result
 */
CarWashResult_t MIFARE_CarWash_GetStatus(CarWashStatus_t *status);

/**
 * @brief Emergency stop wash
 * @return CarWashResult_t Stop result
 */
CarWashResult_t MIFARE_CarWash_EmergencyStop(void);

/**
 * @brief Check if wash is currently active
 * @return bool true if wash in progress
 */
bool MIFARE_CarWash_IsWashActive(void);

/**
 * @brief Initialize a new customer card with specified tokens
 * @param initial_tokens Initial number of tokens to add to the card
 * @param customer_id Unique customer identifier (0 to auto-generate from card serial)
 * @return CarWashResult_t Operation result
 * @note This polls transaction manager, modifies user data, and writes to card
 */
CarWashResult_t MIFARE_CarWash_InitializeNewCustomer(uint32_t initial_tokens, uint64_t customer_id);

/**
 * @brief Add tokens to an existing customer card (top-up)
 * @param topup_tokens Number of tokens to add to the card
 * @return CarWashResult_t Operation result
 * @note This polls transaction manager, increments token count, and writes to card
 */
CarWashResult_t MIFARE_CarWash_TopupCard(uint32_t topup_tokens);

/**
 * @brief Car wash polling task - manages wash timer and card updates
 * @note This task polls MIFARE_Transaction_Manager for card data,
 *       manages the 20-minute wash timer, and updates the card
 */
void MIFARE_CarWash_Task(void* argument);

/**
 * @brief Start the car wash polling task
 */
void Task_Start_CarWash_Task(void);

/* UI Helper Functions - Business logic layer exposes these for UI to poll */
uint32_t CarWash_GetTokenCount(void);           // Get current token count from card
uint32_t CarWash_GetWashTimeRemaining(void);    // Get remaining wash time in seconds
bool CarWash_IsWashActive(void);                // Returns true if wash is active
uint32_t CarWash_GetTotalWashesCompleted(void); // Lifetime washes
uint32_t CarWash_GetTotalTokensPurchased(void); // Lifetime tokens purchased
WashOption_t CarWash_GetSelectedOption(void);   // Get currently selected wash option

/* Compatibility stub for shared Buzzer_Driver */
uint32_t Dispenser_GetDispensedAmountML(void);  // Stub - car wash doesn't dispense water

/**
 * @brief Manually start wash without card (for testing/debugging)
 * @param option Wash option to activate (or WASH_OPTION_NONE to auto-select)
 * @param duration_seconds Wash duration in seconds (0 = use default)
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_ManualStart(WashOption_t option, uint32_t duration_seconds);

/**
 * @brief Manually stop wash (alias for EmergencyStop)
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_ManualStop(void);

/**
 * @brief Update UI with current card token count
 * @details Updates the cardRemaining UI element with the current token count
 */
void MIFARE_CarWash_UpdateUI(void);

/**
 * @brief Reset test mode tokens to 10 (for testing)
 * @details Available only when TEST_MODE_ENABLED is defined
 */
void MIFARE_CarWash_ResetTestMode(void);

/**
 * @brief Get test mode status (for debugging)
 * @details Available only when TEST_MODE_ENABLED is defined
 */
void MIFARE_CarWash_GetTestModeStatus(void);

/**
 * @brief Get the application interface for car wash
 * @return Pointer to the car wash application instance
 */
const Application_Instance_t* CarWash_GetApplicationInterface(void);

/**
 * @brief Convert CarWashResult_t to Application_Result_t
 * @param result Car wash result
 * @return Equivalent application result
 */
Application_Result_t CarWash_ConvertResult(CarWashResult_t result);

#endif /* APPLICATION_INCLUDE_DISPENSER_CONTROLLER_H_ */