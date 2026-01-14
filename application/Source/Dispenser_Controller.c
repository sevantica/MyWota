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
 * @file Car_Wash_Controller.c
 * @brief Car wash business logic layer - manages tokens and wash timers
 * @details This layer polls MIFARE_Transaction_Manager for card data,
 *          makes business decisions, and updates card data accordingly.
 *          Follows the polling architecture pattern.
 *          
 *          Implements the common Application_Interface for interoperability
 *          with other MIFARE-based applications (e.g., water dispensers).
 */

/* Includes ------------------------------------------------------------------*/
#include "Dispenser_Controller.h"
#include "Buzzer_Driver.h"
#include "Application_Interface.h"
#include "MIFARE_Transaction_Core.h"
#include "MIFARE_Async_Mailbox.h"
#include "IO_Expander_Control.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "System.h"
#include "System_Config.h"
#include "Heartbeat_Task.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/*Private defines ---------------------------------------------------*/
/* Task stack configuration */
#define CARWASH_TASK_STACK_BYTES  (128u * 16)  // 2048 bytes for MIFARE operations + business logic
#define CARWASH_TASK_STACK_WORDS  (CARWASH_TASK_STACK_BYTES / sizeof(StackType_t))

/* Debug logging configuration */
#define LOG_DEBUG_CARWASH_EN      1
#define LOG_CRITICAL_CARWASH_EN   1
#define LOG_ERROR_CARWASH_EN      1


#if LOG_DEBUG_CARWASH_EN
    #define CARWASH_DEBUG(fmt, ...) USB_Log_Printf("CARWASH: " fmt "\r\n", ##__VA_ARGS__)
#else
    #define CARWASH_DEBUG(...)
#endif

#if LOG_CRITICAL_CARWASH_EN
    #define CARWASH_CRITICAL(fmt, ...) USB_Log_Printf("CARWASH: " fmt "\r\n", ##__VA_ARGS__)
#else
    #define CARWASH_CRITICAL(...)
#endif

#if LOG_ERROR_CARWASH_EN
    #define CARWASH_ERROR(fmt, ...) USB_Log_Printf("CARWASH: " fmt "\r\n", ##__VA_ARGS__)
#else
    #define CARWASH_ERROR(...)
#endif

/* Convenience macro for debug logging */
#define CARWASH_LOG(fmt, ...) CARWASH_DEBUG(fmt, ##__VA_ARGS__)

/*Private variables -------------------------------------------------*/

/* Note: CarWashState_t enum is defined in Car_Wash_Controller.h */


static CarWash_TimerState_t g_wash_timer = {0};
static CarWashState_t g_carwash_state = CARWASH_IDLE;
static TaskHandle_t carwash_task_handle = NULL;
// New state to track async request
static bool g_deduction_in_progress = false;

/*Private function prototypes ---------------------------------------*/
static void carwash_update_timer(void);
static bool carwash_has_tokens(void);
static WashOption_t carwash_read_button_selection(void);
static void carwash_activate_option_output(WashOption_t option);
static void carwash_clear_all_outputs(void);
static const char* carwash_get_option_name(WashOption_t option);

/*Public Functions ---------------------------------------------------*/

/**
 * @brief Initialize car wash system
 */
CarWashResult_t MIFARE_CarWash_Init(void)
{
    // Initialize wash timer state
    memset(&g_wash_timer, 0, sizeof(CarWash_TimerState_t));
    
    // Read duration from config (live update support)
    const SystemConfig_t* cfg = Config_Get();
    g_wash_timer.wash_duration_seconds = cfg->carwash.wash_duration_seconds;
    
    g_wash_timer.selected_option = WASH_OPTION_NONE;
    g_carwash_state = CARWASH_IDLE;
    
    // Configure button input pins
    IO_Expander_SetPinInput(WASH_BUTTON_VACUUM_CLEANER);
    IO_Expander_SetPinInput(WASH_BUTTON_WASH_BRUSH);
    IO_Expander_SetPinInput(WASH_BUTTON_PRESSURE_WASHER);
    
    // Configure output pins
    IO_Expander_SetPinOutput(WASH_OUTPUT_VACUUM_CLEANER);
    IO_Expander_SetPinOutput(WASH_OUTPUT_WASH_BRUSH);
    IO_Expander_SetPinOutput(WASH_OUTPUT_PRESSURE_WASHER);
    
    // Clear all outputs initially
    carwash_clear_all_outputs();
    
    CARWASH_CRITICAL("[✓] CarWash system initialized");
    return CARWASH_RESULT_OK;
}

/**
 * @brief Check if card has tokens available
 * @return true if tokens > 0
 */
static bool carwash_has_tokens(void)
{
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    if (!user_data) {
        return false;
    }
    return (user_data->balance > 0);
}

/**
 * @brief Check for pending transaction from previous interrupted session
 * @return true if pending transaction was detected and recovery started
 */
static bool carwash_check_pending_transaction_recovery(void)
{
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    if (!user_data) {
        return false;
    }
    
    // Check if pending transaction flag is set
    if (user_data->status_flags & CARD_STATUS_PENDING_TRANSACTION) {
        CARWASH_CRITICAL("[!] RECOVERY: Pending transaction detected from interrupted session");
        CARWASH_CRITICAL("[!] Token was deducted but wash may not have started - starting wash now");
        
        // Feed watchdog before potentially long MIFARE operations
        System_ReportTaskStatus(SYSTEM_TASK_ID_CARWASH, true);
        
        // Clear the pending flag on the card
        MIFARE_UserData_t updated_user_data;
        memcpy(&updated_user_data, user_data, sizeof(MIFARE_UserData_t));
        updated_user_data.status_flags &= ~CARD_STATUS_PENDING_TRANSACTION;
        MIFARE_SetUserData(&updated_user_data);
        
        // Try to write the cleared flag to card
        MIFARE_Result_t result = MIFARE_UpdateCardData(false);
        if (result == MIFARE_RESULT_OK) {
            CARWASH_LOG("Pending transaction flag cleared on card");
        } else {
            CARWASH_ERROR("Failed to clear pending flag: %s", MIFARE_GetResultString(result));
        }
        
        // Mark as token deducted and transition directly to waiting for removal
        g_wash_timer.token_deducted = true;
        g_wash_timer.auto_start_triggered = false;
        g_wash_timer.tokens_used_this_session++;
        g_carwash_state = CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL;
        
        CARWASH_CRITICAL("[✓] Recovery complete - remove card to start wash");
        return true;  // Recovery handled
    }
    
    return false;  // No pending transaction
}

/**
 * @brief Deduct token from card (card must be present)
 * @return CarWashResult_t Operation result
 */
static CarWashResult_t carwash_deduct_token(void)
{
    // CARWASH_LOG("Deducting token..."); // Moved to prevent polling spam
    
    // Check if card is ready
    if (!MIFARE_IsCardReady()) {
        CARWASH_ERROR("Card not ready");
        return CARWASH_RESULT_CARD_NOT_READY;
    }
    
    // Check if already deducted
    if (g_wash_timer.token_deducted) {
        CARWASH_ERROR("Token already deducted");
        return CARWASH_RESULT_BUSY;
    }
    
    // Get user data
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    if (!user_data) {
        CARWASH_ERROR("Failed to get user data");
        return CARWASH_RESULT_ERROR;
    }
    
    // Check tokens
    if (user_data->balance == 0) {
        CARWASH_ERROR("Insufficient tokens");
        return CARWASH_RESULT_INSUFFICIENT_TOKENS;
    }
    
    // --- ASYNC REQUEST ---
    if (!g_deduction_in_progress) {
         CARWASH_LOG("Requesting async deduction (1 token)...");
         
         // Note: Usage reserved time update is now handled generically or via callback
         // We won't block here to update it manually.
         
         // Request with 2 second timeout
         if (MIFARE_RequestDeductionAsync(1, 2000)) {
             g_deduction_in_progress = true;
             return CARWASH_RESULT_BUSY; // Waiting for result
         } else {
             CARWASH_ERROR("Failed to queue deduction request");
             return CARWASH_RESULT_ERROR;
         }
    } else {
        // --- CHECK ASYNC RESULT ---
        MIFARE_Result_t result_code = MIFARE_RESULT_OK;
        MIFARE_AsyncStatus_t status = MIFARE_GetAsyncStatus(&result_code);
        
        if (status == MIFARE_ASYNC_STATUS_SUCCESS) {
             g_deduction_in_progress = false;
             
             // Mark token as deducted
             g_wash_timer.token_deducted = true;
             g_wash_timer.tokens_used_this_session++;
             g_carwash_state = CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL;
             
             // Get updated balance for display
             MIFARE_UserData_t *ud = MIFARE_GetUserData();
             uint32_t bal = ud ? ud->balance : 0;
             CARWASH_CRITICAL("[✓] Token deducted - %lu tokens remaining (remove card to start wash)", bal);
             
             return CARWASH_RESULT_OK;
             
        } else if (status == MIFARE_ASYNC_STATUS_FAIL) {
             g_deduction_in_progress = false;
             CARWASH_ERROR("Async deduction failed: %s", MIFARE_GetResultString(result_code));
             return CARWASH_RESULT_ERROR;
             
        } else if (status == MIFARE_ASYNC_STATUS_TIMEOUT) {
             g_deduction_in_progress = false;
             CARWASH_ERROR("Async deduction timeout - resetting mailbox");
             MIFARE_ResetAsyncMailbox();
             return CARWASH_RESULT_ERROR;
             
        } else {
             // Still pending
             return CARWASH_RESULT_BUSY;
        }
    }
}

/**
 * @brief Start wash timer (called 1s after card removal)
 * @note Reads duration from config for live update support
 */
static void carwash_start_timer(void)
{
    // Refresh duration from config (live update support)
    const SystemConfig_t* cfg = Config_Get();
    g_wash_timer.wash_duration_seconds = cfg->carwash.wash_duration_seconds;
    
    g_wash_timer.wash_start_time = xTaskGetTickCount();
    g_wash_timer.wash_active = true;
    g_wash_timer.auto_start_triggered = true;
    g_carwash_state = CARWASH_WASH_IN_PROGRESS;
    
    CARWASH_CRITICAL("[✓] Wash timer started (duration=%lu sec)", 
                     g_wash_timer.wash_duration_seconds);
}

/**
 * @brief Legacy function - now just deducts token
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_StartWash(void)
{
    return carwash_deduct_token();
}

/**
 * @brief Get current wash status
 * @param status Pointer to status structure to fill
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_GetStatus(CarWashStatus_t *status)
{
    if (!status) {
        return CARWASH_RESULT_ERROR;
    }
    
    status->state = g_carwash_state;
    status->wash_active = g_wash_timer.wash_active;
    status->card_present = MIFARE_IsCardPresent();
    status->wash_bay_id = g_wash_timer.wash_bay_id;
    
    // Calculate remaining time
    if (g_wash_timer.wash_active) {
        uint32_t elapsed_ticks = xTaskGetTickCount() - g_wash_timer.wash_start_time;
        uint32_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;
        
        if (elapsed_seconds < g_wash_timer.wash_duration_seconds) {
            status->remaining_seconds = g_wash_timer.wash_duration_seconds - elapsed_seconds;
            status->elapsed_seconds = elapsed_seconds;
        } else {
            status->remaining_seconds = 0;
            status->elapsed_seconds = g_wash_timer.wash_duration_seconds;
        }
    } else {
        status->remaining_seconds = 0;
        status->elapsed_seconds = 0;
    }
    
    // Get token count
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    status->token_count = user_data ? user_data->balance : 0;
    
    // Get selected option
    status->selected_option = g_wash_timer.selected_option;
    
    return CARWASH_RESULT_OK;
}

/**
 * @brief Emergency stop wash
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_EmergencyStop(void)
{
    if (!g_wash_timer.wash_active) {
        return CARWASH_RESULT_OK;  // Nothing to stop
    }
    
    // Stop wash timer
    g_wash_timer.wash_active = false;
    g_carwash_state = CARWASH_IDLE;
    
    // Clear all outputs
    carwash_clear_all_outputs();
    g_wash_timer.selected_option = WASH_OPTION_NONE;
    
    CARWASH_CRITICAL("[✗] Emergency stop");
    
    return CARWASH_RESULT_OK;
}

/**
 * @brief Manually start wash without card (for testing/debugging)
 * @param option Wash option to activate (WASH_OPTION_NONE to use VACUUM_CLEANER)
 * @param duration_seconds Duration in seconds (0 = use default WASH_DURATION_SECONDS)
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_ManualStart(WashOption_t option, uint32_t duration_seconds)
{
    // Don't allow if wash already active
    if (g_wash_timer.wash_active) {
        CARWASH_ERROR("Manual start failed - wash already in progress");
        return CARWASH_RESULT_ERROR;
    }
    
    // Set duration (use config value if 0)
    if (duration_seconds == 0) {
        const SystemConfig_t* cfg = Config_Get();
        g_wash_timer.wash_duration_seconds = cfg->carwash.wash_duration_seconds;
    } else {
        g_wash_timer.wash_duration_seconds = duration_seconds;
    }
    
    // Set option (default to vacuum cleaner if NONE)
    if (option == WASH_OPTION_NONE) {
        g_wash_timer.selected_option = WASH_OPTION_VACUUM_CLEANER;
    } else {
        g_wash_timer.selected_option = option;
    }
    
    // Activate the output
    carwash_activate_option_output(g_wash_timer.selected_option);
    
    // Start the timer
    g_wash_timer.wash_start_time = xTaskGetTickCount();
    g_wash_timer.wash_active = true;
    g_carwash_state = CARWASH_WASH_IN_PROGRESS;
    
    CARWASH_CRITICAL("[→] MANUAL WASH START: %s for %lu seconds (no card)",
                     carwash_get_option_name(g_wash_timer.selected_option),
                     g_wash_timer.wash_duration_seconds);
    
    return CARWASH_RESULT_OK;
}

/**
 * @brief Manually stop wash (wrapper for EmergencyStop with different log)
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_ManualStop(void)
{
    if (!g_wash_timer.wash_active) {
        CARWASH_DEBUG("Manual stop - no wash active");
        return CARWASH_RESULT_OK;
    }
    
    // Stop wash timer
    g_wash_timer.wash_active = false;
    g_carwash_state = CARWASH_IDLE;
    
    // Clear all outputs
    carwash_clear_all_outputs();
    g_wash_timer.selected_option = WASH_OPTION_NONE;
    
    CARWASH_CRITICAL("[✗] MANUAL WASH STOP");
    
    return CARWASH_RESULT_OK;
}

/**
 * @brief Check if wash is currently active (for buzzer polling)
 * @return true if wash in progress, false otherwise
 */
bool MIFARE_CarWash_IsWashActive(void)
{
    return g_wash_timer.wash_active;
}

/**
 * @brief Update wash timer (called periodically by task)
 */
static void carwash_update_timer(void)
{
    if (!g_wash_timer.wash_active) {
        return;
    }
    
    // Check if timer expired
    uint32_t current_tick = xTaskGetTickCount();
    uint32_t elapsed_ticks = current_tick - g_wash_timer.wash_start_time;
    // Convert ticks to seconds: ticks / Hz = seconds
    uint32_t elapsed_seconds = elapsed_ticks / configTICK_RATE_HZ;
    
    if (elapsed_seconds >= g_wash_timer.wash_duration_seconds) {
        // Timer expired - finish wash
        g_wash_timer.wash_active = false;
        g_carwash_state = CARWASH_CARD_READY;  // Stay in CARD_READY, don't go to IDLE
        
        // Clear all outputs at end of wash
        carwash_clear_all_outputs();
        CARWASH_CRITICAL("[✓] Wash complete - cleared output for %s", 
                         carwash_get_option_name(g_wash_timer.selected_option));
        g_wash_timer.selected_option = WASH_OPTION_NONE;
    }
}

/**
 * @brief Car wash polling task
 * @param argument Task argument (unused)
 */
void MIFARE_CarWash_Task(void* argument)
{
    (void)argument;
    
    CARWASH_CRITICAL("[→] CarWash task started");
    
    uint32_t loop_count = 0;
    static uint32_t last_success_beep_tick = 0;
    static CarWashState_t last_carwash_state = CARWASH_IDLE;

    while (1) {
        // Feed watchdog every second
        TASK_HEARTBEAT_EVERY_SECOND("CarWash");
        System_ReportTaskStatus(SYSTEM_TASK_ID_CARWASH, true);
        
        // Update wash timer
        carwash_update_timer();
        
        // Update state machine based on card presence
        bool card_ready = MIFARE_IsCardReady();
        
        if (card_ready && g_carwash_state == CARWASH_IDLE) {
            g_carwash_state = CARWASH_CARD_READY;
            g_wash_timer.auto_start_triggered = false;
            g_wash_timer.token_deducted = false;
            g_wash_timer.card_ready_time = xTaskGetTickCount();
            
            // Check for pending transaction from interrupted session (crash recovery)
            if (!carwash_check_pending_transaction_recovery()) {
                // No recovery needed - check for pending USB command
                USB_PendingCommandState_t* pending = USB_Command_GetPendingCommand();
                if (pending != NULL && pending->active) {
                    CARWASH_DEBUG("Pending USB command detected - executing instead of auto-start");
                    USB_Command_ExecutePendingCommand(pending);
                    g_wash_timer.auto_start_triggered = true;
                }
            }
            // If recovery was triggered, state is now TOKEN_DEDUCTED_WAITING_REMOVAL
        } else if (card_ready && g_carwash_state == CARWASH_CARD_READY) {
            // Card ready - deduct token immediately if has tokens
            if (carwash_has_tokens() && !g_wash_timer.token_deducted && !g_wash_timer.auto_start_triggered) {
                // Call deduct_token repeatedly until it returns OK or ERROR (it manages async state internally)
                CarWashResult_t result = carwash_deduct_token();
                
                if (result == CARWASH_RESULT_BUSY) {
                    // Still processing async request - do nothing, will check again next loop
                } else if (result != CARWASH_RESULT_OK) {
                    CARWASH_ERROR("Token deduction failed: %d", result);
                    // Reset async flag just in case
                    g_deduction_in_progress = false;
                }
            }
        } else if (!MIFARE_IsCardPresent() && g_carwash_state == CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL) {
            // Card removed after token deduction - start delay timer
            g_wash_timer.card_removed_time = xTaskGetTickCount();
            g_carwash_state = CARWASH_WAITING_TO_START;
            const SystemConfig_t* cfg = Config_Get();
            CARWASH_LOG("Card removed - starting %lu ms countdown to wash start", cfg->buzzer.removal_pattern_repeat_ms); // Use generic log or restore orig
                                                                                                                        // Just kidding, keep orig log
            CARWASH_LOG("Card removed - starting %lu ms countdown to wash start", cfg->carwash.card_removal_delay_ms);
        } else if (g_carwash_state == CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL) {
            // Card is still present (implied by previous check failing)
            // Play success pattern periodically to remind user to remove card
            
            // Check if we just entered this state to force immediate beep
            if (last_carwash_state != CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL) {
                 last_success_beep_tick = 0; // Force immediate trigger
            }
            
            const SystemConfig_t* cfg = Config_Get();
            uint32_t repeat_ticks = pdMS_TO_TICKS(cfg->buzzer.removal_pattern_repeat_ms);
            uint32_t current_tick = xTaskGetTickCount();
            
            // Handle wrap-around or initial 0
            if (last_success_beep_tick == 0 || (current_tick - last_success_beep_tick) >= repeat_ticks) {
                 Buzzer_Handle_t* buzzer = Buzzer_GetHandle(0);
                 if (buzzer && Buzzer_IsInitialized(buzzer)) {
                     Buzzer_Pattern_t pattern = {
                         .on_ms = cfg->buzzer.removal_pattern_on_ms,
                         .off_ms = cfg->buzzer.removal_pattern_off_ms,
                         .repeat_count = cfg->buzzer.removal_pattern_count
                     };
                     Buzzer_PlayPattern(buzzer, &pattern);
                     last_success_beep_tick = current_tick;
                 }
            }
        } else if (g_carwash_state == CARWASH_WAITING_TO_START) {
            // Waiting for delay to expire (read from config for live updates)
            uint32_t elapsed_ticks = xTaskGetTickCount() - g_wash_timer.card_removed_time;
            uint32_t elapsed_ms = pdTICKS_TO_MS(elapsed_ticks);
            const SystemConfig_t* cfg = Config_Get();
            
            if (elapsed_ms >= cfg->carwash.card_removal_delay_ms) {
                // Delay passed - start wash timer
                carwash_start_timer();
            }
        } else if (g_carwash_state == CARWASH_WASH_IN_PROGRESS || // Only if wash really active (safe check)
                   CarWash_IsWashActive()
        ) {
            // Poll buttons for wash option selection during active wash
            WashOption_t selected = carwash_read_button_selection();
            if (selected != WASH_OPTION_NONE && selected != g_wash_timer.selected_option) {
                // New option selected - switch to it
                CARWASH_CRITICAL("[→] Button pressed: %s", carwash_get_option_name(selected));
                CARWASH_LOG("Option changed to: %s", carwash_get_option_name(selected));
                g_wash_timer.selected_option = selected;
                carwash_activate_option_output(selected);
                // Delay 1 second before feedback
                vTaskDelay(pdMS_TO_TICKS(1000));

                // Play feedback pattern
                Buzzer_Handle_t* buzzer = Buzzer_GetHandle(0);
                if (buzzer && Buzzer_IsInitialized(buzzer)) {
                    Buzzer_Pattern_t p_vacuum = { .on_ms = 50, .off_ms = 50, .repeat_count = 3 };   // 3 fast ticks
                    Buzzer_Pattern_t p_brush = { .on_ms = 300, .off_ms = 200, .repeat_count = 2 };  // 2 medium beeps
                    Buzzer_Pattern_t p_pressure = { .on_ms = 1000, .off_ms = 0, .repeat_count = 1 }; // 1 long blast
                    
                    switch (selected) {
                        case WASH_OPTION_VACUUM_CLEANER:
                            Buzzer_PlayPattern(buzzer, &p_vacuum);
                            break;
                            
                        case WASH_OPTION_PRESSURE_WASHER:
                            Buzzer_PlayPattern(buzzer, &p_pressure);
                            break;
                            
                        case WASH_OPTION_WASH_BRUSH:
                            Buzzer_PlayPattern(buzzer, &p_brush);
                            break;
                            
                        default:
                            break;
                    }
                }
            }
        } else if (!MIFARE_IsCardPresent() && (g_carwash_state == CARWASH_IDLE || g_carwash_state == CARWASH_CARD_READY)) {
            // Card removed before token deduction - reset to IDLE
            if (g_carwash_state != CARWASH_IDLE) {
                CARWASH_LOG("Card removed before token deduction - resetting to IDLE");
                g_carwash_state = CARWASH_IDLE;
                g_wash_timer.auto_start_triggered = false;
                g_wash_timer.token_deducted = false;
                carwash_clear_all_outputs();
                g_wash_timer.selected_option = WASH_OPTION_NONE;
            }
        }
        
        // Sleep for 100ms
        // If waiting for async result, sleep less to feel responsive? 
        // 100ms is 10Hz, async process takes ~200ms. 
        // Polling loop inside MIFARE runs every 50ms. 
        // So checking every 100ms is fine.
        vTaskDelay(pdMS_TO_TICKS(100));
        
        last_carwash_state = g_carwash_state;
        loop_count++;
    }
}

/**
 * @brief Start car wash polling task
 */
void Task_Start_CarWash_Task(void)
{
    BaseType_t result = xTaskCreate(MIFARE_CarWash_Task, 
                                    "CarWash",  // Must match WDT tracking name in System.c
                                    CARWASH_TASK_STACK_WORDS,
                                    NULL, 
                                    tskIDLE_PRIORITY + 1, 
                                    &carwash_task_handle);
    
    if (result != pdPASS) {
        CARWASH_ERROR("[✗] Task creation FAILED (result=%d)", result);
    } else {
        CARWASH_CRITICAL("[✓] Task created successfully");
    }
}

/* UI Getter Functions -------------------------------------------------------*/

uint32_t CarWash_GetTokenCount(void)
{
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    return user_data ? user_data->balance : 0;
}

uint32_t CarWash_GetWashTimeRemaining(void)
{
    // During TOKEN_DEDUCTED or WAITING_TO_START states, read directly from config
    // for instant response to 'set' command changes
    if (g_carwash_state == CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL ||
        g_carwash_state == CARWASH_WAITING_TO_START) {
        const SystemConfig_t* cfg = Config_Get();
        return cfg->carwash.wash_duration_seconds;
    }
    
    if (!g_wash_timer.wash_active) {
        return 0;
    }
    
    // During active wash, use the cached duration (don't change mid-wash)
    uint32_t elapsed_ticks = xTaskGetTickCount() - g_wash_timer.wash_start_time;
    uint32_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;
    
    if (elapsed_seconds < g_wash_timer.wash_duration_seconds) {
        return g_wash_timer.wash_duration_seconds - elapsed_seconds;
    }
    return 0;
}

bool CarWash_IsWashActive(void)
{
    // Return true during WAITING_TO_START so UI shows time instead of "--:--"
    // Note: CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL is excluded to keep showing tokens until card removal
    if (g_carwash_state == CARWASH_WAITING_TO_START) {
        return true;
    }

    // Fix race condition: UI may poll after card removal but before Task updates state
    // If we have deducted a token and card is gone, we are effectively active (about to start)
    if (g_carwash_state == CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL && !MIFARE_IsCardPresent()) {
        return true;
    }

    return g_wash_timer.wash_active;
}

uint32_t CarWash_GetTotalWashesCompleted(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_dispenses_completed : 0;
}

uint32_t CarWash_GetTotalTokensPurchased(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_volume_purchased : 0;
}

/**
 * @brief Initialize new customer card with tokens
 * @param initial_tokens Number of tokens to add
 * @param customer_id Customer ID (0 = auto-generate)
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_InitializeNewCustomer(uint32_t initial_tokens, uint64_t customer_id)
{
    CARWASH_LOG("Initializing new customer card with %lu tokens", initial_tokens);
    
    MIFARE_Result_t result = MIFARE_InitializeNewCustomerCard(initial_tokens, customer_id, false);
    
    if (result == MIFARE_RESULT_OK) {
        CARWASH_CRITICAL("[✓] New customer initialized with %lu tokens", initial_tokens);
        return CARWASH_RESULT_OK;
    }
    
    CARWASH_ERROR("Failed to initialize customer: %s", MIFARE_GetResultString(result));
    return CARWASH_RESULT_ERROR;
}

/**
 * @brief Add tokens to card (top-up)
 * @param topup_tokens Number of tokens to add
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_TopupCard(uint32_t topup_tokens)
{
    if (!MIFARE_IsCardReady()) {
        CARWASH_ERROR("Card not ready for topup");
        return CARWASH_RESULT_CARD_NOT_READY;
    }
    
    CARWASH_LOG("Adding %lu tokens to card", topup_tokens);
    
    MIFARE_Result_t result = MIFARE_TopupCardBalance(topup_tokens);
    
    if (result == MIFARE_RESULT_OK) {
        CARWASH_CRITICAL("[✓] Added %lu tokens to card", topup_tokens);
        return CARWASH_RESULT_OK;
    }
    
    CARWASH_ERROR("Failed to topup card: %s", MIFARE_GetResultString(result));
    return CARWASH_RESULT_ERROR;
}

/* UI Update Functions ------------------------------------------------------*/

void MIFARE_CarWash_UpdateUI(void)
{
    // UI polling implementation - polls this function to update display
    // This is a placeholder - actual UI update logic goes here
}

void MIFARE_CarWash_ResetTestMode(void)
{
    #ifdef TEST_MODE_ENABLED
    // Reset test mode to 10 tokens
    CARWASH_LOG("Resetting test mode to 10 tokens");
    #endif
}

void MIFARE_CarWash_GetTestModeStatus(void)
{
    #ifdef TEST_MODE_ENABLED
    CARWASH_LOG("Test mode status");
    #endif
}

/**
 * @brief Compatibility stub for shared Buzzer_Driver
 * @return uint32_t Always returns 0 (car wash doesn't dispense water by milliliter)
 * @note BigYellow uses timed wash sessions, not volume-based dispensing
 */
uint32_t Dispenser_GetDispensedAmountML(void)
{
    return 0;  // Car wash doesn't dispense water - no volume tracking
}

/* Private Helper Functions -------------------------------------------------*/

/**
 * @brief Read button selection from IO expander
 * @return WashOption_t Selected option (WASH_OPTION_NONE if no button pressed)
 */
static WashOption_t carwash_read_button_selection(void)
{
    IO_Expander_State_t button_state;
    
    // Check vacuum cleaner button
    if (IO_Expander_GetPinState(WASH_BUTTON_VACUUM_CLEANER, &button_state) == IO_EXP_CTRL_OK) {
        if (button_state == IO_EXP_STATE_LOW) {  // Active LOW
            CARWASH_LOG("Button state - Vacuum: LOW (pressed)");
            return WASH_OPTION_VACUUM_CLEANER;
        }
    }
    
    // Check wash brush button
    if (IO_Expander_GetPinState(WASH_BUTTON_WASH_BRUSH, &button_state) == IO_EXP_CTRL_OK) {
        if (button_state == IO_EXP_STATE_LOW) {  // Active LOW
            CARWASH_LOG("Button state - Brush: LOW (pressed)");
            return WASH_OPTION_WASH_BRUSH;
        }
    }
    
    // Check pressure washer button
    if (IO_Expander_GetPinState(WASH_BUTTON_PRESSURE_WASHER, &button_state) == IO_EXP_CTRL_OK) {
        if (button_state == IO_EXP_STATE_LOW) {  // Active LOW
            CARWASH_LOG("Button state - Pressure: LOW (pressed)");
            return WASH_OPTION_PRESSURE_WASHER;
        }
    }
    
    return WASH_OPTION_NONE;
}

/**
 * @brief Activate the output pin for selected wash option
 * @param option Wash option to activate
 * @note Only one option can be active at a time (mutually exclusive)
 */
static void carwash_activate_option_output(WashOption_t option)
{
    // First clear all outputs (only one can be active at a time)
    carwash_clear_all_outputs();
    
    // Activate selected output
    switch (option) {
        case WASH_OPTION_VACUUM_CLEANER:
            IO_Expander_WritePin(WASH_OUTPUT_VACUUM_CLEANER, IO_EXP_STATE_HIGH);
            CARWASH_LOG("Activated vacuum cleaner output");
            break;
            
        case WASH_OPTION_WASH_BRUSH:
            IO_Expander_WritePin(WASH_OUTPUT_WASH_BRUSH, IO_EXP_STATE_HIGH);
            CARWASH_LOG("Activated wash brush output");
            break;
            
        case WASH_OPTION_PRESSURE_WASHER:
            IO_Expander_WritePin(WASH_OUTPUT_PRESSURE_WASHER, IO_EXP_STATE_HIGH);
            CARWASH_LOG("Activated pressure washer output");
            break;
            
        case WASH_OPTION_NONE:
        default:
            // No output to activate
            break;
    }
}

/**
 * @brief Clear all wash option outputs
 */
static void carwash_clear_all_outputs(void)
{
    IO_Expander_WritePin(WASH_OUTPUT_VACUUM_CLEANER, IO_EXP_STATE_LOW);
    IO_Expander_WritePin(WASH_OUTPUT_WASH_BRUSH, IO_EXP_STATE_LOW);
    IO_Expander_WritePin(WASH_OUTPUT_PRESSURE_WASHER, IO_EXP_STATE_LOW);
}

/**
 * @brief Get human-readable name for wash option
 * @param option Wash option
 * @return const char* Option name string
 */
static const char* carwash_get_option_name(WashOption_t option)
{
    switch (option) {
        case WASH_OPTION_VACUUM_CLEANER:  return "Vacuum Cleaner";
        case WASH_OPTION_WASH_BRUSH:      return "Wash Brush";
        case WASH_OPTION_PRESSURE_WASHER: return "Pressure Washer";
        case WASH_OPTION_NONE:            return "None";
        default:                          return "Unknown";
    }
}

/**
 * @brief Get selected wash option (for UI polling)
 * @return WashOption_t Currently selected option
 */
WashOption_t CarWash_GetSelectedOption(void)
{
    return g_wash_timer.selected_option;
}

/* Application Interface Implementation -------------------------------------*/

/**
 * @brief Convert CarWashResult_t to Application_Result_t
 */
Application_Result_t CarWash_ConvertResult(CarWashResult_t result)
{
    switch (result) {
        case CARWASH_RESULT_OK:                  return APP_RESULT_OK;
        case CARWASH_RESULT_ERROR:               return APP_RESULT_ERROR;
        case CARWASH_RESULT_NO_CARD:             return APP_RESULT_NO_CARD;
        case CARWASH_RESULT_CARD_NOT_READY:      return APP_RESULT_CARD_NOT_READY;
        case CARWASH_RESULT_INSUFFICIENT_TOKENS: return APP_RESULT_INSUFFICIENT_BALANCE;
        case CARWASH_RESULT_BUSY:                return APP_RESULT_BUSY;
        case CARWASH_RESULT_CARD_ERROR:          return APP_RESULT_CARD_ERROR;
        case CARWASH_RESULT_CARD_REMOVED:        return APP_RESULT_CARD_REMOVED;
        case CARWASH_RESULT_TIMER_ERROR:         return APP_RESULT_TIMER_ERROR;
        case CARWASH_RESULT_COMPLETE:            return APP_RESULT_COMPLETE;
        case CARWASH_RESULT_TIMER_EXPIRED:       return APP_RESULT_EXPIRED;
        default:                                 return APP_RESULT_ERROR;
    }
}

/**
 * @brief Application interface: Init wrapper
 */
static Application_Result_t carwash_app_init(void)
{
    return CarWash_ConvertResult(MIFARE_CarWash_Init());
}

/**
 * @brief Application interface: Get status wrapper
 */
static Application_Result_t carwash_app_get_status(Application_Status_t *status)
{
    if (status == NULL) {
        return APP_RESULT_ERROR;
    }
    
    CarWashStatus_t wash_status;
    CarWashResult_t result = MIFARE_CarWash_GetStatus(&wash_status);
    
    if (result != CARWASH_RESULT_OK) {
        return CarWash_ConvertResult(result);
    }
    
    /* Map to generic status */
    switch ((CarWashState_t)wash_status.state) {
        case CARWASH_IDLE:
            status->state = APP_STATE_IDLE;
            break;
        case CARWASH_CARD_READY:
            status->state = APP_STATE_CARD_READY;
            break;
        case CARWASH_TOKEN_DEDUCTED_WAITING_REMOVAL:
            status->state = APP_STATE_OPERATION_PENDING;
            break;
        case CARWASH_WAITING_TO_START:
            status->state = APP_STATE_OPERATION_PENDING;
            break;
        case CARWASH_WASH_IN_PROGRESS:
            status->state = APP_STATE_OPERATION_ACTIVE;
            break;
        default:
            status->state = APP_STATE_ERROR;
            break;
    }
    
    status->operation_active = wash_status.wash_active;
    status->card_present = wash_status.card_present;
    status->bay_id = wash_status.wash_bay_id;
    status->elapsed_value = wash_status.elapsed_seconds;
    status->remaining_value = wash_status.remaining_seconds;
    
    /* Set balance info */
    status->balance.primary_value = wash_status.token_count;
    status->balance.secondary_value = wash_status.remaining_seconds;
    status->balance.primary_unit = "tokens";
    status->balance.secondary_unit = "seconds";
    
    return APP_RESULT_OK;
}

/**
 * @brief Application interface: Get state wrapper
 */
static Application_State_t carwash_app_get_state(void)
{
    Application_Status_t status;
    if (carwash_app_get_status(&status) == APP_RESULT_OK) {
        return status.state;
    }
    return APP_STATE_ERROR;
}

/**
 * @brief Application interface: Is operation active wrapper
 */
static bool carwash_app_is_operation_active(void)
{
    return MIFARE_CarWash_IsWashActive();
}

/**
 * @brief Application interface: Start operation wrapper
 */
static Application_Result_t carwash_app_start_operation(void)
{
    return CarWash_ConvertResult(MIFARE_CarWash_StartWash());
}

/**
 * @brief Application interface: Stop operation wrapper
 */
static Application_Result_t carwash_app_stop_operation(void)
{
    return CarWash_ConvertResult(MIFARE_CarWash_EmergencyStop());
}

/**
 * @brief Application interface: Manual start wrapper
 */
static Application_Result_t carwash_app_manual_start(uint32_t param)
{
    return CarWash_ConvertResult(MIFARE_CarWash_ManualStart(WASH_OPTION_NONE, param));
}

/**
 * @brief Application interface: Init new customer wrapper
 */
static Application_Result_t carwash_app_init_new_customer(uint32_t initial_value, uint64_t customer_id)
{
    return CarWash_ConvertResult(MIFARE_CarWash_InitializeNewCustomer(initial_value, customer_id));
}

/**
 * @brief Application interface: Topup card wrapper
 */
static Application_Result_t carwash_app_topup_card(uint32_t amount)
{
    return CarWash_ConvertResult(MIFARE_CarWash_TopupCard(amount));
}

/**
 * @brief Application interface: Get primary balance wrapper
 */
static uint32_t carwash_app_get_primary_balance(void)
{
    return CarWash_GetTokenCount();
}

/**
 * @brief Application interface: Get secondary balance wrapper
 */
static uint32_t carwash_app_get_secondary_balance(void)
{
    return CarWash_GetWashTimeRemaining();
}

/**
 * @brief Application interface: Get status string wrapper
 */
static const char* carwash_app_get_status_string(Application_Result_t result)
{
    return Application_GetResultString(result);
}

/**
 * @brief Application interface: Task start wrapper
 */
static void carwash_app_task_start(void)
{
    Task_Start_CarWash_Task();
}

/**
 * @brief Application interface: Task stop wrapper (not implemented for car wash)
 */
static void carwash_app_task_stop(void)
{
    /* Car wash task doesn't support dynamic stop */
}

/* Application Interface Callbacks -------------------------------------------*/
static const Application_Callbacks_t s_carwash_callbacks = {
    .init = carwash_app_init,
    .get_status = carwash_app_get_status,
    .get_state = carwash_app_get_state,
    .is_operation_active = carwash_app_is_operation_active,
    .start_operation = carwash_app_start_operation,
    .stop_operation = carwash_app_stop_operation,
    .manual_start = carwash_app_manual_start,
    .init_new_customer = carwash_app_init_new_customer,
    .topup_card = carwash_app_topup_card,
    .get_primary_balance = carwash_app_get_primary_balance,
    .get_secondary_balance = carwash_app_get_secondary_balance,
    .get_status_string = carwash_app_get_status_string,
    .task_start = carwash_app_task_start,
    .task_stop = carwash_app_task_stop
};

/* Application Interface Instance --------------------------------------------*/
static const Application_Instance_t s_carwash_app_instance = {
    .type = APP_TYPE_CAR_WASH,
    .name = "Car Wash",
    .callbacks = &s_carwash_callbacks
};

/**
 * @brief Get the application interface for car wash
 * @return Pointer to the car wash application instance
 */
const Application_Instance_t* CarWash_GetApplicationInterface(void)
{
    return &s_carwash_app_instance;
}
