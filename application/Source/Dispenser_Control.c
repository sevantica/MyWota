/*
 * @attention
 *
 * Copyright (c) Sevantica 2025.
 * All rights reserved.
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file Dispenser_Control.c
 * @brief Production dispenser control with MIFARE card integration
 * @details Controls water dispensing with real MIFARE card authentication,
 *          balance tracking, and secure transaction management
 */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "Dispenser_Control.h"
#include "System.h"
#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include "MIFARE_Transaction_Manager.h"
#include "YS_S201_Driver.h"
#include "USB_Logging.h"
#include "LCD_Display_Driver.h"
#include "PN532_Driver.h"
#include "Hardware_Access.h"
#include "Buzzer_Driver.h"
#include "CAT9555_Driver.h"
#include <string.h>
#include <stdio.h>
#include "ui.h"
#include "ui_Screen1.h"

/* Private includes ----------------------------------------------------------*/


/* Private typedefs -----------------------------------------------------------*/
typedef enum {
    DISPENSER_IDLE = 0,
    DISPENSER_CARD_DETECTED,
    DISPENSER_CARD_READY,
    DISPENSER_USER_REQUESTED,
    DISPENSER_DISPENSING,
    DISPENSER_COMPLETING,
    DISPENSER_ERROR
} DispenserState_t;

typedef struct {
    DispenserState_t state;
    uint32_t requested_amount_ml;
    uint32_t dispensed_amount_ml;
    uint32_t dispense_start_time;
    bool valve_open;
    bool card_present;
    bool auto_start_attempted;
    YS_S201_Handle_t flow_sensor;
    TickType_t card_ready_since;
    bool ui_clear_pending;              /* Flag to indicate UI clear is scheduled */
    TickType_t ui_clear_request_time;   /* Time when UI clear was requested */
    TickType_t dispense_finish_time;    /* Time when dispensing finished */
} DispenserHandle_t;

/* Private defines ------------------------------------------------------------*/
#define LOOP_PERIOD_MS              100U    /* Fixed execution period */
#define DISPENSER_UPDATE_INTERVAL_MS 100    /* Check dispenser every 100ms */
#define DISPENSER_MAX_FLOW_RATE_LPM 5.0f    /* Maximum 5 L/min */
#define DISPENSER_MIN_DISPENSE_ML   50      /* Minimum 50mL per transaction */
#define DISPENSER_MAX_DISPENSE_ML   5000    /* Maximum 5L per transaction */
#define DISPENSER_SAFETY_TIMEOUT_MS 120000  /* 120 second (2 minute) maximum dispense time - allows 1L at 0.5 L/min */
#define CARD_REMOVAL_DEBOUNCE_COUNT 3       /* Debounce card removal detection */
#define CARD_READY_AUTO_START_DELAY_MS 200  /* Allow PN532 settle time before first write */
#define UI_CLEAR_DELAY_MS           1000    /* Delay clearing UI after card removal to prevent flicker during noise recovery */

/* Private macros -------------------------------------------------------------*/


/*Static variables ---------------------------------------------------------*/
static TaskHandle_t Dispenser_Control_TaskHandle = NULL;
static DispenserHandle_t dispenser_handle;
static uint8_t card_removal_debounce_counter = 0;
static CAT9555_Handle_t cat9555_handle;
static Buzzer_Handle_t buzzer_handle;
static bool initial_hide_done = false;

/*Extern variables ---------------------------------------------------------*/

/*Global variables ---------------------------------------------------------*/

/*Private function prototypes ----------------------------------------------*/
// static void DispenserSafetyTimerCallback(TimerHandle_t timer);  // DISABLED - safety timer removed
static void StartDispensing(uint32_t amount_ml);
static void StopDispensing(bool silent);
static void UpdateDispensingProgress(void);
static void SetValveState(bool open, bool silent);
static const char* GetDispenserStateString(DispenserState_t state);
static void UpdateCardUI(uint32_t current_balance_ml, uint32_t last_topup_amount_ml);
static void ProcessCardDetection(void);
static void ProcessDispenserState(void);

/**
 * @brief Main dispenser control task - PRODUCTION MODE
 * @param argument Task parameters (unused)
 * @details Manages MIFARE card detection, authentication, balance tracking,
 *          and secure water dispensing with continuous card presence monitoring
 */
static void Dispenser_Control_Task(void* argument)
{
    (void)argument;
    
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t periodTicks = pdMS_TO_TICKS(LOOP_PERIOD_MS);
    
    // Initialize dispenser handle
    memset(&dispenser_handle, 0, sizeof(DispenserHandle_t));
    dispenser_handle.state = DISPENSER_IDLE;
    
    #if defined(PICO_BOARD)
    // Initialize valve control pin (GPIO 15)
    gpio_init(VALVE_CONTROL_PIN);
    gpio_set_dir(VALVE_CONTROL_PIN, GPIO_OUT);
    gpio_put(VALVE_CONTROL_PIN, 0);  // Start with valve closed
    USB_Log_Printf("DISPENSER: Valve control pin (GPIO %d) initialized\r\n", VALVE_CONTROL_PIN);
    #endif
    
    // Initialize CAT9555 I/O expander
    CAT9555_Status_t cat_status = CAT9555_Init(&cat9555_handle, CAT9555_I2C_ADDRESS);
    if (cat_status != CAT9555_OK) {
        USB_Log_Printf("DISPENSER: Failed to initialize CAT9555: %s\r\n", 
                       CAT9555_GetStatusString(cat_status));
        // Continue anyway - buzzer is optional
    } else {
        // Initialize buzzer driver
        Buzzer_Status_t buzzer_status = Buzzer_Init(&buzzer_handle, &cat9555_handle);
        if (buzzer_status != BUZZER_OK) {
            USB_Log_Printf("DISPENSER: Failed to initialize buzzer: %s\r\n", 
                           Buzzer_GetStatusString(buzzer_status));
            // Continue anyway - buzzer is optional
        } else {
            USB_Log_Printf("DISPENSER: Buzzer initialized successfully\r\n");
        }
    }
    
    // Initialize flow sensor
    YS_S201_Status_t flow_status = YS_S201_Init(&dispenser_handle.flow_sensor, FLOW_SENSOR_PIN);
    if (flow_status != YS_S201_OK) {
        USB_Log_Printf("DISPENSER: Failed to initialize flow sensor: %s\r\n", 
                       YS_S201_GetStatusString(flow_status));
        // Continue anyway - sensor errors will be detected during operation
    } else {
        USB_Log_Printf("YS_S201: YS-S201 sensor initialized on GPIO %d\r\n", FLOW_SENSOR_PIN);
        
        // Start flow measurement (enables interrupts)
        flow_status = YS_S201_Start(&dispenser_handle.flow_sensor);
        if (flow_status != YS_S201_OK) {
            USB_Log_Printf("DISPENSER: Failed to start flow sensor: %s\r\n", 
                           YS_S201_GetStatusString(flow_status));
        } else {
            USB_Log_Printf("YS_S201: Flow measurement started\r\n");
        }
    }
    
    // Safety timer disabled - relying on card presence monitoring for safety
    // dispenser_safety_timer = xTimerCreate(
    //     "DispenserSafety",
    //     pdMS_TO_TICKS(DISPENSER_SAFETY_TIMEOUT_MS),
    //     pdFALSE,  // One-shot timer
    //     NULL,
    //     DispenserSafetyTimerCallback
    // );
    // 
    // if (dispenser_safety_timer == NULL) {
    //     USB_Log_Printf("DISPENSER: CRITICAL - Failed to create safety timer\r\n");
    // }
    
    USB_Log_Printf("DISPENSER: Task started - Card presence based dispensing (no safety timer)\r\n");
    
    // Reset UI to default state at boot
    ui_set_visibility(ui_dispensedSession, false);
    ui_set_label_text(ui_cardRemaining, "0mL");
    ui_set_bar_value(ui_totalRemainingBar, 0, LV_ANIM_OFF);
    ui_set_visibility(ui_customerID, false);
    
    for (;;)
    {
        TASK_HEARTBEAT_EVERY_SECOND("Dispenser");
        
        // Early crash detection - log before ANY processing
        static uint32_t loop_counter = 0;
        loop_counter++;
        if (loop_counter % 10 == 0) {
            USB_Log_Printf("DISPENSER: Loop alive - iteration %lu\r\n", loop_counter);
        }
        
        vTaskDelayUntil(&lastWake, periodTicks); /* Deterministic 100ms cycle */
        
        // Ensure label is initially hidden (retry until UI is ready)
        if (!initial_hide_done) {
            if (ui_set_visibility(ui_dispensedSession, false)) {
                initial_hide_done = true;
            }
        }
        
        // Process card detection and MIFARE state
        ProcessCardDetection();
        
        // Check if we need to hide the dispensed session label
        if (dispenser_handle.dispense_finish_time > 0) {
            if ((xTaskGetTickCount() - dispenser_handle.dispense_finish_time) > pdMS_TO_TICKS(5000)) {
                ui_set_visibility(ui_dispensedSession, false);
                dispenser_handle.dispense_finish_time = 0;
            }
        }
        
        // Process dispenser state machine
        ProcessDispenserState();
    }
}
/*Private Functions ---------------------------------------------------------*/

/**
 * @brief Process card detection and MIFARE state updates
 */
static void ProcessCardDetection(void)
{
    // Check if card is present using the correct API
    bool card_present = MIFARE_IsCardPresent();
    
    static int log_counter = 0;
    log_counter++;
    if (log_counter % 10 == 0) { // Every 1 second
         USB_Log_Printf("DISPENSER DETECT DBG: CardPresent=%d, HandlePresent=%d\r\n", card_present, dispenser_handle.card_present);
    }
    
    // Only log state changes, not every cycle
    
    // Debounce card removal
    if (!card_present) {
        if (dispenser_handle.card_present) {
            card_removal_debounce_counter++;
            if (card_removal_debounce_counter == 1) {
                USB_Log_Printf("[DISPENSER DETECT] Card absence detected, starting debounce...\r\n");
            }
            if (card_removal_debounce_counter >= CARD_REMOVAL_DEBOUNCE_COUNT) {
                dispenser_handle.card_present = false;
                card_removal_debounce_counter = 0;
                
                USB_Log_Printf("[DISPENSER DETECT] Card REMOVED (debounced)\r\n");
                
                // If dispensing, emergency stop
                if (dispenser_handle.state == DISPENSER_DISPENSING) {
                    USB_Log_Printf("DISPENSER: EMERGENCY STOP - Card removed during dispensing\r\n");
                    StopDispensing(false); // Non-silent stop - beep to alert user
                    dispenser_handle.state = DISPENSER_ERROR;
                }
                
                // Schedule UI clear instead of doing it immediately to prevent flicker on transient errors
                dispenser_handle.ui_clear_pending = true;
                dispenser_handle.ui_clear_request_time = xTaskGetTickCount();
                USB_Log_Printf("DISPENSER: UI clear scheduled (delayed %d ms)\r\n", UI_CLEAR_DELAY_MS);
            }
        }
    } else {
        card_removal_debounce_counter = 0;
        
        if (!dispenser_handle.card_present) {
            dispenser_handle.card_present = true;
            USB_Log_Printf("[DISPENSER DETECT] Card DETECTED\r\n");
            
            // Cancel any pending UI clear if card returns quickly
            if (dispenser_handle.ui_clear_pending) {
                dispenser_handle.ui_clear_pending = false;
                USB_Log_Printf("DISPENSER: UI clear cancelled - card re-detected\r\n");
            }
        }
    }
    
    // Process pending UI clear
    if (dispenser_handle.ui_clear_pending) {
        if ((xTaskGetTickCount() - dispenser_handle.ui_clear_request_time) > pdMS_TO_TICKS(UI_CLEAR_DELAY_MS)) {
            UpdateCardUI(0, 0);
            dispenser_handle.ui_clear_pending = false;
            USB_Log_Printf("DISPENSER: UI cleared (timeout)\r\n");
        }
    }
}

/**
 * @brief Process dispenser state machine
 */
static void ProcessDispenserState(void)
{
    MIFARE_DispenseState_t mifare_state = MIFARE_GetDispenseState();
    bool card_physically_present = MIFARE_IsCardPresent();
    
    // Card is ready if MIFARE says ready AND card is physically present
    bool card_ready = (mifare_state == DISPENSE_STATE_READY_TO_DISPENSE) && card_physically_present;
    
    // Removed repetitive state logging - only log significant transitions
    
    // Debug logging every 200ms (2 cycles)
    static uint32_t debug_counter = 0;
    debug_counter++;
    if (debug_counter % 2 == 0) {
        USB_Log_Printf("DISPENSER DBG: State=%d, CardPresent=%d, CardReady=%d, MifareState=%d, PhysPresent=%d, AutoStart=%d\r\n",
                       dispenser_handle.state, dispenser_handle.card_present, card_ready, mifare_state, card_physically_present, dispenser_handle.auto_start_attempted);
    }
    
    switch (dispenser_handle.state) {
        case DISPENSER_IDLE:
            if (dispenser_handle.card_present) {
                dispenser_handle.state = DISPENSER_CARD_DETECTED;
                USB_Log_Printf("DISPENSER: Transitioning from IDLE to CARD_DETECTED\r\n");
            }
            break;
            
        case DISPENSER_CARD_DETECTED:
            if (card_ready) {
                dispenser_handle.state = DISPENSER_CARD_READY;
                USB_Log_Printf("DISPENSER: Card ready for dispensing\r\n");
                dispenser_handle.card_ready_since = xTaskGetTickCount();
                dispenser_handle.auto_start_attempted = false;  // Reset flag for new card ready event
                
                // Update UI with current card balance
                uint32_t balance_ml = MIFARE_GetBalanceML();
                uint32_t last_topup_ml = MIFARE_GetLastTopupAmountML();
                UpdateCardUI(balance_ml, last_topup_ml);
                
                // Send event to UI system
                send_event_rfid_picc(PICC_POS_0, (uint8_t)PICC_STATE_ACTIVE, 0, EVENT_SOURCE_RFID_RC522);
            }
            break;
            
        case DISPENSER_CARD_READY:
            if (!card_ready) {
                dispenser_handle.state = DISPENSER_IDLE;
                dispenser_handle.auto_start_attempted = false;
                USB_Log_Printf("DISPENSER: Card no longer ready\r\n");
                
                // Only clear UI if card is actually removed (debounced check)
                // If it's just a transient error (noise), card_present will still be true
                if (!dispenser_handle.card_present) {
                    UpdateCardUI(0, 0);
                } else {
                    USB_Log_Printf("DISPENSER: Card not ready but still present (debounced) - preserving UI\r\n");
                }
                
                send_event_rfid_picc(PICC_POS_0, (uint8_t)PICC_STATE_INACTIVE, 0, EVENT_SOURCE_RFID_RC522);
                break;
            }
            
            // Auto-start dispensing when card is CONFIRMED stable (not just ready)
            if (!dispenser_handle.auto_start_attempted) {
                // Early balance check - don't even try if balance is zero
                uint32_t balance_ml = MIFARE_GetBalanceML();
                USB_Log_Printf("DISPENSER: Auto-start check - balance=%lu mL\r\n", balance_ml);
                
                if (balance_ml == 0) {
                    USB_Log_Printf("DISPENSER: Card has zero balance, no auto-start\r\n");
                    dispenser_handle.auto_start_attempted = true;
                    break;
                }
                
                // Update stability check (this will confirm card after 1s)
                MIFARE_UpdateStabilityCheck();
                
                // Check if card presence is confirmed (stable for 1s) to prevent dispensing on transient RF
                bool card_confirmed = MIFARE_IsCardPresenceConfirmed();
                USB_Log_Printf("DISPENSER: Card confirmed status: %d\r\n", card_confirmed);
                
                if (card_confirmed) {
                    // Card confirmed stable - start dispensing full balance
                    USB_Log_Printf("DISPENSER: Card CONFIRMED stable - balance=%lu mL\r\n", balance_ml);
                    
                    dispenser_handle.requested_amount_ml = balance_ml;
                    USB_Log_Printf("DISPENSER: Setting state to USER_REQUESTED\r\n");
                    dispenser_handle.state = DISPENSER_USER_REQUESTED;
                    dispenser_handle.auto_start_attempted = true;
                    USB_Log_Printf("DISPENSER: State set to USER_REQUESTED, auto_start flag set\r\n");
                }
                // else: Card not yet confirmed stable - continue waiting (no logging spam)
            }
            break;
            
        case DISPENSER_USER_REQUESTED:
            USB_Log_Printf("DISPENSER: USER_REQUESTED state - calling StartDispensing(%lu mL)\r\n", 
                           dispenser_handle.requested_amount_ml);
            StartDispensing(dispenser_handle.requested_amount_ml);
            break;
            
        case DISPENSER_DISPENSING:
            UpdateDispensingProgress();
            
            // Check for card removal
            if (!dispenser_handle.card_present || 
                mifare_state == DISPENSE_STATE_CARD_REMOVED) {
                USB_Log_Printf("[DISPENSER DISPENSING] EMERGENCY VALVE CLOSE - Card removed\r\n");
                StopDispensing(false); // Non-silent stop - beep to alert user
                dispenser_handle.state = DISPENSER_ERROR;
            }
            break;
            
        case DISPENSER_COMPLETING:
            StopDispensing(false); // Normal stop
            MIFARE_CommitTransaction();
            USB_Log_Printf("DISPENSER: Completed - %lu mL dispensed\r\n", 
                           dispenser_handle.dispensed_amount_ml);
            dispenser_handle.state = DISPENSER_CARD_READY;
            dispenser_handle.card_ready_since = xTaskGetTickCount();
            dispenser_handle.auto_start_attempted = false;  // Reset to allow next auto-start
            
            // Update UI with new balance and final dispensed amount
            uint32_t new_balance_ml = MIFARE_GetBalanceML();
            uint32_t last_topup_ml = MIFARE_GetLastTopupAmountML();
            UpdateCardUI(new_balance_ml, last_topup_ml);
            
            // Update final dispensed session amount
            uint32_t total_dispensed_ml = MIFARE_GetTotalDispensedThisSession();
            
            static char dispensed_str[16];
            uint32_t liters = total_dispensed_ml / 1000;
            uint32_t decimal = (total_dispensed_ml % 1000) / 100;
            snprintf(dispensed_str, sizeof(dispensed_str), "%lu.%luL", liters, decimal);
            ui_set_label_text(ui_dispensedSession, dispensed_str);
            break;
            
        case DISPENSER_ERROR:
            StopDispensing(false); // Non-silent stop - beep to indicate error
            if (mifare_state == DISPENSE_STATE_READY_TO_DISPENSE) {
                dispenser_handle.state = DISPENSER_CARD_READY;
            } else {
                dispenser_handle.state = DISPENSER_IDLE;
            }
            break;
            
        default:
            USB_Log_Printf("DISPENSER ERROR: Invalid state %d - resetting to IDLE\r\n", dispenser_handle.state);
            dispenser_handle.state = DISPENSER_IDLE;
            break;
    }
}

/**
 * @brief Start dispensing operation
 */
static void StartDispensing(uint32_t amount_ml)
{
    // Check if card has sufficient balance
    uint32_t current_balance_ml = MIFARE_GetBalanceML();
    if (current_balance_ml == 0) {
        USB_Log_Printf("DISPENSER: Cannot start - card balance is zero\r\n");
        dispenser_handle.state = DISPENSER_ERROR;
        return;
    }
    
    // Limit dispense amount to available balance
    if (amount_ml > current_balance_ml) {
        USB_Log_Printf("DISPENSER: Requested %lu mL exceeds balance %lu mL, limiting to balance\r\n", 
                       amount_ml, current_balance_ml);
        amount_ml = current_balance_ml;
    }
    
    // Begin MIFARE transaction
    MIFARE_Result_t mifare_result = MIFARE_BeginTransaction(amount_ml);
    if (mifare_result != MIFARE_RESULT_OK) {
        USB_Log_Printf("DISPENSER: Failed to begin transaction: %s\r\n", 
                       MIFARE_GetResultString(mifare_result));
        dispenser_handle.state = DISPENSER_ERROR;
        return;
    }
    
    // Reset flow sensor total
    YS_S201_ResetTotalVolume(&dispenser_handle.flow_sensor);
    
    // Open dispenser valve (beep will sound inside SetValveState)
    SetValveState(true, false);
    
    // Safety timer disabled - card presence monitoring provides safety
    // if (dispenser_safety_timer != NULL) {
    //     xTimerStart(dispenser_safety_timer, 0);
    // }
    
    // Initialize dispensing tracking
    dispenser_handle.dispensed_amount_ml = 0;
    dispenser_handle.dispense_start_time = (uint32_t)xTaskGetTickCount();
    dispenser_handle.state = DISPENSER_DISPENSING;
    dispenser_handle.dispense_finish_time = 0;
    
    // Initialize UI - show 0 dispensed at start
    ui_set_visibility(ui_dispensedSession, true);
    
    static char dispensed_str[16];
    snprintf(dispensed_str, sizeof(dispensed_str), "0.0L");
    ui_set_label_text(ui_dispensedSession, dispensed_str);
    
    USB_Log_Printf("DISPENSER: Dispensing started - Target: %lu mL\r\n", amount_ml);
}

/**
 * @brief Update dispensing progress
 */
static void UpdateDispensingProgress(void)
{
    // Get current flow data
    YS_S201_FlowData_t flow_data;
    YS_S201_Status_t flow_status = YS_S201_GetFlowData(&dispenser_handle.flow_sensor, &flow_data);
    
    if (flow_status != YS_S201_OK) {
        USB_Log_Printf("DISPENSER: Flow sensor error: %s\r\n", YS_S201_GetStatusString(flow_status));
        StopDispensing(false); // Normal stop
        dispenser_handle.state = DISPENSER_ERROR;
        return;
    }
    
    // Log flow data every 2 seconds to debug sensor
    static uint32_t flow_debug_counter = 0;
    if (++flow_debug_counter % 20 == 0) {  // Every 20 cycles = 2 seconds
        USB_Log_Printf("FLOW SENSOR: pulses=%lu, total_ml=%.1f, rate=%.2f L/min\r\n",
                       flow_data.pulse_count, flow_data.total_volume_ml, flow_data.flow_rate_lpm);
    }
    
    // Update flow rate on UI (removed repetitive flow data logging)
    static char flow_str[16];
    snprintf(flow_str, sizeof(flow_str), "%.1f L/min", flow_data.flow_rate_lpm);
    ui_set_label_text(ui_flowRateSensor, flow_str);

    // Calculate dispensed amount
    uint32_t current_dispensed_ml = (uint32_t)(flow_data.total_volume_ml);
    uint32_t additional_ml = 0;
    
    // Update MIFARE transaction with current flow rate
    // This will be called periodically: 50ms intervals when flowing, 100ms when idle
    if (current_dispensed_ml > dispenser_handle.dispensed_amount_ml) {
        additional_ml = current_dispensed_ml - dispenser_handle.dispensed_amount_ml;
        dispenser_handle.dispensed_amount_ml = current_dispensed_ml;
        
        USB_Log_Printf("DISPENSER: Progress %lu/%lu mL (%.1f L/min)\r\n", 
                       current_dispensed_ml, dispenser_handle.requested_amount_ml,
                       flow_data.flow_rate_lpm);
    }
    
    // Always call UpdateTransactionProgress to maintain card presence and write at appropriate intervals
    MIFARE_Result_t mifare_result = MIFARE_UpdateTransactionProgress(additional_ml, flow_data.flow_rate_lpm);
    // Only log errors, not successful updates
    
    if (mifare_result != MIFARE_RESULT_OK) {
        USB_Log_Printf("DISPENSER: MIFARE update failed: %s\r\n", 
                       MIFARE_GetResultString(mifare_result));
        
        if (mifare_result == MIFARE_RESULT_CARD_REMOVED) {
            USB_Log_Printf("DISPENSER: Card removed - emergency stop\r\n");
            StopDispensing(false); // Non-silent stop - beep to alert user
            dispenser_handle.state = DISPENSER_ERROR;
            return;
        }
    }
    
    // Update UI with current balance during dispensing
    if (additional_ml > 0) {
        uint32_t current_balance_ml = MIFARE_GetBalanceML();
        uint32_t last_topup_ml = MIFARE_GetLastTopupAmountML();
        UpdateCardUI(current_balance_ml, last_topup_ml);
        
        // Update dispensed session UI with total dispensed amount
        uint32_t total_dispensed_ml = MIFARE_GetTotalDispensedThisSession();
        
        static char dispensed_str[16];
        uint32_t liters = total_dispensed_ml / 1000;
        uint32_t decimal = (total_dispensed_ml % 1000) / 100;
        snprintf(dispensed_str, sizeof(dispensed_str), "%lu.%luL", liters, decimal);
        ui_set_label_text(ui_dispensedSession, dispensed_str);
        
        // Stop dispensing if balance reaches zero
        if (current_balance_ml == 0) {
            USB_Log_Printf("DISPENSER: Balance reached zero - stopping\r\n");
            dispenser_handle.state = DISPENSER_COMPLETING;
            return;
        }
    }
    
    // Check if target reached
    if (dispenser_handle.dispensed_amount_ml >= dispenser_handle.requested_amount_ml) {
        USB_Log_Printf("DISPENSER: Target reached\r\n");
        dispenser_handle.state = DISPENSER_COMPLETING;
    }
    
    // Flow rate limit check disabled - no maximum flow rate enforcement
    // if (flow_data.flow_rate_lpm > DISPENSER_MAX_FLOW_RATE_LPM) {
    //     USB_Log_Printf("DISPENSER: Flow rate too high: %.2f L/min\r\n", flow_data.flow_rate_lpm);
    //     StopDispensing();
    //     dispenser_handle.state = DISPENSER_ERROR;
    // }
}

/**
 * @brief Stop dispensing operation
 */
static void StopDispensing(bool silent)
{
    // Close valve (beep will sound inside SetValveState unless silent)
    SetValveState(false, silent);
    
    // Update flow rate to 0 on UI
    ui_set_label_text(ui_flowRateSensor, "0.0 L/min");
    
    // Record finish time for UI visibility timeout
    dispenser_handle.dispense_finish_time = xTaskGetTickCount();
    
    // Safety timer disabled
    // if (dispenser_safety_timer != NULL) {
    //     xTimerStop(dispenser_safety_timer, 0);
    // }
}

/**
 * @brief Control dispenser valve
 */
static void SetValveState(bool open, bool silent)
{
    // Always log function call for debugging
    USB_Log_Printf("DISPENSER: SetValveState() called - open=%d, silent=%d\r\n", open, silent);
    
    dispenser_handle.valve_open = open;
    
    #if defined(PICO_BOARD)
    // Direct GPIO control for Pico platform
    USB_Log_Printf("DISPENSER: Setting GPIO %d to %d\r\n", VALVE_CONTROL_PIN, open ? 1 : 0);
    gpio_put(VALVE_CONTROL_PIN, open ? 1 : 0);
    #else
    // Legacy I/O expander control for STM32 platform
    io_driver_set_state(RELAY_CONTROL_0_POS, 
                        open ? GPIO_PIN_SET : GPIO_PIN_RESET, 
                        Dispenser_Control_TaskHandle);
    #endif
    
    // Sound buzzer to indicate valve state change (unless silent mode)
    if (!silent) {
        if (open) {
            // Valve opening - single beep
            USB_Log_Printf("DISPENSER: Calling Buzzer_SignalDispenserStart...\r\n");
            Buzzer_Status_t buzzer_status = Buzzer_SignalDispenserStart(&buzzer_handle);
            if (buzzer_status != BUZZER_OK) {
                USB_Log_Printf("DISPENSER: Buzzer start failed: %s\r\n", 
                               Buzzer_GetStatusString(buzzer_status));
            }
            USB_Log_Printf("DISPENSER: Valve OPEN (beep requested)\r\n");
        } else {
            // Valve closing - double beep
            USB_Log_Printf("DISPENSER: Calling Buzzer_SignalDispenserStop...\r\n");
            Buzzer_Status_t buzzer_status = Buzzer_SignalDispenserStop(&buzzer_handle);
            if (buzzer_status != BUZZER_OK) {
                USB_Log_Printf("DISPENSER: Buzzer stop failed: %s\r\n", 
                               Buzzer_GetStatusString(buzzer_status));
            }
            USB_Log_Printf("DISPENSER: Valve CLOSED (double beep requested)\r\n");
        }
    } else {
        USB_Log_Printf("DISPENSER: Valve %s (silent)\r\n", open ? "OPEN" : "CLOSED");
    }
}

/**
 * @brief Safety timer callback - DISABLED (not used)
 */
// static void DispenserSafetyTimerCallback(TimerHandle_t timer)
// {
//     (void)timer;
//     
//     USB_Log_Printf("DISPENSER: SAFETY TIMEOUT - Emergency stop\r\n");
//     
//     StopDispensing(false); // Normal stop
//     dispenser_handle.state = DISPENSER_ERROR;
//     MIFARE_RollbackTransaction();
// }

/**
 * @brief Get dispenser state string
 */
static const char* GetDispenserStateString(DispenserState_t state)
{
    switch (state) {
        case DISPENSER_IDLE:            return "Idle";
        case DISPENSER_CARD_DETECTED:   return "Card Detected";
        case DISPENSER_CARD_READY:      return "Card Ready";
        case DISPENSER_USER_REQUESTED:  return "User Requested";
        case DISPENSER_DISPENSING:      return "Dispensing";
        case DISPENSER_COMPLETING:      return "Completing";
        case DISPENSER_ERROR:           return "Error";
        default:                        return "Unknown";
    }
}

/**
 * @brief Update card UI display
 */
static void UpdateCardUI(uint32_t current_balance_ml, uint32_t last_topup_amount_ml)
{
    USB_Log_Printf("DISPENSER: UpdateCardUI called with balance=%lu, topup=%lu\r\n", current_balance_ml, last_topup_amount_ml);
    
    // Verify UI objects exist before accessing
    if (ui_cardRemaining == NULL) {
        USB_Log_Printf("DISPENSER: UpdateCardUI skipped - UI not initialized\r\n");
        return;
    }
    
    // Update card remaining balance
    static char balance_str[16];
    if (current_balance_ml > 9000) {
        uint32_t liters = current_balance_ml / 1000;
        snprintf(balance_str, sizeof(balance_str), "%luL", liters);
    } else {
        snprintf(balance_str, sizeof(balance_str), "%luml", current_balance_ml);
    }
    
    USB_Log_Printf("DISPENSER: Setting balance text to '%s'\r\n", balance_str);
    ui_set_label_text(ui_cardRemaining, balance_str);
    USB_Log_Printf("DISPENSER: Balance text set successfully\r\n");
    
    // Update total remaining bar
    uint8_t percentage = 0;
    if (last_topup_amount_ml > 0) {
        if (current_balance_ml >= last_topup_amount_ml) {
            percentage = 100;
        } else {
            percentage = (uint8_t)((current_balance_ml * 100) / last_topup_amount_ml);
        }
    }
    
    USB_Log_Printf("DISPENSER: Setting bar to %d%%\r\n", percentage);
    if (ui_totalRemainingBar != NULL) {
        ui_set_bar_value(ui_totalRemainingBar, percentage, LV_ANIM_ON);
        USB_Log_Printf("DISPENSER: Bar set successfully\r\n");
    }
    
    // Update level color indicator
    if (ui_levelColourIndicator != NULL) {
        USB_Log_Printf("DISPENSER: Updating color indicator\r\n");
        if (percentage > 25) {
            ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0x05820A), LV_PART_MAIN | LV_STATE_DEFAULT);
        } else if (percentage > 10) {
            ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    
    // Update customer ID from card account data
    USB_Log_Printf("DISPENSER: Updating customer ID\r\n");
    extern MIFARE_TransactionManager_t g_transaction_manager;
    if (current_balance_ml > 0 && ui_customerID != NULL) {
        // Card is present, display phone number (extract from raw_data)
        static char phone_str[12];  // 11 digits + null terminator
        memcpy(phone_str, &g_transaction_manager.current_card.account_data.raw_data[ACCOUNT_DATA_PHONE_OFFSET], ACCOUNT_DATA_PHONE_SIZE);
        phone_str[ACCOUNT_DATA_PHONE_SIZE] = '\0';  // Null terminate
        
        ui_set_label_text(ui_customerID, phone_str);
        ui_set_visibility(ui_customerID, true);
        USB_Log_Printf("DISPENSER: Customer ID updated\r\n");
    } else if (ui_customerID != NULL) {
        // Card removed, clear display
        ui_set_visibility(ui_customerID, false);
        USB_Log_Printf("DISPENSER: Customer ID hidden\r\n");
    }
    USB_Log_Printf("DISPENSER: UpdateCardUI completed\r\n");
}

/*Public Functions ----------------------------------------------------------*/

void Task_Start_Dispenser_Control_Task()
{
    xTaskCreate(Dispenser_Control_Task, 
                "Dispenser_Control Task", 
                DISPENSER_CONTROL_TASK_STACK_WORDS, 
                NULL, 
                DISPENSER_CONTROL_TASK_PRIORITY, 
                &Dispenser_Control_TaskHandle);
}

TaskHandle_t task_get_handle_Dispenser_Control_Task()
{
	return Dispenser_Control_TaskHandle;
}

