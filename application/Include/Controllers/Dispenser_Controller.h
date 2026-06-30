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
#include "FreeRTOS.h"
#include "task.h"
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
    bool transaction_counted;          // True after transaction_counter incremented this session
} Dispenser_TimerState_t;

/**
 * @brief Dispenser state machine states
 */
typedef enum {
    DISPENSER_IDLE,                            /* No card present */
    DISPENSER_DISPENSE_IN_PROGRESS,            /* Card present and dispensing (deducting time) */
    DISPENSER_WAITING_FOR_REMOVAL,             /* Dispense stopped, waiting for card to be removed */
    DISPENSER_SELF_CLEANING                    /* CCH-commanded self-clean cycle in progress */
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
 * @note This function reacts to the MIFARE transaction snapshot,
 *       modifies volume balance, and manages the dispense timer.
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
 * @note Updates the current transaction snapshot, modifies user data, and writes to card.
 */
DispenserResult_t MIFARE_Dispenser_InitializeNewCustomer(uint32_t initial_balance_ml, uint64_t customer_id);

/**
 * @brief Add water volume to an existing customer card (top-up)
 * @param topup_ml Number of milliliters of water to add
 * @return DispenserResult_t Operation result
 * @note Updates the current transaction snapshot, increments balance, and writes to card.
 */
DispenserResult_t MIFARE_Dispenser_TopupCard(uint32_t topup_ml);

/**
 * @brief Start the dispenser event/status service task
 */
void Task_Start_Dispenser_Task(void);

/**
 * @brief Stop the dispenser event/status service task
 */
void Task_Stop_Dispenser_Task(void);

/**
 * @brief Get dispenser task handle.
 */
TaskHandle_t Dispenser_Task_GetHandle(void);

/* Legacy snapshot helpers for command/RS485 adapters. UI consumes events. */
uint32_t Dispenser_GetBalanceMl(void);            // Get current balance in milliliters from card
uint32_t Dispenser_GetDispenseVolumeRemainingMl(void);  // Get remaining dispense volume in milliliters (same as balance when dispensing)
bool Dispenser_IsDispenseActive(void);            // Returns true if dispense is active
uint32_t Dispenser_GetDispensedAmountML(void);    // Get amount dispensed in current session (ml)
uint32_t Dispenser_GetTotalDispensesCompleted(void); // Lifetime dispenses
uint32_t Dispenser_GetTotalVolumePurchasedMl(void); // Lifetime volume purchased in ml
ValveState_t Dispenser_GetValveState(void);       // Get current valve state (OPEN/CLOSED)
float Dispenser_GetFlowRateLPM(void);             // Get current flow rate in liters per minute
uint8_t Dispenser_GetLastError(void);              // Get last RS485_App_Error_t value
bool Dispenser_IsCardPresent(void);
bool Dispenser_IsCardReady(void);
bool Dispenser_GetCardUID(uint8_t *uid_out, uint8_t *len_out);

/**
 * @brief Sample the slave's flow diagnostics ring (called by RS485 status responder).
 * @details Maintains an internal 8-deep ring of recent flow samples. Each call
 *          advances the ring with the current instantaneous flow rate, then
 *          returns the latest value plus the min/max across the ring. Units
 *          are centiLitres / minute (cL/min); divide by 100 for L/min.
 * @param[out] flow_clpm     Latest instantaneous flow (cL/min). May be NULL.
 * @param[out] flow_clpm_min Min value in the ring (cL/min). May be NULL.
 * @param[out] flow_clpm_max Max value in the ring (cL/min). May be NULL.
 */
void Dispenser_SampleFlowDiagnostics(uint16_t *flow_clpm,
                                     uint16_t *flow_clpm_min,
                                     uint16_t *flow_clpm_max);

/**
 * @brief Returns true while the valve is commanded open (i.e. the controller
 *        wants water to flow). Mirrors `Dispenser_GetValveState() == VALVE_OPEN`.
 */
bool Dispenser_IsValveCommanded(void);

/**
 * @brief Get this dispenser's current pump request for the CCH master.
 * @details MyWota dispensers only ever request a BOOSTER pump (never a
 *          pressure washer). The request is asserted from "card validated
 *          with balance" through dispense completion, and aggregated by
 *          the master across all slaves so the pump only stops when no
 *          dispenser still needs it.
 * @param[out] out_pump_id    RS485_Peripheral_ID_t. RS485_PERIPHERAL_NONE if not requesting.
 * @param[out] out_level      0 = off, 255 = max. >0 means pump ON.
 */
void Dispenser_GetPeripheralRequest(uint8_t *out_pump_id, uint8_t *out_level);

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

/* ===========================================================================
 * Self-Clean (CCH-orchestrated periodic flush)
 * ===========================================================================
 *
 * The CCH master commands one slave at a time to self-clean so the cleaning
 * valve gets full booster pressure (no concurrent cleans). The slave opens
 * its valve until either the target volume has flowed (per the YS-S201 flow
 * sensor) or max_duration_sec elapses, then closes the valve and persists
 * the new last-clean timestamp to flash.
 */

/**
 * @brief Begin a self-clean cycle.
 * @param volume_ml          Target volume to flush (mL). 0 uses config default.
 * @param max_duration_sec   Hard time cap (seconds). 0 uses config default.
 * @return DISPENSER_RESULT_OK if started, DISPENSER_RESULT_BUSY if not idle,
 *         DISPENSER_RESULT_ERROR otherwise.
 * @note Refused if a card is currently present or a dispense is active.
 */
DispenserResult_t Dispenser_StartSelfClean(uint32_t volume_ml, uint32_t max_duration_sec);

/**
 * @brief Abort an in-progress self-clean cycle (closes valve immediately).
 */
void Dispenser_StopSelfClean(void);

/**
 * @brief Check whether a self-clean cycle is currently running.
 */
bool Dispenser_IsSelfCleaning(void);

/**
 * @brief Get the timestamp (RTC unix seconds) of the last successful clean.
 * @return 0 if never cleaned.
 */
uint32_t Dispenser_GetLastCleanUnixTime(void);

/**
 * @brief Get current dispenser state (legacy RS485 snapshot reporting).
 */
DispenserState_t Dispenser_GetControllerState(void);

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

void Dispenser_ClearLastError(void);

#endif /* APPLICATION_INCLUDE_DISPENSER_CONTROLLER_H_ */
