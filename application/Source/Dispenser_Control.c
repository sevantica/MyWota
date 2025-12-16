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
#include "FreeRTOS.h"                      // Core RTOS types (TickType_t) and timing functions
#include "task.h"                          // Task creation/management (xTaskCreate, vTaskDelayUntil)
#include "Dispenser_Control.h"             // Own module interface
#include "System.h"                        // System event types (EVENT_SOURCE_Enum)
#include "Task_Heartbeat.h"                // TASK_HEARTBEAT_EVERY_SECOND watchdog macro
#include "task_stack_config.h"             // Task stack size definitions
#include "MIFARE_Transaction_Manager.h"    // Card transaction management (MIFARE_* functions)
#include "YS_S201_Driver.h"                // Flow sensor driver (YS_S201_* functions)
#include "hardware/gpio.h"                 // Pico SDK GPIO control (gpio_init, gpio_put)
#include "USB_Logging.h"                   // USB_Log_Printf debug output
#include "Hardware_Access.h"               // Hardware abstraction (Get_App_GPIO_Pins)
#include "Buzzer_Driver.h"                 // Buzzer control for user feedback
#include "CAT9555_Driver.h"                // I/O expander driver (buzzer interface)
#include <string.h>                        // memset for struct initialization

/* Private includes ----------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
static App_GPIO_Pins_t app_gpio_pins;  // Module pin configuration

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

/*Extern variables ---------------------------------------------------------*/

/*Global variables ---------------------------------------------------------*/

/*Private function prototypes ----------------------------------------------*/
// static void DispenserSafetyTimerCallback(TimerHandle_t timer);  // DISABLED - safety timer removed
static void StartDispensing(uint32_t amount_ml);
static void StopDispensing(bool silent);
static void UpdateDispensingProgress(void);
static void SetValveState(bool open, bool silent);
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
    
    /* Get module pin configuration (first thing when creating driver instance) */
    app_gpio_pins = Get_App_GPIO_Pins();
    
    // Initialize dispenser handle
    memset(&dispenser_handle, 0, sizeof(DispenserHandle_t));
    dispenser_handle.state = DISPENSER_IDLE;
    
    #if defined(PICO_BOARD)
    // Initialize valve control pin
    gpio_init(app_gpio_pins.valve_control_pin);
    gpio_set_dir(app_gpio_pins.valve_control_pin, GPIO_OUT);
    gpio_put(app_gpio_pins.valve_control_pin, 0);  // Start with valve closed
    USB_Log_Printf("DISPENSER: Valve control pin (GPIO %d) initialized\r\n", app_gpio_pins.valve_control_pin);
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
    YS_S201_Status_t flow_status = YS_S201_Init(&dispenser_handle.flow_sensor, app_gpio_pins.flow_sensor_pin);
    if (flow_status != YS_S201_OK) {
        USB_Log_Printf("DISPENSER: Failed to initialize flow sensor: %s\r\n", 
                       YS_S201_GetStatusString(flow_status));
        // Continue anyway - sensor errors will be detected during operation
    } else {
        USB_Log_Printf("YS_S201: YS-S201 sensor initialized on GPIO %d\r\n", app_gpio_pins.flow_sensor_pin);
        
        // Start flow measurement (enables interrupts)
        flow_status = YS_S201_Start(&dispenser_handle.flow_sensor);
        if (flow_status != YS_S201_OK) {
            USB_Log_Printf("DISPENSER: Failed to start flow sensor: %s\r\n", 
                           YS_S201_GetStatusString(flow_status));
        } else {
            USB_Log_Printf("YS_S201: Flow measurement started\r\n");
        }
    }
    
    USB_Log_Printf("DISPENSER: Task started - Card presence based dispensing (no safety timer)\r\n");
    
    for (;;)
    {
        TASK_HEARTBEAT_EVERY_SECOND("Dispenser");
        
        vTaskDelayUntil(&lastWake, periodTicks); /* Deterministic 100ms cycle */
        
        // Process card detection and MIFARE state
        ProcessCardDetection();
        
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
    /* Check if card is present using the correct API */
    bool card_present = MIFARE_IsCardPresent();
    
    /* Debounce card removal */
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
    
    // Process pending clear notification
    if (dispenser_handle.ui_clear_pending) {
        if ((xTaskGetTickCount() - dispenser_handle.ui_clear_request_time) > pdMS_TO_TICKS(UI_CLEAR_DELAY_MS)) {
            dispenser_handle.ui_clear_pending = false;
            USB_Log_Printf("DISPENSER: Clear confirmed (timeout)\r\n");
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
                
                // Card balance available - UI removed
                uint32_t balance_ml = MIFARE_GetBalanceML();
                USB_Log_Printf("DISPENSER: Card balance: %lu mL\r\n", balance_ml);
                
                // Send event to UI system - removed undefined send_event_rfid_picc
            }
            break;
            
        case DISPENSER_CARD_READY:
            if (!card_ready) {
                dispenser_handle.state = DISPENSER_IDLE;
                dispenser_handle.auto_start_attempted = false;
                USB_Log_Printf("DISPENSER: Card no longer ready\r\n");
                
                // Card no longer ready
                if (!dispenser_handle.card_present) {
                    USB_Log_Printf("DISPENSER: Card removed (debounced)\r\n");
                } else {
                    USB_Log_Printf("DISPENSER: Card not ready but still present (debounced)\r\n");
                }
                
                // Removed undefined send_event_rfid_picc
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
            
            // Log final dispensing results
            uint32_t new_balance_ml = MIFARE_GetBalanceML();
            uint32_t total_dispensed_ml = MIFARE_GetTotalDispensedThisSession();
            USB_Log_Printf("DISPENSER: Final balance: %lu mL, Total dispensed this session: %lu mL\r\n",
                           new_balance_ml, total_dispensed_ml);
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
    
    /* Open dispenser valve (beep will sound inside SetValveState) */
    SetValveState(true, false);
    
    /* Initialize dispensing tracking */
    dispenser_handle.dispensed_amount_ml = 0;
    dispenser_handle.dispense_start_time = (uint32_t)xTaskGetTickCount();
    dispenser_handle.state = DISPENSER_DISPENSING;
    dispenser_handle.dispense_finish_time = 0;
    
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
    
    // Check balance during dispensing
    if (additional_ml > 0) {
        uint32_t current_balance_ml = MIFARE_GetBalanceML();
        USB_Log_Printf("DISPENSER: Current balance: %lu mL\r\n", current_balance_ml);
        
        // Stop dispensing if balance reaches zero
        if (current_balance_ml == 0) {
            USB_Log_Printf("DISPENSER: Balance reached zero - stopping\r\n");
            dispenser_handle.state = DISPENSER_COMPLETING;
            return;
        }
    }
    
    /* Check if target reached */
    if (dispenser_handle.dispensed_amount_ml >= dispenser_handle.requested_amount_ml) {
        USB_Log_Printf("DISPENSER: Target reached\r\n");
        dispenser_handle.state = DISPENSER_COMPLETING;
    }
}

/**
 * @brief Stop dispensing operation
 */
static void StopDispensing(bool silent)
{
    /* Close valve (beep will sound inside SetValveState unless silent) */
    SetValveState(false, silent);
    
    /* Record finish time */
    dispenser_handle.dispense_finish_time = xTaskGetTickCount();
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
    USB_Log_Printf("DISPENSER: Setting GPIO %d to %d\r\n", app_gpio_pins.valve_control_pin, open ? 1 : 0);
    gpio_put(app_gpio_pins.valve_control_pin, open ? 1 : 0);
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
 * @brief Update card UI display - REMOVED (UI interactions disabled)
 */
static void UpdateCardUI(uint32_t current_balance_ml, uint32_t last_topup_amount_ml)
{
    // UI interactions removed - stub function for compatibility
    (void)current_balance_ml;
    (void)last_topup_amount_ml;
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

/* UI Getter Functions - UI polls these instead of receiving events */

/**
 * @brief Get requested dispense amount in milliliters
 * @return uint32_t Requested amount (in mL)
 */
uint32_t Dispenser_GetRequestedAmountML(void)
{
    return dispenser_handle.requested_amount_ml;
}

/**
 * @brief Get currently dispensed amount in milliliters
 * @return uint32_t Dispensed amount (in mL) for current dispense
 */
uint32_t Dispenser_GetDispensedAmountML(void)
{
    return dispenser_handle.dispensed_amount_ml;
}

/**
 * @brief Get total dispensed in this session (may include multiple dispenses)
 * @return uint32_t Session dispensed amount (in mL)
 */
uint32_t Dispenser_GetDispensedSessionML(void)
{
    // For now, same as single dispense amount
    // Could be extended to track multiple dispenses per card session
    return dispenser_handle.dispensed_amount_ml;
}

/**
 * @brief Check if valve is currently open
 * @return bool True if valve is open
 */
bool Dispenser_IsValveOpen(void)
{
    return dispenser_handle.valve_open;
}

/**
 * @brief Check if actively dispensing
 * @return bool True if in dispensing state
 */
bool Dispenser_IsDispensing(void)
{
    return (dispenser_handle.state == DISPENSER_DISPENSING);
}
