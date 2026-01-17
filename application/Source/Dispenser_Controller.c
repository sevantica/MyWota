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
 * @file Dispenser_Controller.c
 * @brief Dispenser business logic layer - manages balance (ml) and dispense operations
 * @details This layer polls MIFARE_Transaction_Manager for card data,
 *          makes business decisions, and updates card data accordingly.
 *          Follows the polling architecture pattern.
 *          
 *          Implements the common Application_Interface for interoperability
 *          with other MIFARE-based applications (e.g., car wash).
 */

/* Includes ------------------------------------------------------------------*/
#include "Dispenser_Controller.h"
#include "Application_Interface.h"
#include "MIFARE_Transaction_Core.h"
#include "IO_Expander_Control.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "System.h"
#include "System_Config.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "YS_S201_Driver.h"
#include "Hardware_Access.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"  /* For gpio_put */

/*Private defines ---------------------------------------------------*/
#define LOG_DEBUG_DISPENSER_EN      0
#define LOG_CRITICAL_DISPENSER_EN   1
#define LOG_ERROR_DISPENSER_EN      1

#if LOG_DEBUG_DISPENSER_EN
    #define DISPENSER_DEBUG(fmt, ...) USB_Log_Printf("DISPENSER: " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DISPENSER_DEBUG(...)
#endif

#if LOG_CRITICAL_DISPENSER_EN
    #define DISPENSER_CRITICAL(fmt, ...) USB_Log_Printf("DISPENSER: " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DISPENSER_CRITICAL(...)
#endif

#if LOG_ERROR_DISPENSER_EN
    #define DISPENSER_ERROR(fmt, ...) USB_Log_Printf("DISPENSER: " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DISPENSER_ERROR(...)
#endif

/* Convenience macro for debug logging */
#define DISPENSER_LOG(fmt, ...) DISPENSER_DEBUG(fmt, ##__VA_ARGS__)

/*Private variables -------------------------------------------------*/

/* Note: DispenserState_t enum is defined in Dispenser_Controller.h */

/* Timing configuration */
#define DISPENSER_DEDUCTION_INTERVAL_MS   50     /* Update in-memory balance every 50ms (faster response) */
#define DISPENSER_FAST_WRITE_INTERVAL_MS    200    /* Write primary data every 200ms */
#define DISPENSER_BACKUP_WRITE_INTERVAL_MS  500    /* Write backup data every 500ms */

static Dispenser_TimerState_t g_dispense_timer = {0};
static DispenserState_t g_dispenser_state = DISPENSER_IDLE;
static TaskHandle_t dispenser_task_handle = NULL;

/* Card write timing - write less frequently to avoid slow I/O */
static uint32_t g_last_card_write_time = 0;
static uint32_t g_last_backup_write_time = 0;
static uint32_t g_pending_deduction_ml = 0;  /* Accumulated deduction not yet written to card */

/* Card removal detection via write failures (not polling) */
#define DISPENSER_WRITE_FAILURE_THRESHOLD   1   /* Consecutive failures before confirming removal */
static uint32_t g_consecutive_write_failures = 0;
static bool g_card_removal_confirmed = false;  /* Set true when PN532 confirms card removed */

/* No-card mode: dispense without card (for testing/maintenance) */
static bool g_no_card_mode = false;
static uint32_t g_target_volume_ml = 0;  /* Target volume in no-card mode (0 = unlimited) */

/* Wait-and-dispense mode: wait for flow then limit volume */
static bool g_wait_for_flow_mode = false;
static uint32_t g_max_dispense_volume_ml = 0;  /* Maximum volume to dispense after flow detected */
static bool g_flow_started = false;  /* Track if flow has started in wait mode */

/* Flow sensor for water volume measurement */
static YS_S201_Handle_t g_flow_sensor_handle;
static bool g_flow_sensor_initialized = false;
static float g_dispense_start_volume_ml = 0.0f;  /* Volume at dispense start for delta calculation */

/* Flow watchdog timer - detects card removal when no flow for 3s */
#define DISPENSER_FLOW_WATCHDOG_TIMEOUT_MS  3000
static float g_last_flow_volume_ml = 0.0f;

/* Card removal polling - wait for card absent for 5s before returning to IDLE */
#define DISPENSER_CARD_REMOVAL_TIMEOUT_MS  5000
static uint32_t g_card_last_seen_tick = 0;
static uint32_t g_last_flow_change_tick = 0;

/*Private function prototypes ---------------------------------------*/
static void dispenser_start_dispense(void);
static void dispenser_start_dispense_internal(bool no_card_mode, uint32_t target_ml);
static void dispenser_stop_dispense(const char* reason);
static bool dispense(uint32_t elapsed_ms);
static bool dispenser_has_balance(void);
static void dispenser_valve_open(void);
static void dispenser_valve_close(void);
static const char* dispenser_get_valve_state_name(ValveState_t state);

/*Public Functions ---------------------------------------------------*/

/**
 * @brief Initialize dispenser system
 */
DispenserResult_t MIFARE_Dispenser_Init(void)
{
    // Initialize dispense timer state
    memset(&g_dispense_timer, 0, sizeof(Dispenser_TimerState_t));
    
    g_dispense_timer.valve_state = VALVE_CLOSED;
    g_dispenser_state = DISPENSER_IDLE;
    
    // Ensure valve is closed initially
    dispenser_valve_close();
    
    // Initialize YS-S201 water flow sensor (GPIO 22)
    App_GPIO_Pins_t gpio_pins = Get_App_GPIO_Pins();
    YS_S201_Status_t flow_status = YS_S201_Init(&g_flow_sensor_handle, gpio_pins.flow_sensor_pin);
    if (flow_status == YS_S201_OK) {
        // Start flow measurement
        flow_status = YS_S201_Start(&g_flow_sensor_handle);
        if (flow_status == YS_S201_OK) {
            g_flow_sensor_initialized = true;
            DISPENSER_CRITICAL("[✓] Flow sensor initialized on GPIO %lu", gpio_pins.flow_sensor_pin);
        } else {
            DISPENSER_ERROR("[✗] Flow sensor start failed: %d", flow_status);
        }
    } else {
        DISPENSER_ERROR("[✗] Flow sensor init failed: %d", flow_status);
    }
    
    // Register application interface for buzzer polling
    const Application_Instance_t* app_interface = Dispenser_GetApplicationInterface();
    Application_Result_t app_result = Application_Register(app_interface);
    if (app_result == APP_RESULT_OK) {
        DISPENSER_CRITICAL("[✓] Dispenser application interface registered");
    } else {
        DISPENSER_ERROR("[✗] Failed to register application interface: %d", app_result);
    }
    
    DISPENSER_CRITICAL("[✓] Dispenser system initialized");
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Check if card has balance available
 * @return true if balance_ml > 0
 */
static bool dispenser_has_balance(void)
{
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    if (!user_data) {
        DISPENSER_DEBUG("has_balance: No user data available");
        return false;
    }
    bool has_balance = (user_data->balance > 0);
    DISPENSER_DEBUG("has_balance: balance=%lu ml, result=%s", user_data->balance, has_balance ? "YES" : "NO");
    return has_balance;
}

/**
 * @brief Start the dispense
 * @param no_card_mode If true, dispense without card (for testing)
 * @param target_ml Target volume in ml (only used in no_card_mode, 0 = unlimited)
 */
static void dispenser_start_dispense_internal(bool no_card_mode, uint32_t target_ml)
{
    if (!no_card_mode) {
        MIFARE_UserData_t *user_data = MIFARE_GetUserData();
        if (!user_data || user_data->balance == 0) {
            DISPENSER_ERROR("Cannot start dispense - no balance");
            return;
        }
    }
    
    g_no_card_mode = no_card_mode;
    g_target_volume_ml = target_ml;
    
    g_dispense_timer.dispense_start_time = xTaskGetTickCount();
    g_dispense_timer.last_deduction_time = g_dispense_timer.dispense_start_time;
    g_dispense_timer.dispense_active = true;
    g_dispense_timer.balance_deducted_ml = 0;
    g_dispenser_state = DISPENSER_DISPENSE_IN_PROGRESS;
    
    // Reset card write tracking
    g_last_card_write_time = g_dispense_timer.dispense_start_time;
    g_last_backup_write_time = g_dispense_timer.dispense_start_time;
    g_pending_deduction_ml = 0;
    g_consecutive_write_failures = 0;  // Reset failure counter for new dispense session
    g_card_removal_confirmed = false;  // Reset card removal flag
    
    // Capture starting flow volume for delta calculation
    if (g_flow_sensor_initialized) {
        YS_S201_FlowData_t flow_data;
        if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) == YS_S201_OK) {
            g_dispense_start_volume_ml = flow_data.total_volume_ml;
            g_last_flow_volume_ml = flow_data.total_volume_ml;  // Initialize watchdog
            DISPENSER_DEBUG("Flow sensor start volume: %.1f ml", g_dispense_start_volume_ml);
        }
    }
    
    // Initialize flow watchdog timer
    g_last_flow_change_tick = xTaskGetTickCount();
    
    // Open the valve to start dispensing
    dispenser_valve_open();
    
    if (no_card_mode) {
        if (target_ml > 0) {
            DISPENSER_CRITICAL("[✓] Dispense STARTED - valve OPEN (no-card mode, target: %lu ml)", target_ml);
        } else {
            DISPENSER_CRITICAL("[✓] Dispense STARTED - valve OPEN (no-card mode, unlimited)");
        }
    } else {
        MIFARE_UserData_t *user_data = MIFARE_GetUserData();
        DISPENSER_CRITICAL("[✓] Dispense STARTED - valve OPEN (balance: %lu ml)", 
                         user_data ? user_data->balance : 0);
    }
}

/**
 * @brief Start the dispense (called when card with balance is detected)
 */
static void dispenser_start_dispense(void)
{
    dispenser_start_dispense_internal(false, 0);
}

/**
 * @brief Stop the dispense
 * @param reason Reason for stopping (for logging)
 */
static void dispenser_stop_dispense(const char* reason)
{
    if (!g_dispense_timer.dispense_active) {
        return;
    }
    
    // Close the valve
    dispenser_valve_close();
    
    g_dispense_timer.dispense_active = false;
    g_dispenser_state = DISPENSER_IDLE;
    
    // Calculate dispensed volume from flow sensor
    float dispensed_ml = 0.0f;
    if (g_flow_sensor_initialized) {
        YS_S201_FlowData_t flow_data;
        if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) == YS_S201_OK) {
            dispensed_ml = flow_data.total_volume_ml - g_dispense_start_volume_ml;
        }
    }
    
    if (g_no_card_mode) {
        // No-card mode: just log completion
        DISPENSER_CRITICAL("[✓] Dispense STOPPED - %s (dispensed: %.0f ml)", reason, dispensed_ml);
    } else {
        // Card mode: log with warning if balance was lost
        if (g_pending_deduction_ml > 0) {
            DISPENSER_CRITICAL("[✗] Dispense STOPPED - %s (deducted: %lu ml, LOST: %lu ml not written to card)", 
                             reason, g_dispense_timer.balance_deducted_ml, g_pending_deduction_ml);
            g_pending_deduction_ml = 0;  // Reset for next session
        } else {
            DISPENSER_CRITICAL("[✓] Dispense STOPPED - %s (deducted: %lu ml this session)", 
                             reason, g_dispense_timer.balance_deducted_ml);
        }
    }
    
    // Clear no-card mode
    g_no_card_mode = false;
    g_target_volume_ml = 0;
    
    // Clear wait-and-dispense mode
    g_wait_for_flow_mode = false;
    g_max_dispense_volume_ml = 0;
    g_flow_started = false;
    
    g_dispense_timer.valve_state = VALVE_CLOSED;
}

/**
 * @brief Process flow sensor and deduct volume (handles both card and no-card modes)
 * @param elapsed_ms Milliseconds elapsed since last call (used for timing card writes)
 * @return true if dispense should continue, false if should stop
 * 
 * In card mode: reads flow sensor, deducts from card balance, writes to card periodically
 * In no-card mode: reads flow sensor, checks target volume, no card operations
 */
static bool dispense(uint32_t elapsed_ms)
{
    (void)elapsed_ms;  // Used only for timing card writes
    
    // Get current flow volume from sensor
    float current_volume_ml = 0.0f;
    float volume_since_start = 0.0f;
    
    if (g_flow_sensor_initialized) {
        YS_S201_FlowData_t flow_data;
        if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) == YS_S201_OK) {
            current_volume_ml = flow_data.total_volume_ml;
            volume_since_start = current_volume_ml - g_dispense_start_volume_ml;
            
            // Flow watchdog: reset timer if volume increased
            if (current_volume_ml > g_last_flow_volume_ml) {
                g_last_flow_volume_ml = current_volume_ml;
                g_last_flow_change_tick = xTaskGetTickCount();
                
                // In wait-for-flow mode, mark flow as started
                if (g_wait_for_flow_mode && !g_flow_started) {
                    g_flow_started = true;
                    DISPENSER_CRITICAL("[→] Flow detected - car present, dispensing up to %lu ml", g_max_dispense_volume_ml);
                }
            }
            
            
        }
    } else {
        // No flow sensor - can't measure volume
        DISPENSER_DEBUG("No flow sensor - skipping volume processing");
        return true;
    }
    
    // STOP CONDITION 1: Flow watchdog - no flow for 3s
    uint32_t time_since_flow = pdTICKS_TO_MS(xTaskGetTickCount() - g_last_flow_change_tick);
    if (time_since_flow >= DISPENSER_FLOW_WATCHDOG_TIMEOUT_MS) {
        DISPENSER_CRITICAL("[→] No flow for %lu ms - stopping dispense", time_since_flow);
        dispenser_stop_dispense("No flow timeout");
        
        // Only set error state if in normal mode AND card is still present
        // (card may have been removed during the 3s timeout)
        if (!g_no_card_mode && MIFARE_IsCardPresent()) {
            // Check if this is actually a balance issue
            MIFARE_UserData_t *user_data = MIFARE_GetUserData();
            if (user_data && user_data->balance == 0) {
                // Balance is zero - this is a balance issue, not flow issue
                MIFARE_SetTransactionState(TRANSACTION_STATE_READY);  // Keep card ready
                DISPENSER_CRITICAL("[✗] Insufficient balance to dispense");
                DISPENSER_CRITICAL("[→] Card balance is 0 ml - please top up.");
            } else {
                // Balance is available - this is a genuine flow sensor issue
                MIFARE_SetTransactionState(TRANSACTION_STATE_ERROR_NO_FLOW);
                DISPENSER_CRITICAL("[✗] No flow detected during dispense");
                DISPENSER_CRITICAL("[→] Remove card and re-insert to retry.");
            }
        } else if (!g_no_card_mode) {
            DISPENSER_CRITICAL("[✗] Card removed during dispense");
        } else {
            DISPENSER_CRITICAL("[✗] No flow detected during test");
            DISPENSER_CRITICAL("[→] Card remains ready.");
        }
        
        return false;
    }
    
    // STOP CONDITION 2 (no-card mode): Target volume reached
    if (g_no_card_mode) {
        // Update dispensed amount tracker (for UI display) - use rounding
        g_dispense_timer.balance_deducted_ml = (uint32_t)(volume_since_start + 0.5f);
        DISPENSER_DEBUG("No-card mode: total_deducted=%lu ml, volume_since_start=%.1f ml", 
                       g_dispense_timer.balance_deducted_ml, volume_since_start);
        
        if (g_target_volume_ml > 0 && volume_since_start >= (float)g_target_volume_ml) {
            dispenser_stop_dispense("Target volume reached");
            return false;
        }
        return true;  // Continue dispensing
    }
    
    // STOP CONDITIONS 3-5 (card mode): Card removed, balance exhausted handled below
    
    // Card mode: deduct from balance
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    if (!user_data) {
        return true;  // Can't determine - assume card present
    }
    
    // Calculate how much NEW volume to deduct (delta since last deduction)
    // Use rounding instead of truncation to prevent undercharging
    uint32_t total_used_ml = (uint32_t)(volume_since_start + 0.5f);
    uint32_t to_deduct = 0;
    if (total_used_ml > g_dispense_timer.balance_deducted_ml) {
        to_deduct = total_used_ml - g_dispense_timer.balance_deducted_ml;
    }
    
    // Clamp to available balance
    if (to_deduct > user_data->balance) {
        to_deduct = user_data->balance;  // Don't go negative
    }
    
    if (to_deduct > 0) {
        // Update balance in memory (fast - always do this)
        MIFARE_UserData_t updated_user_data;
        memcpy(&updated_user_data, user_data, sizeof(MIFARE_UserData_t));
        updated_user_data.balance -= to_deduct;
        updated_user_data.transaction_counter++;
        MIFARE_SetUserData(&updated_user_data);
        
        // Track total deducted this session (in ml)
        g_dispense_timer.balance_deducted_ml += to_deduct;
        g_pending_deduction_ml += to_deduct;
        
        DISPENSER_DEBUG("Deducted: to_deduct=%lu ml, total_deducted=%lu ml, volume_since_start=%.1f ml", 
                       to_deduct, g_dispense_timer.balance_deducted_ml, volume_since_start);
        
        // STOP CONDITION: Wait-for-flow mode - volume limit reached (check AFTER deduction)
        if (g_wait_for_flow_mode && g_flow_started) {
            if (g_dispense_timer.balance_deducted_ml >= g_max_dispense_volume_ml) {
                DISPENSER_CRITICAL("[✓] Volume limit reached: %lu ml / %lu ml", 
                                 g_dispense_timer.balance_deducted_ml, g_max_dispense_volume_ml);
                dispenser_stop_dispense("Volume limit reached");
                return false;
            }
        }
    }
    
    // Check if we should write to card (Dual Interval: Fast=200ms, Backup=500ms)
    {
        uint32_t current_time = xTaskGetTickCount();
        bool attempt_write = false;
        bool is_fast_write = true;
        
        // 1. Check Backup Trigger (Full Write = Primary + Backup)
        if (pdTICKS_TO_MS(current_time - g_last_backup_write_time) >= DISPENSER_BACKUP_WRITE_INTERVAL_MS && g_pending_deduction_ml > 0) {
            attempt_write = true;
            is_fast_write = false; // Full write
        }
        // 2. Check Fast Trigger (Primary Only)
        else if (pdTICKS_TO_MS(current_time - g_last_card_write_time) >= DISPENSER_FAST_WRITE_INTERVAL_MS && g_pending_deduction_ml > 0) {
            attempt_write = true;
            is_fast_write = true; // Fast write
        }
        
        if (attempt_write) {
            // Feed watchdog before potentially long MIFARE operations
            System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
            
            // Perform write (Fast or Full based on interval)
            MIFARE_Result_t result = MIFARE_UpdateCardData(is_fast_write);
            
            if (result == MIFARE_RESULT_OK) {
#if LOG_DEBUG_DISPENSER_EN
                MIFARE_UserData_t *written_user_data = MIFARE_GetUserData();
                DISPENSER_DEBUG("Card write (%s): deducted %lu ml total, remaining: %lu ml", 
                             is_fast_write ? "fast" : "FULL",
                             g_pending_deduction_ml, written_user_data ? written_user_data->balance : 0);
#endif
                g_pending_deduction_ml = 0;
                g_last_card_write_time = current_time;
                g_consecutive_write_failures = 0;  // Reset on success
                
                if (!is_fast_write) {
                    g_last_backup_write_time = current_time; // Update backup timer only on full write
                }
            } else {
                g_consecutive_write_failures++;
                DISPENSER_ERROR("Write failure (%s) #%lu: %s", 
                             is_fast_write ? "fast" : "FULL",
                             g_consecutive_write_failures, 
                             MIFARE_GetResultString(result));
            }
            
            // After N consecutive failures, verify card is actually gone via PN532 poll
            if (g_consecutive_write_failures >= DISPENSER_WRITE_FAILURE_THRESHOLD) {
                DISPENSER_CRITICAL("[→] %lu consecutive write failures - polling PN532 to confirm card state", 
                                g_consecutive_write_failures);
            
                // Feed WDT before PN532 poll
                System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
                
                MIFARE_Result_t verify_result = MIFARE_VerifyCardPresence();
                if (verify_result != MIFARE_RESULT_OK) {
                    // Card is confirmed removed
                    DISPENSER_CRITICAL("[✗] Card removal CONFIRMED by PN532 poll");
                    g_consecutive_write_failures = 0;
                    g_card_removal_confirmed = true;  // Skip DISPENSER_WAITING_FOR_REMOVAL
                    
                    // Force immediate card removal (skip stability check - already confirmed)
                    MIFARE_ForceCardRemoval();
                    dispenser_stop_dispense("Card removed");
                    
                    return false;  // Signal card removed
                } else {
                    // Card is still there - RF interference or temporary issue
                    DISPENSER_DEBUG("Card still present - resetting failure counter");
                    g_consecutive_write_failures = 0;
                }
            }
        }
    }
    
    // Check if balance exhausted
    MIFARE_UserData_t *final_user_data = MIFARE_GetUserData();
    if (final_user_data && final_user_data->balance == 0) {
        dispenser_stop_dispense("Balance exhausted");
        return false;
    }
    
    return true;  // Continue dispensing
}

/**
 * @brief Legacy function - no longer used in new model
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_StartDispense(void)
{
    // In the new model, dispense starts automatically when card is detected
    if (MIFARE_IsCardReady() && dispenser_has_balance()) {
        dispenser_start_dispense();
        return DISPENSER_RESULT_OK;
    }
    return DISPENSER_RESULT_INSUFFICIENT_BALANCE;
}

/**
 * @brief Get current dispense status
 * @param status Pointer to status structure to fill
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_GetStatus(DispenserStatus_t *status)
{
    if (!status) {
        return DISPENSER_RESULT_ERROR;
    }
    
    status->state = g_dispenser_state;
    status->dispense_active = g_dispense_timer.dispense_active;
    status->card_present = MIFARE_IsCardPresent();
    status->dispense_bay_id = g_dispense_timer.dispense_bay_id;
    
    // Get balance from card (0 in no-card mode)
    if (g_no_card_mode) {
        status->balance_ml = 0;
        status->remaining_ml = g_target_volume_ml;  // Target volume in no-card mode
    } else {
        MIFARE_UserData_t *user_data = MIFARE_GetUserData();
        status->balance_ml = user_data ? user_data->balance : 0;
        status->remaining_ml = status->balance_ml;  // Remaining = current balance
    }
    
    // Calculate elapsed time this session
    if (g_dispense_timer.dispense_active) {
        uint32_t elapsed_ticks = xTaskGetTickCount() - g_dispense_timer.dispense_start_time;
        status->elapsed_ms = pdTICKS_TO_MS(elapsed_ticks);
    } else {
        status->elapsed_ms = 0;
    }
    
    // Get valve state
    status->valve_state = g_dispense_timer.valve_state;
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Emergency stop dispense
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_EmergencyStop(void)
{
    dispenser_stop_dispense("Emergency stop");
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Manually start dispense without card (for testing/debugging)
 * @param target_ml Target volume in ml (0 = unlimited, runs until stopped)
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_ManualStart(uint32_t target_ml)
{
    // Don't allow if dispense already active
    if (g_dispense_timer.dispense_active) {
        DISPENSER_ERROR("Manual start failed - dispense already in progress");
        return DISPENSER_RESULT_ERROR;
    }
    
    // Use the unified dispense start in no-card mode
    dispenser_start_dispense_internal(true, target_ml);
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Wait for card validation then dispense up to max volume
 * @param max_volume_ml Maximum volume to dispense in milliliters
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_WaitAndDispense(uint32_t max_volume_ml)
{
    // Don't allow if dispense already active
    if (g_dispense_timer.dispense_active) {
        DISPENSER_ERROR("Wait-and-dispense failed - dispense already in progress");
        return DISPENSER_RESULT_ERROR;
    }
    
    if (max_volume_ml == 0) {
        DISPENSER_ERROR("Wait-and-dispense failed - volume must be > 0");
        return DISPENSER_RESULT_ERROR;
    }
    
    if (!g_flow_sensor_initialized) {
        DISPENSER_ERROR("Wait-and-dispense failed - flow sensor not initialized");
        return DISPENSER_RESULT_ERROR;
    }
    
    // Set wait-for-flow mode flags - valve will open when card validates
    g_wait_for_flow_mode = true;
    g_max_dispense_volume_ml = max_volume_ml;
    g_flow_started = false;
    
    DISPENSER_CRITICAL("[→] Waiting for card validation, will dispense max %lu ml", max_volume_ml);
    DISPENSER_CRITICAL("[→] Scan card to start dispense...");
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Manually stop dispense
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_ManualStop(void)
{
    if (!g_dispense_timer.dispense_active) {
        DISPENSER_DEBUG("Manual stop - no dispense active");
        return DISPENSER_RESULT_OK;
    }
    
    dispenser_stop_dispense("Manual stop");
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Check if dispense is currently active (for buzzer polling)
 * @return true if dispense in progress, false otherwise
 */
bool MIFARE_Dispenser_IsDispenseActive(void)
{
    return g_dispense_timer.dispense_active;
}

/**
 * @brief Get amount dispensed in current session (for buzzer polling)
 * @return Amount dispensed in milliliters (0 if no dispense or nothing dispensed)
 */
uint32_t Dispenser_GetDispensedAmountML(void)
{
    return g_dispense_timer.balance_deducted_ml;
}

/**
 * @brief Dispenser polling task - UNIFIED STATE MACHINE
 * 
 * Handles both card-based and no-card (manual) dispensing through the same state machine.
 * 
 * Sequence:
 * 1. Card scanned with balance > 0 → Dispense auto-starts (card mode)
 * 2. dispensestart command → Dispense starts (no-card mode)
 * 3. While dispensing: deduct from balance (card mode) or check target (no-card mode)
 * 4. Stops on: card removal, balance=0, target reached, no flow, or manual stop
 * 
 * @param argument Task argument (unused)
 */
void MIFARE_Dispenser_Task(void* argument)
{
    (void)argument;
    
    DISPENSER_CRITICAL("[→] Dispenser task started");
    
    while (1) {
        // Feed watchdog every second
        TASK_HEARTBEAT_EVERY_SECOND("Dispenser");
        System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
        
        // In no-card mode, skip card-related checks in IDLE state
        bool card_ready = false;
        static bool last_card_ready = false;
        if (!g_no_card_mode) {
            card_ready = MIFARE_IsCardReady();
            // Log state transitions (edge detection)
            if (card_ready != last_card_ready) {
                if (card_ready) {
                    DISPENSER_CRITICAL("[→] Card READY detected (state=%d)", g_dispenser_state);
                } else {
                    DISPENSER_CRITICAL("[→] Card NO LONGER ready (state=%d)", g_dispenser_state);
                }
                last_card_ready = card_ready;
            }
        }
        
        // STATE MACHINE
        switch (g_dispenser_state) {
            case DISPENSER_IDLE:
                // Only check for card in card mode
                if (!g_no_card_mode && card_ready) {
                    // Get MIFARE state to check if auto-dispense is allowed
                    MIFARE_TransactionState_t mifare_state = MIFARE_GetTransactionState();
                    
                    // Only auto-dispense if card is in normal READY state
                    // Block auto-dispense for WAITING_REMOVAL (post-topup/init/deduction)
                    if (mifare_state != TRANSACTION_STATE_READY) {
                        DISPENSER_DEBUG("IDLE: Card ready but state=%d - no auto-dispense", mifare_state);
                        break;  // Don't auto-dispense
                    }
                    
                    DISPENSER_DEBUG("IDLE: Card ready detected - checking for pending commands");
                    // Card detected - first check for pending USB commands (like topup, recover)
                    USB_PendingCommandState_t* pending = USB_Command_GetPendingCommand();
                    if (pending != NULL && pending->active) {
                        DISPENSER_DEBUG("IDLE: Pending USB command found (cmd=%d) - skipping auto-dispense", pending->command);
                        // For RECOVER command, verify UID matches before executing
                        if (pending->command == USB_PENDING_CMD_RECOVER) {
                            PN532_CardInfo_t card_info;
                            if (MIFARE_GetCurrentCardInfo(&card_info)) {
                                if (card_info.uid_length == pending->target_uid_length &&
                                    memcmp(card_info.uid, pending->target_uid, pending->target_uid_length) == 0) {
                                    // UID matches - execute recovery
                                    USB_Command_ExecutePendingCommand(pending);
                                }
                                // UID doesn't match - don't execute, wait for correct card
                            }
                        } else {
                            // Other commands (topup, cardinit) - execute immediately
                            USB_Command_ExecutePendingCommand(pending);
                            DISPENSER_DEBUG("IDLE: USB command executed");
                        }
                        // Don't auto-start dispense this cycle - let card be re-polled
                        break;
                    }
                    
                    // No pending command - check balance and auto-start dispense
                    DISPENSER_DEBUG("IDLE: No pending commands - checking balance");
                    if (dispenser_has_balance()) {
                        DISPENSER_DEBUG("IDLE: Balance available - starting dispense");
                        dispenser_start_dispense();
                    } else {
                        DISPENSER_DEBUG("IDLE: No balance available - skipping dispense");
                    }
                }
                break;
                
            case DISPENSER_DISPENSE_IN_PROGRESS:
                // Dispense active - process flow and balance (unified for card and no-card modes)
                {
                    uint32_t current_time = xTaskGetTickCount();
                    uint32_t elapsed_since_deduction = pdTICKS_TO_MS(current_time - g_dispense_timer.last_deduction_time);
                    
                    if (elapsed_since_deduction >= DISPENSER_DEDUCTION_INTERVAL_MS) {
                        // Process flow and balance (handles all stop conditions)
                        bool should_continue = dispense(elapsed_since_deduction);
                        g_dispense_timer.last_deduction_time = current_time;
                        
                        if (!should_continue) {
                            // Dispense stopped - transition based on mode and reason
                            if (!g_no_card_mode && !g_card_removal_confirmed) {
                                // Card mode, card NOT confirmed removed - wait for removal
                                g_dispenser_state = DISPENSER_WAITING_FOR_REMOVAL;
                                g_card_last_seen_tick = current_time;
                            } else {
                                // No-card mode OR card already confirmed removed - go to IDLE
                                g_dispenser_state = DISPENSER_IDLE;
                                g_card_removal_confirmed = false;  // Reset flag
                            }
                            break;
                        }
                    }
                }
                break;
                
            case DISPENSER_WAITING_FOR_REMOVAL:
                // Wait for card removal before returning to IDLE
                // This prevents automatic retry on flow timeout - user must remove and re-tap
                {
                    // Check if card is no longer ready (removed by user or MIFARE)
                    bool card_is_ready = MIFARE_IsCardReady();
                    
                    if (card_is_ready) {
                        // Card still present - keep waiting for removal
                        DISPENSER_DEBUG("WAITING_FOR_REMOVAL: Card still present");
                    } else {
                        // Card removed - return to IDLE immediately
                        DISPENSER_CRITICAL("[✓] Card removed - returning to IDLE");
                        g_dispenser_state = DISPENSER_IDLE;
                    }
                }
                break;
        }
        
        // Adaptive polling: faster when IDLE (waiting for card), slower when dispensing
        uint32_t poll_delay_ms = (g_dispenser_state == DISPENSER_IDLE) ? 20 : 100;
        vTaskDelay(pdMS_TO_TICKS(poll_delay_ms));
    }
}

/**
 * @brief Start dispenser polling task
 */
void Task_Start_Dispenser_Task(void)
{
    if (dispenser_task_handle != NULL) {
        DISPENSER_CRITICAL("Task already running");
        return;
    }

    BaseType_t result = xTaskCreate(MIFARE_Dispenser_Task, 
                                    "Dispenser",  // Must match WDT tracking name in System.c
                                    DISPENSER_TASK_STACK_WORDS,
                                    NULL, 
                                    tskIDLE_PRIORITY + 1, 
                                    &dispenser_task_handle);
    
    if (result != pdPASS) {
        DISPENSER_ERROR("[✗] Task creation FAILED (result=%d)", result);
    } else {
        DISPENSER_CRITICAL("[✓] Task created successfully");
    }
}

/**
 * @brief Stop dispenser polling task
 */
void Task_Stop_Dispenser_Task(void)
{
    if (dispenser_task_handle != NULL) {
        // Stop any active dispense before deleting task
        if (g_dispense_timer.dispense_active) {
            dispenser_stop_dispense("Task stopped");
        }
        vTaskDelete(dispenser_task_handle);
        dispenser_task_handle = NULL;
        DISPENSER_CRITICAL("[✓] Task stopped");
    }
}

/* UI Getter Functions -------------------------------------------------------*/

uint32_t Dispenser_GetBalanceMl(void)
{
    // In no-card mode, no balance tracking
    if (g_no_card_mode) {
        return 0;
    }
    
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    return user_data ? user_data->balance : 0;
}

uint32_t Dispenser_GetDispenseVolumeRemainingMl(void)
{
    // In no-card mode, return target volume (0 = unlimited)
    if (g_no_card_mode) {
        return g_target_volume_ml;
    }
    
    // For card-based dispense, remaining volume IS the card balance
    return Dispenser_GetBalanceMl();
}

bool Dispenser_IsDispenseActive(void)
{
    return g_dispense_timer.dispense_active;
}

uint32_t Dispenser_GetTotalDispensesCompleted(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_dispenses_completed : 0;
}

uint32_t Dispenser_GetTotalVolumePurchasedMl(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_volume_purchased : 0;
}

float Dispenser_GetFlowRateLPM(void)
{
    if (!g_flow_sensor_initialized) {
        return 0.0f;
    }
    
    YS_S201_FlowData_t flow_data;
    if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) == YS_S201_OK) {
        return flow_data.flow_rate_lpm;
    }
    
    return 0.0f;
}

/**
 * @brief Initialize new customer card with balance
 * @param initial_balance_ml Initial balance in milliliters
 * @param customer_id Customer ID (0 = auto-generate)
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_InitializeNewCustomer(uint32_t initial_balance_ml, uint64_t customer_id)
{
    DISPENSER_LOG("Initializing new customer card with %lu ml balance", initial_balance_ml);
    
    MIFARE_Result_t result = MIFARE_InitializeNewCustomerCard(initial_balance_ml, customer_id, false);
    
    if (result == MIFARE_RESULT_OK) {
        DISPENSER_CRITICAL("[\u2713] New customer initialized with %lu ml balance", initial_balance_ml);
        return DISPENSER_RESULT_OK;
    }
    
    DISPENSER_ERROR("Failed to initialize customer: %s", MIFARE_GetResultString(result));
    return DISPENSER_RESULT_ERROR;
}

/**
 * @brief Add volume to card (top-up)
 * @param topup_ml Number of milliliters to add
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_TopupCard(uint32_t topup_ml)
{
    if (!MIFARE_IsCardReady()) {
        DISPENSER_ERROR("Card not ready for topup");
        return DISPENSER_RESULT_CARD_NOT_READY;
    }
    
    DISPENSER_LOG("Adding %lu ml to card", topup_ml);
    
    MIFARE_Result_t result = MIFARE_TopupCardBalance(topup_ml);
    
    if (result == MIFARE_RESULT_OK) {
        DISPENSER_CRITICAL("[\u2713] Added %lu ml to card", topup_ml);
        return DISPENSER_RESULT_OK;
    }
    
    DISPENSER_ERROR("Failed to topup card: %s", MIFARE_GetResultString(result));
    return DISPENSER_RESULT_ERROR;
}

/* UI Update Functions ------------------------------------------------------*/

void MIFARE_Dispenser_UpdateUI(void)
{
    // UI polling implementation - polls this function to update display
    // This is a placeholder - actual UI update logic goes here
}

void MIFARE_Dispenser_ResetTestMode(void)
{
    #ifdef TEST_MODE_ENABLED
    // Reset test mode balance
    DISPENSER_LOG("Resetting test mode balance");
    #endif
}

void MIFARE_Dispenser_GetTestModeStatus(void)
{
    #ifdef TEST_MODE_ENABLED
    DISPENSER_LOG("Test mode status");
    #endif
}

/* Private Helper Functions -------------------------------------------------*/

/**
 * @brief Open the water dispenser valve
 */
static void dispenser_valve_open(void)
{
    App_GPIO_Pins_t gpio_pins = Get_App_GPIO_Pins();
    gpio_put(gpio_pins.valve_control_pin, 1);  // Turn on valve
    g_dispense_timer.valve_state = VALVE_OPEN;
    DISPENSER_LOG("Valve OPEN (GPIO %lu)", gpio_pins.valve_control_pin);
}

/**
 * @brief Close the water dispenser valve
 */
static void dispenser_valve_close(void)
{
    App_GPIO_Pins_t gpio_pins = Get_App_GPIO_Pins();
    gpio_put(gpio_pins.valve_control_pin, 0);  // Turn off valve
    g_dispense_timer.valve_state = VALVE_CLOSED;
    DISPENSER_LOG("Valve CLOSED (GPIO %lu)", gpio_pins.valve_control_pin);
}

/**
 * @brief Get human-readable name for valve state
 * @param state Valve state
 * @return const char* State name string
 */
static const char* dispenser_get_valve_state_name(ValveState_t state)
{
    switch (state) {
        case VALVE_OPEN:   return "OPEN";
        case VALVE_CLOSED: return "CLOSED";
        default:           return "UNKNOWN";
    }
}

/**
 * @brief Get current valve state (for UI polling)
 * @return ValveState_t Current valve state
 */
ValveState_t Dispenser_GetValveState(void)
{
    return g_dispense_timer.valve_state;
}

/* Application Interface Implementation -------------------------------------*/

/**
 * @brief Convert DispenserResult_t to Application_Result_t
 */
Application_Result_t Dispenser_ConvertResult(DispenserResult_t result)
{
    switch (result) {
        case DISPENSER_RESULT_OK:                   return APP_RESULT_OK;
        case DISPENSER_RESULT_ERROR:                return APP_RESULT_ERROR;
        case DISPENSER_RESULT_NO_CARD:              return APP_RESULT_NO_CARD;
        case DISPENSER_RESULT_CARD_NOT_READY:       return APP_RESULT_CARD_NOT_READY;
        case DISPENSER_RESULT_INSUFFICIENT_BALANCE: return APP_RESULT_INSUFFICIENT_BALANCE;
        case DISPENSER_RESULT_BUSY:                 return APP_RESULT_BUSY;
        case DISPENSER_RESULT_CARD_ERROR:           return APP_RESULT_CARD_ERROR;
        case DISPENSER_RESULT_CARD_REMOVED:         return APP_RESULT_CARD_REMOVED;
        case DISPENSER_RESULT_TIMER_ERROR:          return APP_RESULT_TIMER_ERROR;
        case DISPENSER_RESULT_COMPLETE:             return APP_RESULT_COMPLETE;
        case DISPENSER_RESULT_TIMER_EXPIRED:        return APP_RESULT_EXPIRED;
        default:                                    return APP_RESULT_ERROR;
    }
}

/**
 * @brief Application interface: Init wrapper
 */
static Application_Result_t dispenser_app_init(void)
{
    return Dispenser_ConvertResult(MIFARE_Dispenser_Init());
}

/**
 * @brief Application interface: Get status wrapper
 */
static Application_Result_t dispenser_app_get_status(Application_Status_t *status)
{
    if (status == NULL) {
        return APP_RESULT_ERROR;
    }
    
    DispenserStatus_t disp_status;
    DispenserResult_t result = MIFARE_Dispenser_GetStatus(&disp_status);
    
    if (result != DISPENSER_RESULT_OK) {
        return Dispenser_ConvertResult(result);
    }
    
    /* Map to generic status */
    switch ((DispenserState_t)disp_status.state) {
        case DISPENSER_IDLE:
            status->state = APP_STATE_IDLE;
            break;
        case DISPENSER_DISPENSE_IN_PROGRESS:
            status->state = APP_STATE_OPERATION_ACTIVE;
            break;
        case DISPENSER_WAITING_FOR_REMOVAL:
            status->state = APP_STATE_WAITING_REMOVAL;
            break;
        default:
            status->state = APP_STATE_ERROR;
            break;
    }
    
    status->operation_active = disp_status.dispense_active;
    status->card_present = disp_status.card_present;
    status->bay_id = disp_status.dispense_bay_id;
    status->elapsed_value = disp_status.elapsed_ms / 1000;  /* Convert to seconds */
    status->remaining_value = disp_status.remaining_ml;
    
    /* Set balance info */
    status->balance.primary_value = disp_status.balance_ml;
    status->balance.secondary_value = 0;  /* No secondary value for dispenser */
    status->balance.primary_unit = "ml";
    status->balance.secondary_unit = NULL;
    
    return APP_RESULT_OK;
}

/**
 * @brief Application interface: Get state wrapper
 */
static Application_State_t dispenser_app_get_state(void)
{
    Application_Status_t status;
    if (dispenser_app_get_status(&status) == APP_RESULT_OK) {
        return status.state;
    }
    return APP_STATE_ERROR;
}

/**
 * @brief Application interface: Is operation active wrapper
 */
static bool dispenser_app_is_operation_active(void)
{
    return MIFARE_Dispenser_IsDispenseActive();
}

/**
 * @brief Application interface: Start operation wrapper
 */
static Application_Result_t dispenser_app_start_operation(void)
{
    return Dispenser_ConvertResult(MIFARE_Dispenser_StartDispense());
}

/**
 * @brief Application interface: Stop operation wrapper
 */
static Application_Result_t dispenser_app_stop_operation(void)
{
    return Dispenser_ConvertResult(MIFARE_Dispenser_EmergencyStop());
}

/**
 * @brief Application interface: Manual start wrapper
 */
static Application_Result_t dispenser_app_manual_start(uint32_t param)
{
    return Dispenser_ConvertResult(MIFARE_Dispenser_ManualStart(param));
}

/**
 * @brief Application interface: Init new customer wrapper
 */
static Application_Result_t dispenser_app_init_new_customer(uint32_t initial_value, uint64_t customer_id)
{
    return Dispenser_ConvertResult(MIFARE_Dispenser_InitializeNewCustomer(initial_value, customer_id));
}

/**
 * @brief Application interface: Topup card wrapper
 */
static Application_Result_t dispenser_app_topup_card(uint32_t amount)
{
    return Dispenser_ConvertResult(MIFARE_Dispenser_TopupCard(amount));
}

/**
 * @brief Application interface: Get primary balance wrapper
 */
static uint32_t dispenser_app_get_primary_balance(void)
{
    return Dispenser_GetBalanceMl();
}

/**
 * @brief Application interface: Get secondary balance wrapper
 */
static uint32_t dispenser_app_get_secondary_balance(void)
{
    return 0;  /* Dispenser has no secondary balance */
}

/**
 * @brief Application interface: Get status string wrapper
 */
static const char* dispenser_app_get_status_string(Application_Result_t result)
{
    return Application_GetResultString(result);
}

/**
 * @brief Application interface: Task start wrapper
 */
static void dispenser_app_task_start(void)
{
    Task_Start_Dispenser_Task();
}

/**
 * @brief Application interface: Task stop wrapper
 */
static void dispenser_app_task_stop(void)
{
    Task_Stop_Dispenser_Task();
}

/* Application Interface Callbacks -------------------------------------------*/
static const Application_Callbacks_t s_dispenser_callbacks = {
    .init = dispenser_app_init,
    .get_status = dispenser_app_get_status,
    .get_state = dispenser_app_get_state,
    .is_operation_active = dispenser_app_is_operation_active,
    .start_operation = dispenser_app_start_operation,
    .stop_operation = dispenser_app_stop_operation,
    .manual_start = dispenser_app_manual_start,
    .init_new_customer = dispenser_app_init_new_customer,
    .topup_card = dispenser_app_topup_card,
    .get_primary_balance = dispenser_app_get_primary_balance,
    .get_secondary_balance = dispenser_app_get_secondary_balance,
    .get_status_string = dispenser_app_get_status_string,
    .task_start = dispenser_app_task_start,
    .task_stop = dispenser_app_task_stop
};

/* Application Interface Instance --------------------------------------------*/
static const Application_Instance_t s_dispenser_app_instance = {
    .type = APP_TYPE_WATER_DISPENSER,
    .name = "Water Dispenser",
    .callbacks = &s_dispenser_callbacks
};

/**
 * @brief Get the application interface for dispenser
 * @return Pointer to the dispenser application instance
 */
const Application_Instance_t* Dispenser_GetApplicationInterface(void)
{
    return &s_dispenser_app_instance;
}
