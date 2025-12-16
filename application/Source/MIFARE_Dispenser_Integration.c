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

/**
 * @file MIFARE_Dispenser_Integration.c
 * @brief Example integration of MIFARE transaction manager with water dispenser
 * @details Demonstrates safe dispensing with continuous card monitoring and
 *          automatic rollback on card removal
 */

/*Includes ----------------------------------------------------------*/
#include <string.h>
#include <stdio.h>
#include "MIFARE_Transaction_Manager.h"
#include "MIFARE_Dispenser_Integration.h"
#include "YS_S201_Driver.h"
#include "USB_Logging.h"
#include "mywota_ui_driver.h"
#include "PN532_Driver.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "ui.h"
#include "ui_Screen1.h"

/*Private defines ---------------------------------------------------*/
#define DISPENSER_TASK_STACK_SIZE       (configMINIMAL_STACK_SIZE * 6)
#define DISPENSER_TASK_PRIORITY         (tskIDLE_PRIORITY + 3)
#define DISPENSER_UPDATE_INTERVAL_MS    (100)    // Check dispenser every 100ms
#define DISPENSER_MAX_FLOW_RATE_LPM     (5.0f)   // Maximum 5 L/min
#define DISPENSER_MIN_DISPENSE_ML       (50)     // Minimum 50mL per transaction
#define DISPENSER_MAX_DISPENSE_ML       (5000)   // Maximum 5L per transaction

/* Test Mode Configuration */
#define TEST_MODE_ENABLED               (1)      // Enable test mode (set to 0 to disable)
#define TEST_MODE_INITIAL_BALANCE_ML    (100000) // 100L initial balance for test mode
#define TEST_MODE_SIMULATED_FLOW_LPM    (20.0f)  // 20L/min simulated flow rate
#define TEST_MODE_CUSTOMER_ID           (0x1234567890ABCDEFULL) // Test customer ID

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_MIFARE_DISPENSER_INTEGRATION_EN      1
#define LOG_CRITICAL_MIFARE_DISPENSER_INTEGRATION_EN   1
#define LOG_ERROR_MIFARE_DISPENSER_INTEGRATION_EN      1

#if LOG_DEBUG_MIFARE_DISPENSER_INTEGRATION_EN
    #define LOG_DEBUG_MIFARE_DISPENSER_INTEGRATION(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_MIFARE_DISPENSER_INTEGRATION(...)
#endif

#if LOG_CRITICAL_MIFARE_DISPENSER_INTEGRATION_EN
    #define LOG_CRITICAL_MIFARE_DISPENSER_INTEGRATION(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_MIFARE_DISPENSER_INTEGRATION(...)
#endif

#if LOG_ERROR_MIFARE_DISPENSER_INTEGRATION_EN
    #define LOG_ERROR_MIFARE_DISPENSER_INTEGRATION(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_MIFARE_DISPENSER_INTEGRATION(...)
#endif

/*Private typedefs --------------------------------------------------*/
typedef enum {
    DISPENSER_IDLE = 0,
    DISPENSER_CARD_READY,
    DISPENSER_USER_REQUESTED,
    DISPENSER_DISPENSING,
    DISPENSER_COMPLETING,
    DISPENSER_ERROR
} DispenserState_t;

typedef struct {
    DispenserState_t state;
    uint16_t requested_amount_ml;
    uint16_t dispensed_amount_ml;
    uint32_t dispense_start_time;
    bool valve_open;
    YS_S201_Handle_t flow_sensor;
    
    /* Test Mode Variables */
    bool test_mode_active;
    bool test_card_present;
    uint32_t test_balance_ml;
    uint32_t test_last_topup_ml;
    uint32_t test_simulated_dispensed_ml;
    uint32_t test_last_update_time;
    bool test_dispensing_active;
} DispenserHandle_t;

/*Private variables -------------------------------------------------*/
static TaskHandle_t dispenser_task_handle = NULL;
static DispenserHandle_t dispenser_handle;
static TimerHandle_t dispenser_safety_timer = NULL;

/*Private function prototypes -----------------------------------*/
static void DispenserTask(void *pvParameters);
static void DispenserSafetyTimerCallback(TimerHandle_t timer);
static DispenserResult_t StartDispensing(uint16_t amount_ml);
static DispenserResult_t StopDispensing(void);
static DispenserResult_t UpdateDispensingProgress(void);
static void SetValveState(bool open);
static const char* GetDispenserStateString(DispenserState_t state);
static void UpdateCardUI(uint32_t current_balance_ml, uint32_t last_topup_amount_ml);

/* Test Mode Functions */
#if TEST_MODE_ENABLED
static void InitializeTestMode(void);
static void ProcessTestModeCardDetection(void);
static DispenserResult_t StartTestModeDispensing(uint16_t amount_ml);
static DispenserResult_t UpdateTestModeProgress(void);
static void StopTestModeDispensing(void);
static void SimulateCardInsertion(void);
static void SimulateCardRemoval(void);
#endif

/*Public Functions ----------------------------------------------*/

/**
 * @brief Initialize the dispenser integration system
 * @return DispenserResult_t Initialization result
 */
DispenserResult_t MIFARE_Dispenser_Init(void)
{
    // Initialize dispenser handle
    memset(&dispenser_handle, 0, sizeof(DispenserHandle_t));
    dispenser_handle.state = DISPENSER_IDLE;
    
#if TEST_MODE_ENABLED
    // Initialize test mode
    InitializeTestMode();
    LOG_CRITICAL_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Test mode enabled - Initial balance: %u mL (%.1f L)\r\n", 
                   TEST_MODE_INITIAL_BALANCE_ML, TEST_MODE_INITIAL_BALANCE_ML / 1000.0f);
#endif
    
    // Initialize flow sensor
    YS_S201_Status_t flow_status = YS_S201_Init(&dispenser_handle.flow_sensor, FLOW_SENSOR_PIN);
    if (flow_status != YS_S201_OK) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Failed to initialize flow sensor: %s\r\n", 
                       YS_S201_GetStatusString(flow_status));
        return DISPENSER_RESULT_ERROR;
    }
    
    // Start flow monitoring
    flow_status = YS_S201_Start(&dispenser_handle.flow_sensor);
    if (flow_status != YS_S201_OK) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Failed to start flow monitoring\r\n");
        return DISPENSER_RESULT_ERROR;
    }
    
    // Create safety timer (30 second maximum dispense time)
    dispenser_safety_timer = xTimerCreate(
        "DispenserSafety",
        pdMS_TO_TICKS(30000),
        pdFALSE,  // One-shot timer
        NULL,
        DispenserSafetyTimerCallback
    );
    
    if (dispenser_safety_timer == NULL) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Failed to create safety timer\r\n");
        return DISPENSER_RESULT_ERROR;
    }
    
    // Create dispenser task
    BaseType_t result = xTaskCreate(
        DispenserTask,
        "DispenserTask",
        DISPENSER_TASK_STACK_SIZE,
        NULL,
        DISPENSER_TASK_PRIORITY,
        &dispenser_task_handle
    );
    
    if (result != pdPASS) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Failed to create dispenser task\r\n");
        return DISPENSER_RESULT_ERROR;
    }
    
    LOG_CRITICAL_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Initialization successful\r\n");
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Request water dispensing
 * @param amount_ml Amount to dispense in milliliters
 * @return DispenserResult_t Request result
 */
DispenserResult_t MIFARE_Dispenser_RequestWater(uint16_t amount_ml)
{
    // Validate amount
    if (amount_ml < DISPENSER_MIN_DISPENSE_ML || amount_ml > DISPENSER_MAX_DISPENSE_ML) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Invalid amount requested: %u mL\r\n", amount_ml);
        return DISPENSER_RESULT_INVALID_AMOUNT;
    }
    
    // Check if card is present and ready
    if (!MIFARE_IsCardPresent()) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: No card present\r\n");
        return DISPENSER_RESULT_NO_CARD;
    }
    
    MIFARE_DispenseState_t mifare_state = MIFARE_GetDispenseState();
    if (mifare_state != DISPENSE_STATE_READY_TO_DISPENSE) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Card not ready for dispensing (state: %s)\r\n", 
                       MIFARE_GetStateString(mifare_state));
        return DISPENSER_RESULT_CARD_NOT_READY;
    }
    
    // Check sufficient balance
    uint32_t balance = MIFARE_GetBalanceML();
    if (balance < amount_ml) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Insufficient balance: %lu mL available, %u mL requested\r\n", 
                       balance, amount_ml);
        return DISPENSER_RESULT_INSUFFICIENT_BALANCE;
    }
    
    // Check dispenser state
    if (dispenser_handle.state != DISPENSER_CARD_READY) {
        LOG_ERROR_MIFARE_DISPENSER_INTEGRATION("DISPENSER: Dispenser not ready (state: %s)\r\n", 
                       GetDispenserStateString(dispenser_handle.state));
        return DISPENSER_RESULT_BUSY;
    }
    
    // Set request
    dispenser_handle.requested_amount_ml = amount_ml;
    dispenser_handle.state = DISPENSER_USER_REQUESTED;
    
    USB_Log_Printf("DISPENSER: Water dispense requested: %u mL\r\n", amount_ml);
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Get current dispensing status
 * @param status Pointer to status structure
 * @return DispenserResult_t Query result
 */
DispenserResult_t MIFARE_Dispenser_GetStatus(DispenserStatus_t *status)
{
    if (status == NULL) {
        return DISPENSER_RESULT_ERROR;
    }
    
    status->state = dispenser_handle.state;
    status->requested_amount_ml = dispenser_handle.requested_amount_ml;
    status->dispensed_amount_ml = dispenser_handle.dispensed_amount_ml;
    status->valve_open = dispenser_handle.valve_open;
    status->card_present = MIFARE_IsCardPresent();
    status->balance_ml = MIFARE_GetBalanceML();
    
    // Get flow data
    YS_S201_FlowData_t flow_data;
    YS_S201_Status_t flow_status = YS_S201_GetFlowData(&dispenser_handle.flow_sensor, &flow_data);
    if (flow_status == YS_S201_OK) {
        status->current_flow_rate_lpm = flow_data.flow_rate_lpm;
        status->flow_detected = flow_data.flow_detected;
    } else {
        status->current_flow_rate_lpm = 0.0f;
        status->flow_detected = false;
    }
    
    return DISPENSER_RESULT_OK;
}

/*Private Functions ---------------------------------------------*/

/**
 * @brief Main dispenser task
 * @param pvParameters Task parameters (unused)
 */
static void DispenserTask(void *pvParameters)
{
    (void)pvParameters;
    
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t task_period = pdMS_TO_TICKS(DISPENSER_UPDATE_INTERVAL_MS);
    
    USB_Log_Printf("DISPENSER: Task started\r\n");
    
    while (1) {
#if TEST_MODE_ENABLED
        // Process test mode card detection and simulation
        if (dispenser_handle.test_mode_active) {
            ProcessTestModeCardDetection();
        }
#endif
        
        // Monitor MIFARE state and update dispenser state accordingly
        MIFARE_DispenseState_t mifare_state = MIFARE_GetDispenseState();
        
        switch (dispenser_handle.state) {
            case DISPENSER_IDLE:
                if (mifare_state == DISPENSE_STATE_READY_TO_DISPENSE) {
                    dispenser_handle.state = DISPENSER_CARD_READY;
                    USB_Log_Printf("DISPENSER: Card ready for dispensing\r\n");
                    
                    // Update UI with current card balance and percentage bar
                    uint32_t balance_ml = MIFARE_GetBalanceML();
                    uint32_t last_topup_ml = MIFARE_GetLastTopupAmountML();
                    UpdateCardUI(balance_ml, last_topup_ml);
                }
                break;
                
            case DISPENSER_CARD_READY:
                if (mifare_state != DISPENSE_STATE_READY_TO_DISPENSE) {
                    dispenser_handle.state = DISPENSER_IDLE;
                    USB_Log_Printf("DISPENSER: Card no longer ready\r\n");
                    
                    // Clear UI display when card is removed
                    UpdateCardUI(0, 0);
                }
                // State can also transition to USER_REQUESTED via API call
                break;
                
            case DISPENSER_USER_REQUESTED:
                // Start dispensing transaction
                if (StartDispensing(dispenser_handle.requested_amount_ml) == DISPENSER_RESULT_OK) {
                    dispenser_handle.state = DISPENSER_DISPENSING;
                } else {
                    dispenser_handle.state = DISPENSER_ERROR;
                }
                break;
                
            case DISPENSER_DISPENSING:
                // Monitor dispensing progress
                DispenserResult_t update_result = UpdateDispensingProgress();
                
                if (update_result == DISPENSER_RESULT_COMPLETE) {
                    dispenser_handle.state = DISPENSER_COMPLETING;
                } else if (update_result != DISPENSER_RESULT_OK) {
                    USB_Log_Printf("DISPENSER: Error during dispensing\r\n");
                    StopDispensing();
                    dispenser_handle.state = DISPENSER_ERROR;
                }
                
                // Check for card removal
                if (mifare_state == DISPENSE_STATE_CARD_REMOVED) {
                    USB_Log_Printf("DISPENSER: EMERGENCY VALVE CLOSE - Card removed during dispensing\r\n");
                    StopDispensing();
                    dispenser_handle.state = DISPENSER_ERROR;
                }
                break;
                
            case DISPENSER_COMPLETING:
                // Finalize transaction
                StopDispensing();
                MIFARE_CommitTransaction();
                USB_Log_Printf("DISPENSER: Dispensing completed successfully - %u mL dispensed\r\n", 
                               dispenser_handle.dispensed_amount_ml);
                dispenser_handle.state = DISPENSER_CARD_READY;
                
                // Update UI with new card balance and percentage after dispensing
                uint32_t new_balance_ml = MIFARE_GetBalanceML();
                uint32_t last_topup_ml = MIFARE_GetLastTopupAmountML();
                UpdateCardUI(new_balance_ml, last_topup_ml);
                break;
                
            case DISPENSER_ERROR:
                // Error recovery
                StopDispensing();
                if (mifare_state == DISPENSE_STATE_READY_TO_DISPENSE) {
                    dispenser_handle.state = DISPENSER_CARD_READY;
                } else {
                    dispenser_handle.state = DISPENSER_IDLE;
                }
                break;
        }
        
        vTaskDelayUntil(&last_wake_time, task_period);
    }
}

/**
 * @brief Start dispensing operation
 * @param amount_ml Amount to dispense
 * @return DispenserResult_t Start result
 */
static DispenserResult_t StartDispensing(uint16_t amount_ml)
{
    // Begin MIFARE transaction
    MIFARE_Result_t mifare_result = MIFARE_BeginTransaction(amount_ml);
    if (mifare_result != MIFARE_RESULT_OK) {
        USB_Log_Printf("DISPENSER: Failed to begin MIFARE transaction: %s\r\n", 
                       MIFARE_GetResultString(mifare_result));
        return DISPENSER_RESULT_CARD_ERROR;
    }
    
    // Reset flow sensor total
    YS_S201_ResetTotalVolume(&dispenser_handle.flow_sensor);
    
    // Open dispenser valve
    SetValveState(true);
    
    // Start safety timer
    xTimerStart(dispenser_safety_timer, 0);
    
    // Initialize dispensing tracking
    dispenser_handle.dispensed_amount_ml = 0;
    dispenser_handle.dispense_start_time = (uint32_t)xTaskGetTickCount();
    
    USB_Log_Printf("DISPENSER: Dispensing started - Target: %u mL\r\n", amount_ml);
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Update dispensing progress
 * @return DispenserResult_t Update result
 */
static DispenserResult_t UpdateDispensingProgress(void)
{
    // Get current flow data
    YS_S201_FlowData_t flow_data;
    YS_S201_Status_t flow_status = YS_S201_GetFlowData(&dispenser_handle.flow_sensor, &flow_data);
    
    if (flow_status != YS_S201_OK) {
        USB_Log_Printf("DISPENSER: Failed to read flow sensor\r\n");
        return DISPENSER_RESULT_SENSOR_ERROR;
    }
    
    // Calculate dispensed amount (convert liters to mL)
    uint16_t current_dispensed_ml = (uint16_t)(flow_data.total_volume_ml);
    
    // Check if we've dispensed more since last update
    if (current_dispensed_ml > dispenser_handle.dispensed_amount_ml) {
        uint16_t additional_ml = current_dispensed_ml - dispenser_handle.dispensed_amount_ml;
        
        // Update MIFARE transaction with progress
        MIFARE_Result_t mifare_result = MIFARE_UpdateTransactionProgress(additional_ml, flow_data.flow_rate_lpm);
        if (mifare_result != MIFARE_RESULT_OK) {
            USB_Log_Printf("DISPENSER: MIFARE update failed: %s\r\n", 
                           MIFARE_GetResultString(mifare_result));
            
            if (mifare_result == MIFARE_RESULT_CARD_REMOVED) {
                return DISPENSER_RESULT_CARD_REMOVED;
            }
            return DISPENSER_RESULT_CARD_ERROR;
        }
        
        dispenser_handle.dispensed_amount_ml = current_dispensed_ml;
        
        USB_Log_Printf("DISPENSER: Progress - %u/%u mL (%.1f L/min)\r\n", 
                       current_dispensed_ml, dispenser_handle.requested_amount_ml,
                       flow_data.flow_rate_lpm);
    }
    
    // Check if target amount reached
    if (dispenser_handle.dispensed_amount_ml >= dispenser_handle.requested_amount_ml) {
        USB_Log_Printf("DISPENSER: Target amount reached\r\n");
        return DISPENSER_RESULT_COMPLETE;
    }
    
    // Check for flow rate limits
    if (flow_data.flow_rate_lpm > DISPENSER_MAX_FLOW_RATE_LPM) {
        USB_Log_Printf("DISPENSER: Flow rate too high: %.2f L/min\r\n", flow_data.flow_rate_lpm);
        return DISPENSER_RESULT_FLOW_ERROR;
    }
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Stop dispensing operation
 * @return DispenserResult_t Stop result
 */
static DispenserResult_t StopDispensing(void)
{
    // Close valve immediately
    SetValveState(false);
    
    // Stop safety timer
    xTimerStop(dispenser_safety_timer, 0);
    
    USB_Log_Printf("DISPENSER: Dispensing stopped - valve closed\r\n");
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Control dispenser valve
 * @param open True to open valve, false to close valve
 */
static void SetValveState(bool open)
{
    dispenser_handle.valve_open = open;
    
    // Control the valve using GPIO 15 (VALVE_CONTROL_PIN)
    // Note: This assumes direct GPIO control. If using the CAT9555 expander,
    // you would use: io_driver_set_state(RELAY_CONTROL_0_POS, open ? GPIO_PIN_SET : GPIO_PIN_RESET, dispenser_task_handle);
    // gpio_put(VALVE_CONTROL_PIN, open);
    
    USB_Log_Printf("DISPENSER: Valve %s (GPIO %d)\r\n", open ? "OPEN" : "CLOSED", VALVE_CONTROL_PIN);
}

/**
 * @brief Safety timer callback - emergency stop
 * @param timer Timer handle
 */
static void DispenserSafetyTimerCallback(TimerHandle_t timer)
{
    (void)timer;
    
    USB_Log_Printf("DISPENSER: SAFETY TIMEOUT - Emergency valve close activated\r\n");
    
    // Force stop dispensing (closes valve)
    StopDispensing();
    dispenser_handle.state = DISPENSER_ERROR;
    
    // Rollback MIFARE transaction
    MIFARE_RollbackTransaction();
}

/**
 * @brief Get string representation of dispenser state
 * @param state Dispenser state
 * @return const char* State string
 */
static const char* GetDispenserStateString(DispenserState_t state)
{
    switch (state) {
        case DISPENSER_IDLE:            return "Idle";
        case DISPENSER_CARD_READY:      return "Card Ready";
        case DISPENSER_USER_REQUESTED:  return "User Requested";
        case DISPENSER_DISPENSING:      return "Dispensing";
        case DISPENSER_COMPLETING:      return "Completing";
        case DISPENSER_ERROR:           return "Error";
        default:                        return "Unknown";
    }
}

/**
 * @brief Initialize a new customer card with specified balance
 * @param initial_balance_ml Initial balance to add to the card in milliliters
 * @param customer_id Unique customer identifier (0 to auto-generate from card serial)
 * @return DispenserResult_t Operation result
 * 
 * @details This function provides a high-level interface to initialize a blank
 *          MIFARE card for a new customer. If customer_id is 0, it will be
 *          automatically generated from the card's serial number.
 */
DispenserResult_t MIFARE_Dispenser_InitializeNewCustomer(uint32_t initial_balance_ml, uint64_t customer_id)
{
    if (dispenser_handle.state != DISPENSER_IDLE) {
        USB_Log_Printf("DISPENSER: Cannot initialize card - system not idle (state: %s)\r\n", 
                       GetDispenserStateString(dispenser_handle.state));
        return DISPENSER_RESULT_BUSY;
    }
    
    USB_Log_Printf("DISPENSER: Initializing new customer card with %u mL balance\r\n", initial_balance_ml);
    
    // Temporarily change state to prevent other operations
    dispenser_handle.state = DISPENSER_USER_REQUESTED;
    
    // If customer_id is 0, we'll let the MIFARE manager use the card serial
    if (customer_id == 0) {
        // Try to detect card first to get its serial number
        PN532_CardInfo_t card_info;
        PN532_Status_t status = PN532_DetectCard(&card_info);
        if (status == PN532_STATUS_CARD_DETECTED && card_info.uid_length >= 4) {
            // Use the card UID as customer ID
            customer_id = 0;
            for (uint8_t i = 0; i < card_info.uid_length && i < 8; i++) {
                customer_id |= ((uint64_t)card_info.uid[i]) << (i * 8);
            }
            USB_Log_Printf("DISPENSER: Auto-generated customer ID: %llu from card UID\r\n", customer_id);
        } else {
            USB_Log_Printf("DISPENSER: No card detected for initialization\r\n");
            dispenser_handle.state = DISPENSER_IDLE;
            return DISPENSER_RESULT_NO_CARD;
        }
    }
    
    // Call the MIFARE transaction manager to initialize the card
    MIFARE_Result_t result = MIFARE_InitializeNewCustomerCard(initial_balance_ml, customer_id);
    
    DispenserResult_t dispenser_result;
    switch (result) {
        case MIFARE_RESULT_OK:
            USB_Log_Printf("DISPENSER: Card initialization successful\r\n");
            USB_Log_Printf("DISPENSER: Customer ID: %llu, Balance: %u mL\r\n", customer_id, initial_balance_ml);
            dispenser_handle.state = DISPENSER_CARD_READY;
            dispenser_result = DISPENSER_RESULT_OK;
            
            // Update UI with the initial balance and 100% bar (full top-up)
            UpdateCardUI(initial_balance_ml, initial_balance_ml);
            break;
            
        case MIFARE_RESULT_ERROR:
            USB_Log_Printf("DISPENSER: Card initialization failed - general error\r\n");
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_ERROR;
            break;
            
        case MIFARE_RESULT_AUTHENTICATION_FAILED:
            USB_Log_Printf("DISPENSER: Card initialization failed - authentication error\r\n");
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_CARD_ERROR;
            break;
            
        case MIFARE_RESULT_WRITE_FAILED:
            USB_Log_Printf("DISPENSER: Card initialization failed - write error\r\n");
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_CARD_ERROR;
            break;
            
        case MIFARE_RESULT_DATA_MISMATCH:
            USB_Log_Printf("DISPENSER: Card initialization failed - verification error\r\n");
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_CARD_ERROR;
            break;
            
        default:
            USB_Log_Printf("DISPENSER: Card initialization failed - unknown error (%d)\r\n", result);
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_ERROR;
            break;
    }
    
    // If initialization failed, return to idle state after a brief delay
    if (dispenser_result != DISPENSER_RESULT_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Give time for error logging
        dispenser_handle.state = DISPENSER_IDLE;
    }
    
    return dispenser_result;
}

/**
 * @brief Update both card balance display and remaining percentage bar
 * @param current_balance_ml Current card balance in milliliters
 * @param last_topup_amount_ml Last top-up amount for percentage calculation
 * @details Helper function to update both UI elements together for consistency
 */
static void UpdateCardUI(uint32_t current_balance_ml, uint32_t last_topup_amount_ml)
{
    // Update card remaining balance
    static char balance_str[16];
    if (current_balance_ml > 9000) {
        uint32_t liters = current_balance_ml / 1000;
        snprintf(balance_str, sizeof(balance_str), "%luL", liters);
    } else {
        snprintf(balance_str, sizeof(balance_str), "%luml", current_balance_ml);
    }
    ui_set_label_text(ui_cardRemaining, balance_str);
    
    // Update total remaining bar
    uint8_t percentage = 0;
    if (last_topup_amount_ml > 0) {
        if (current_balance_ml >= last_topup_amount_ml) {
            percentage = 100;
        } else {
            percentage = (uint8_t)((current_balance_ml * 100) / last_topup_amount_ml);
        }
    }
    ui_set_bar_value(ui_totalRemainingBar, percentage, LV_ANIM_ON);
    
    // Update level color indicator
    if (ui_levelColourIndicator != NULL) {
        if (percentage > 25) {
            ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0x05820A), LV_PART_MAIN | LV_STATE_DEFAULT);
        } else if (percentage > 10) {
            ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

/**
 * @brief Update UI with current card balance if a card is present
 * @details This function can be called periodically to ensure the UI stays
 *          synchronized with the actual card balance and percentage bar
 */
void MIFARE_Dispenser_UpdateUI(void)
{
    // Only update UI if a card is ready
    if (dispenser_handle.state == DISPENSER_CARD_READY || 
        dispenser_handle.state == DISPENSER_DISPENSING) {
        
        // Check if MIFARE system has a valid card
        if (MIFARE_IsCardPresent()) {
            uint32_t current_balance = MIFARE_GetBalanceML();
            uint32_t last_topup = MIFARE_GetLastTopupAmountML();
            UpdateCardUI(current_balance, last_topup);
        } else {
            // Clear the display if no card is detected
            UpdateCardUI(0, 0);
        }
    } else {
        // Clear the display when not in card-ready states
        UpdateCardUI(0, 0);
    }
}

/**
 * @brief Add balance to an existing customer card (top-up)
 * @param topup_amount_ml Amount to add to the card in milliliters
 * @return DispenserResult_t Operation result
 * 
 * @details This function provides a high-level interface to add credit to an
 *          existing customer card. It includes validation, state management,
 *          and automatic UI updates including the percentage progress bar.
 */
DispenserResult_t MIFARE_Dispenser_TopupCard(uint32_t topup_amount_ml)
{
    if (dispenser_handle.state != DISPENSER_CARD_READY) {
        USB_Log_Printf("DISPENSER: Cannot top-up card - card not ready (state: %s)\r\n", 
                       GetDispenserStateString(dispenser_handle.state));
        return DISPENSER_RESULT_CARD_NOT_READY;
    }
    
    // Validate input parameters
    if (topup_amount_ml == 0) {
        USB_Log_Printf("DISPENSER: Cannot top-up with zero amount\r\n");
        return DISPENSER_RESULT_INVALID_AMOUNT;
    }
    
    if (topup_amount_ml > 50000) { // Max 50 liters per top-up
        USB_Log_Printf("DISPENSER: Top-up amount too large (max 50L)\r\n");
        return DISPENSER_RESULT_INVALID_AMOUNT;
    }
    
    USB_Log_Printf("DISPENSER: Topping up card with %u mL\r\n", topup_amount_ml);
    
    // Get current balance before top-up
    uint32_t old_balance = MIFARE_GetBalanceML();
    
    // Temporarily change state to prevent other operations
    dispenser_handle.state = DISPENSER_USER_REQUESTED;
    
    // Call the MIFARE transaction manager to perform the top-up
    MIFARE_Result_t result = MIFARE_TopupCardBalance(topup_amount_ml);
    
    DispenserResult_t dispenser_result;
    switch (result) {
        case MIFARE_RESULT_OK:
            {
                uint32_t new_balance = MIFARE_GetBalanceML();
                USB_Log_Printf("DISPENSER: Top-up successful\r\n");
                USB_Log_Printf("DISPENSER: Balance updated: %u → %u mL (+%u mL)\r\n", 
                               old_balance, new_balance, topup_amount_ml);
                dispenser_handle.state = DISPENSER_CARD_READY;
                dispenser_result = DISPENSER_RESULT_OK;
                
                // Update UI with new balance and reset percentage to 100% (full top-up)
                UpdateCardUI(new_balance, topup_amount_ml);
                break;
            }
            
        case MIFARE_RESULT_CARD_REMOVED:
            USB_Log_Printf("DISPENSER: Top-up failed - card removed\r\n");
            dispenser_handle.state = DISPENSER_IDLE;
            dispenser_result = DISPENSER_RESULT_CARD_REMOVED;
            break;
            
        case MIFARE_RESULT_ERROR:
            USB_Log_Printf("DISPENSER: Top-up failed - general error\r\n");
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_ERROR;
            break;
            
        case MIFARE_RESULT_WRITE_FAILED:
            USB_Log_Printf("DISPENSER: Top-up failed - write error\r\n");
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_CARD_ERROR;
            break;
            
        default:
            USB_Log_Printf("DISPENSER: Top-up failed - unknown error (%d)\r\n", result);
            dispenser_handle.state = DISPENSER_ERROR;
            dispenser_result = DISPENSER_RESULT_ERROR;
            break;
    }
    
    // If top-up failed, return to previous state after a brief delay
    if (dispenser_result != DISPENSER_RESULT_OK && dispenser_result != DISPENSER_RESULT_CARD_REMOVED) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Give time for error logging
        dispenser_handle.state = DISPENSER_CARD_READY; // Return to card ready if card still present
    }
    
    return dispenser_result;
}

/* ========================================================================== */
/*                              TEST MODE FUNCTIONS                          */
/* ========================================================================== */

#if TEST_MODE_ENABLED

/**
 * @brief Initialize test mode with 100L balance
 */
static void InitializeTestMode(void)
{
    dispenser_handle.test_mode_active = true;
    dispenser_handle.test_card_present = false;
    dispenser_handle.test_balance_ml = TEST_MODE_INITIAL_BALANCE_ML;
    dispenser_handle.test_last_topup_ml = TEST_MODE_INITIAL_BALANCE_ML;
    dispenser_handle.test_simulated_dispensed_ml = 0;
    dispenser_handle.test_last_update_time = (uint32_t)xTaskGetTickCount();
    dispenser_handle.test_dispensing_active = false;
    
    USB_Log_Printf("TEST_MODE: Initialized with %.1f L balance\r\n", 
                   TEST_MODE_INITIAL_BALANCE_ML / 1000.0f);
}

/**
 * @brief Process test mode card detection simulation
 */
static void ProcessTestModeCardDetection(void)
{
    static uint32_t last_card_check = 0;
    static bool last_real_card_state = false;
    uint32_t current_time = (uint32_t)xTaskGetTickCount();
    
    // Check every 500ms
    if ((current_time - last_card_check) < 500) {
        return;
    }
    last_card_check = current_time;
    
    // Detect if a real card is present (any card)
    PN532_CardInfo_t card_info;
    PN532_Status_t card_status = PN532_DetectCard(&card_info);
    bool real_card_present = (card_status == PN532_STATUS_CARD_DETECTED);
    
    // State change detection
    if (real_card_present != last_real_card_state) {
        last_real_card_state = real_card_present;
        
        if (real_card_present) {
            USB_Log_Printf("TEST_MODE: Any card detected - Starting simulated dispense\r\n");
            USB_Log_Printf("TEST_MODE: Card UID: ");
            for (uint8_t i = 0; i < card_info.uid_length && i < 8; i++) {
                USB_Log_Printf("%02X ", card_info.uid[i]);
            }
            USB_Log_Printf("\r\n");
            
            SimulateCardInsertion();
        } else {
            USB_Log_Printf("TEST_MODE: Card removed - Stopping simulated dispense\r\n");
            SimulateCardRemoval();
        }
    }
    
    // Update test mode dispensing if active
    if (dispenser_handle.test_dispensing_active) {
        UpdateTestModeProgress();
    }
}

/**
 * @brief Simulate card insertion and start dispensing
 */
static void SimulateCardInsertion(void)
{
    dispenser_handle.test_card_present = true;
    
    // Update UI to show test card with current balance
    UpdateCardUI(dispenser_handle.test_balance_ml, dispenser_handle.test_last_topup_ml);
    
    // Automatically start dispensing simulation (simulate user requesting 1L)
    uint16_t auto_dispense_amount = 1000; // 1L automatic dispense
    
    if (dispenser_handle.test_balance_ml >= auto_dispense_amount) {
        USB_Log_Printf("TEST_MODE: Auto-starting simulated dispense of %u mL at %.1f L/min\r\n", 
                       auto_dispense_amount, TEST_MODE_SIMULATED_FLOW_LPM);
        StartTestModeDispensing(auto_dispense_amount);
    } else {
        USB_Log_Printf("TEST_MODE: Insufficient balance for auto-dispense (%u mL available)\r\n", 
                       dispenser_handle.test_balance_ml);
    }
}

/**
 * @brief Simulate card removal and stop dispensing
 */
static void SimulateCardRemoval(void)
{
    dispenser_handle.test_card_present = false;
    
    if (dispenser_handle.test_dispensing_active) {
        USB_Log_Printf("TEST_MODE: Card removed during dispensing - Emergency stop\r\n");
        StopTestModeDispensing();
    }
    
    // Clear UI display
    UpdateCardUI(0, 0);
}

/**
 * @brief Start test mode dispensing simulation
 */
static DispenserResult_t StartTestModeDispensing(uint16_t amount_ml)
{
    if (dispenser_handle.test_dispensing_active) {
        return DISPENSER_RESULT_BUSY;
    }
    
    if (dispenser_handle.test_balance_ml < amount_ml) {
        return DISPENSER_RESULT_INSUFFICIENT_BALANCE;
    }
    
    dispenser_handle.test_dispensing_active = true;
    dispenser_handle.requested_amount_ml = amount_ml;
    dispenser_handle.test_simulated_dispensed_ml = 0;
    dispenser_handle.test_last_update_time = (uint32_t)xTaskGetTickCount();
    
    // Simulate valve opening (call the real function that would be called)
    SetValveState(true);
    
    USB_Log_Printf("TEST_MODE: Dispensing started - Target: %u mL at %.1f L/min\r\n", 
                   amount_ml, TEST_MODE_SIMULATED_FLOW_LPM);
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Update test mode dispensing progress with 20L/min simulation
 */
static DispenserResult_t UpdateTestModeProgress(void)
{
    uint32_t current_time = (uint32_t)xTaskGetTickCount();
    uint32_t elapsed_ms = current_time - dispenser_handle.test_last_update_time;
    
    // Calculate how much should be dispensed based on 20L/min flow rate
    // 20 L/min = 20000 mL/min = 333.33 mL/s = 0.333 mL/ms
    float flow_rate_ml_per_ms = (TEST_MODE_SIMULATED_FLOW_LPM * 1000.0f) / (60.0f * 1000.0f);
    uint32_t additional_ml = (uint32_t)(elapsed_ms * flow_rate_ml_per_ms);
    
    if (additional_ml > 0) {
        dispenser_handle.test_simulated_dispensed_ml += additional_ml;
        dispenser_handle.test_last_update_time = current_time;
        
        // Update balance
        if (dispenser_handle.test_balance_ml >= additional_ml) {
            dispenser_handle.test_balance_ml -= additional_ml;
        } else {
            dispenser_handle.test_balance_ml = 0;
        }
        
        // Update UI with new balance
        UpdateCardUI(dispenser_handle.test_balance_ml, dispenser_handle.test_last_topup_ml);
        
        USB_Log_Printf("TEST_MODE: Progress - %u/%u mL dispensed, %.1f L remaining (%.1f L/min)\r\n", 
                       dispenser_handle.test_simulated_dispensed_ml,
                       dispenser_handle.requested_amount_ml,
                       dispenser_handle.test_balance_ml / 1000.0f,
                       TEST_MODE_SIMULATED_FLOW_LPM);
    }
    
    // Check if target amount reached
    if (dispenser_handle.test_simulated_dispensed_ml >= dispenser_handle.requested_amount_ml) {
        USB_Log_Printf("TEST_MODE: Target amount reached - Stopping dispense\r\n");
        StopTestModeDispensing();
        return DISPENSER_RESULT_COMPLETE;
    }
    
    // Check if card is still present
    if (!dispenser_handle.test_card_present) {
        USB_Log_Printf("TEST_MODE: Card removed - Emergency stop\r\n");
        StopTestModeDispensing();
        return DISPENSER_RESULT_CARD_REMOVED;
    }
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Stop test mode dispensing
 */
static void StopTestModeDispensing(void)
{
    if (!dispenser_handle.test_dispensing_active) {
        return;
    }
    
    dispenser_handle.test_dispensing_active = false;
    
    // Simulate valve closing (call the real function that would be called)
    SetValveState(false);
    
    USB_Log_Printf("TEST_MODE: Dispensing stopped - Total dispensed: %u mL\r\n", 
                   dispenser_handle.test_simulated_dispensed_ml);
    
    // Update UI with final balance
    UpdateCardUI(dispenser_handle.test_balance_ml, dispenser_handle.test_last_topup_ml);
}

/**
 * @brief Reset test mode balance to 100L (for testing)
 */
void MIFARE_Dispenser_ResetTestMode(void)
{
#if TEST_MODE_ENABLED
    if (dispenser_handle.test_mode_active) {
        dispenser_handle.test_balance_ml = TEST_MODE_INITIAL_BALANCE_ML;
        dispenser_handle.test_last_topup_ml = TEST_MODE_INITIAL_BALANCE_ML;
        
        if (dispenser_handle.test_card_present) {
            UpdateCardUI(dispenser_handle.test_balance_ml, dispenser_handle.test_last_topup_ml);
        }
        
        USB_Log_Printf("TEST_MODE: Balance reset to %.1f L\r\n", 
                       TEST_MODE_INITIAL_BALANCE_ML / 1000.0f);
    }
#endif
}

/**
 * @brief Get test mode status (for debugging)
 */
void MIFARE_Dispenser_GetTestModeStatus(void)
{
#if TEST_MODE_ENABLED
    if (dispenser_handle.test_mode_active) {
        USB_Log_Printf("TEST_MODE STATUS:\r\n");
        USB_Log_Printf("  - Active: %s\r\n", dispenser_handle.test_mode_active ? "YES" : "NO");
        USB_Log_Printf("  - Card Present: %s\r\n", dispenser_handle.test_card_present ? "YES" : "NO");
        USB_Log_Printf("  - Balance: %.3f L (%u mL)\r\n", 
                       dispenser_handle.test_balance_ml / 1000.0f, dispenser_handle.test_balance_ml);
        USB_Log_Printf("  - Dispensing: %s\r\n", dispenser_handle.test_dispensing_active ? "YES" : "NO");
        if (dispenser_handle.test_dispensing_active) {
            USB_Log_Printf("  - Target: %u mL\r\n", dispenser_handle.requested_amount_ml);
            USB_Log_Printf("  - Dispensed: %u mL\r\n", dispenser_handle.test_simulated_dispensed_ml);
        }
    } else {
        USB_Log_Printf("TEST_MODE: Disabled\r\n");
    }
#else
    USB_Log_Printf("TEST_MODE: Not compiled in\r\n");
#endif
}

#endif /* TEST_MODE_ENABLED */