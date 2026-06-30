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
 * @details This layer reacts to MIFARE transaction state, publishes operation/status
 *          events, makes business decisions, and updates card data accordingly.
 *          
 *          Implements the common Application_Interface for interoperability
 *          with other MIFARE-based applications (e.g., car wash).
 */

/* Includes ------------------------------------------------------------------*/
#include "Dispenser_Controller.h"
#include "Application_Interface.h"
#include "MyWota_IO_Expander_Adapter.h"
#include "USB_Logging.h"
#include "CLI_Processor.h"
#include "RS485_Protocol.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "System_Config.h"
#include "Heartbeat_Task.h"
#include "Event_Broker.h"
#include "Fault_Manager.h"
#include "Task_Stack_Config.h"
#include "YS_S201_Driver.h"
#include "Hardware_Access.h"
#include "RTC_Manager.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
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

/* Private variables -------------------------------------------------*/

/* Decoupled event-driven Auth/Card state variables */
static QueueHandle_t s_dispenser_event_queue = NULL;
static StaticQueue_t s_dispenser_queue_buffer;
static uint8_t s_dispenser_queue_storage[10 * sizeof(Event_t*)];
static bool g_auth_card_present = false;
static bool g_auth_card_ready = false;
static uint32_t g_auth_card_balance = 0;
static bool g_auth_card_admin = false;
static uint8_t g_auth_card_uid[7] = {0};
static uint8_t g_auth_card_uid_len = 0;

/* Semaphore and result to handle synchronous Topup and Init Customer requests */
static SemaphoreHandle_t s_trans_sem = NULL;
static StaticSemaphore_t s_trans_sem_buffer;
static DispenserResult_t s_trans_result = DISPENSER_RESULT_ERROR;

/* Local stats tracking */
static uint32_t g_dispenses_completed = 0;
static uint32_t g_dispenses_failed = 0;
static uint32_t g_total_volume_dispensed_ml = 0;

static void process_dispenser_event(Event_t const *e)
{
    if (e == NULL) return;
    
    switch (e->header.id) {
        case EVT_RFID_CARD_DETECTED: {
            Event_RFID_Detected_t* detected = (Event_RFID_Detected_t*)e;
            g_auth_card_present = true;
            g_auth_card_ready = true;
            g_auth_card_uid_len = detected->uid_len;
            if (detected->uid_len <= 7) {
                memcpy(g_auth_card_uid, detected->uid, detected->uid_len);
            }
            DISPENSER_DEBUG("Event: CARD_DETECTED");
            break;
        }
        case EVT_RFID_CARD_REMOVED: {
            g_auth_card_present = false;
            g_auth_card_ready = false;
            g_auth_card_balance = 0;
            g_auth_card_admin = false;
            g_auth_card_uid_len = 0;
            DISPENSER_DEBUG("Event: CARD_REMOVED");
            break;
        }
        case EVT_RFID_STATE_CHANGED: {
            Event_RFID_State_t* state_evt = (Event_RFID_State_t*)e;
            if (state_evt->card_state == 0) { // absent
                g_auth_card_present = false;
                g_auth_card_ready = false;
                g_auth_card_balance = 0;
                g_auth_card_admin = false;
                g_auth_card_uid_len = 0;
            } else {
                g_auth_card_present = true;
                g_auth_card_ready = (state_evt->transaction_state == 1); // READY
                g_auth_card_balance = state_evt->balance;
                g_auth_card_admin = (state_evt->flags & 0x02); // ADMIN_CARD flag
            }
            DISPENSER_DEBUG("Event: STATE_CHANGED present=%d, ready=%d, balance=%lu", 
                          g_auth_card_present, g_auth_card_ready, g_auth_card_balance);
            break;
        }
        case EVT_RFID_TRANSACTION_SUCCESS: {
            s_trans_result = DISPENSER_RESULT_OK;
            if (s_trans_sem != NULL) {
                xSemaphoreGive(s_trans_sem);
            }
            DISPENSER_DEBUG("Event: TRANSACTION_SUCCESS");
            break;
        }
        case EVT_RFID_TRANSACTION_FAILED: {
            s_trans_result = DISPENSER_RESULT_ERROR;
            if (s_trans_sem != NULL) {
                xSemaphoreGive(s_trans_sem);
            }
            DISPENSER_DEBUG("Event: TRANSACTION_FAILED");
            break;
        }
        default:
            break;
    }
}

/* Note: DispenserState_t enum is defined in Dispenser_Controller.h */

/* Timing configuration */
#define DISPENSER_DEDUCTION_INTERVAL_MS   50     /* Update in-memory balance every 50ms (faster response) */
#define DISPENSER_PROGRESS_EVENT_INTERVAL_MS 250U

static Dispenser_TimerState_t g_dispense_timer = {0};
static DispenserState_t g_dispenser_state = DISPENSER_IDLE;
static uint8_t g_last_error = RS485_ERR_NONE;
static TaskHandle_t dispenser_task_handle = NULL;
static StaticTask_t dispenser_task_tcb;
static StackType_t dispenser_task_stack[DISPENSER_TASK_STACK_WORDS];

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

/* Self-clean (CCH-orchestrated periodic flush) ------------------------------*/
static bool     g_self_clean_active = false;
static bool     g_self_clean_pending_safety = false;  /* one-shot boot fallback */
static uint32_t g_self_clean_target_ml = 0;
static uint32_t g_self_clean_max_duration_ms = 0;
static uint32_t g_self_clean_start_tick = 0;
static float    g_self_clean_start_volume_ml = 0.0f;
static float    g_self_clean_last_flow_ml = 0.0f;
static uint32_t g_self_clean_last_flow_tick = 0;
#define DISPENSER_SELF_CLEAN_FLOW_WATCHDOG_MS  3000

/*Private function prototypes ---------------------------------------*/
static void dispenser_start_dispense(void);
static void dispenser_start_dispense_internal(bool no_card_mode, uint32_t target_ml);
static void dispenser_stop_dispense(const char* reason, uint8_t error_code, Event_Operation_StopReason_t stop_reason);
static bool dispense(uint32_t elapsed_ms);
static void dispenser_publish_progress_event(uint32_t amount_ml, uint32_t remaining_ml, bool force);
static void dispenser_publish_status_event(bool force);
static bool dispenser_has_balance(void);
static void dispenser_valve_open(void);
static void dispenser_valve_close(void);
static const char* dispenser_get_valve_state_name(ValveState_t state);
static void dispenser_self_clean_finish(bool success, const char* reason);
static void dispenser_self_clean_step(void);
static void dispenser_clear_wait_for_flow_mode(void);

/*Public Functions ---------------------------------------------------*/

uint8_t Dispenser_GetLastError(void)
{
    return g_last_error;
}

void Dispenser_ClearLastError(void)
{
    g_last_error = RS485_ERR_NONE;
    dispenser_publish_status_event(true);
}

static uint16_t dispenser_get_flow_clpm(void)
{
    if (!g_flow_sensor_initialized) {
        return 0;
    }

    YS_S201_FlowData_t flow_data;
    if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) != YS_S201_OK) {
        return 0;
    }

    float centilitres_per_minute = flow_data.flow_rate_lpm * 100.0f;
    if (centilitres_per_minute < 0.0f) {
        centilitres_per_minute = 0.0f;
    }
    if (centilitres_per_minute > 65000.0f) {
        centilitres_per_minute = 65000.0f;
    }
    return (uint16_t)centilitres_per_minute;
}

static void dispenser_publish_status_event(bool force)
{
    uint32_t balance_ml = 0;
    uint32_t remaining_ml = 0;
    uint8_t flags = 0;
    uint8_t peripheral_request_id = (uint8_t)RS485_PERIPHERAL_NONE;
    uint8_t peripheral_request_level = RS485_PERIPHERAL_LEVEL_OFF;

    if (g_dispense_timer.dispense_active) {
        flags |= EVENT_DISPENSER_FLAG_ACTIVE;
    }
    if (g_no_card_mode) {
        flags |= EVENT_DISPENSER_FLAG_NO_CARD_MODE;
        remaining_ml = g_target_volume_ml;
    } else {
        balance_ml = g_auth_card_balance;
        remaining_ml = g_auth_card_balance;
    }
    if (g_auth_card_present) {
        flags |= EVENT_DISPENSER_FLAG_CARD_PRESENT;
    }
    if (g_self_clean_active) {
        flags |= EVENT_DISPENSER_FLAG_SELF_CLEAN;
    }
    if (g_wait_for_flow_mode) {
        flags |= EVENT_DISPENSER_FLAG_WAIT_FOR_FLOW;
    }
    if (g_dispense_timer.valve_state == VALVE_OPEN) {
        flags |= EVENT_DISPENSER_FLAG_VALVE_OPEN;
    }

    Dispenser_GetPeripheralRequest(&peripheral_request_id, &peripheral_request_level);

    uint16_t flow_clpm = dispenser_get_flow_clpm();

    static uint8_t last_state = 0xFF;
    static uint8_t last_flags = 0xFF;
    static uint8_t last_error_code = 0xFF;
    static uint8_t last_peripheral_request_id = 0xFF;
    static uint8_t last_peripheral_request_level = 0xFF;
    static uint32_t last_balance_ml = UINT32_MAX;
    static uint32_t last_remaining_ml = UINT32_MAX;
    static uint32_t last_dispensed_ml = UINT32_MAX;
    static uint16_t last_flow_clpm = UINT16_MAX;

    if (!force &&
        last_state == (uint8_t)g_dispenser_state &&
        last_flags == flags &&
        last_error_code == g_last_error &&
        last_peripheral_request_id == peripheral_request_id &&
        last_peripheral_request_level == peripheral_request_level &&
        last_balance_ml == balance_ml &&
        last_remaining_ml == remaining_ml &&
        last_dispensed_ml == g_dispense_timer.balance_deducted_ml &&
        last_flow_clpm == flow_clpm) {
        return;
    }

    Event_Dispenser_Status_t* status_evt = (Event_Dispenser_Status_t*)EventPool_Alloc(EVT_DISPENSER_STATUS_CHANGED,
                                                                                       sizeof(Event_Dispenser_Status_t));
    if (status_evt == NULL) {
        return;
    }

    status_evt->state = (uint8_t)g_dispenser_state;
    status_evt->flags = flags;
    status_evt->error_code = g_last_error;
    status_evt->peripheral_request_id = peripheral_request_id;
    status_evt->peripheral_request_level = peripheral_request_level;
    memset(status_evt->reserved, 0, sizeof(status_evt->reserved));
    status_evt->balance_ml = balance_ml;
    status_evt->remaining_ml = remaining_ml;
    status_evt->dispensed_ml = g_dispense_timer.balance_deducted_ml;
    status_evt->flow_clpm = flow_clpm;

    EventBroker_Publish((Event_t*)status_evt);

    last_state = (uint8_t)g_dispenser_state;
    last_flags = flags;
    last_error_code = g_last_error;
    last_peripheral_request_id = peripheral_request_id;
    last_peripheral_request_level = peripheral_request_level;
    last_balance_ml = balance_ml;
    last_remaining_ml = remaining_ml;
    last_dispensed_ml = g_dispense_timer.balance_deducted_ml;
    last_flow_clpm = flow_clpm;
}

static void dispenser_publish_progress_event(uint32_t amount_ml, uint32_t remaining_ml, bool force)
{
    static uint32_t last_progress_event_tick = 0;
    static uint32_t last_progress_amount_ml = UINT32_MAX;
    static uint32_t last_progress_remaining_ml = UINT32_MAX;

    uint32_t now = xTaskGetTickCount();
    bool interval_elapsed = pdTICKS_TO_MS(now - last_progress_event_tick) >= DISPENSER_PROGRESS_EVENT_INTERVAL_MS;
    bool value_changed = (amount_ml != last_progress_amount_ml) ||
                         (remaining_ml != last_progress_remaining_ml);

    if (!force && (!interval_elapsed || !value_changed)) {
        return;
    }

    Event_Operation_Progress_t* progress_evt = (Event_Operation_Progress_t*)EventPool_Alloc(EVT_OPERATION_PROGRESS,
                                                                                             sizeof(Event_Operation_Progress_t));
    if (progress_evt == NULL) {
        return;
    }

    progress_evt->operation_kind = EVENT_OPERATION_KIND_DISPENSE;
    progress_evt->operation_mode = g_no_card_mode ? EVENT_OPERATION_MODE_MANUAL : EVENT_OPERATION_MODE_AUTO;
    progress_evt->reserved = 0;
    progress_evt->amount_ml = amount_ml;
    progress_evt->remaining_ml = remaining_ml;

    EventBroker_Publish((Event_t*)progress_evt);
    last_progress_event_tick = now;
    last_progress_amount_ml = amount_ml;
    last_progress_remaining_ml = remaining_ml;
    dispenser_publish_status_event(force);
}


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

    // Fault subsystem (idempotent). Filter life is tracked at the CCH side
    // because the site shares a single physical filter across all dispensers.
    Fault_Manager_Init();
    
    // Initialize Event Queue and Semaphore
    s_dispenser_event_queue = xQueueCreateStatic(10, sizeof(Event_t*), s_dispenser_queue_storage, &s_dispenser_queue_buffer);
    if (s_dispenser_event_queue != NULL) {
        EventBroker_Subscribe(s_dispenser_event_queue, EVT_RFID_CARD_DETECTED);
        EventBroker_Subscribe(s_dispenser_event_queue, EVT_RFID_CARD_REMOVED);
        EventBroker_Subscribe(s_dispenser_event_queue, EVT_RFID_STATE_CHANGED);
        EventBroker_Subscribe(s_dispenser_event_queue, EVT_RFID_TRANSACTION_SUCCESS);
        EventBroker_Subscribe(s_dispenser_event_queue, EVT_RFID_TRANSACTION_FAILED);
    }
    
    s_trans_sem = xSemaphoreCreateBinaryStatic(&s_trans_sem_buffer);

    // Initialize YS-S201 water flow sensor (GPIO 22)
    App_GPIO_Pins_t gpio_pins = Get_App_GPIO_Pins();
    YS_S201_Status_t flow_status = YS_S201_Init(&g_flow_sensor_handle, gpio_pins.flow_sensor_pin);
    if (flow_status == YS_S201_OK) {
        // Start flow measurement
        flow_status = YS_S201_Start(&g_flow_sensor_handle);
        if (flow_status == YS_S201_OK) {
            g_flow_sensor_initialized = true;
            DISPENSER_CRITICAL("[✓] Flow sensor initialized on GPIO %lu", gpio_pins.flow_sensor_pin);

            /* Apply configured flow calibration (pulses/L). 0 = keep driver default. */
            uint16_t flow_ppl = g_system_config.dispenser_logic.flow_pulses_per_liter;
            if (flow_ppl != 0) {
                if (YS_S201_SetCalibration(&g_flow_sensor_handle, flow_ppl) == YS_S201_OK) {
                    DISPENSER_CRITICAL("[✓] Flow calibration set to %u pulses/L", flow_ppl);
                } else {
                    DISPENSER_ERROR("[✗] Flow calibration value invalid: %u pulses/L", flow_ppl);
                }
            }
        } else {
            DISPENSER_ERROR("[✗] Flow sensor start failed: %d", flow_status);
        }
    } else {
        DISPENSER_ERROR("[✗] Flow sensor init failed: %d", flow_status);
    }
    
    // Register legacy application interface for command/RS485 snapshots
    const Application_Instance_t* app_interface = Dispenser_GetApplicationInterface();
    Application_Result_t app_result = Application_Register(app_interface);
    if (app_result == APP_RESULT_OK) {
        DISPENSER_CRITICAL("[✓] Dispenser application interface registered");
    } else {
        DISPENSER_ERROR("[✗] Failed to register application interface: %d", app_result);
    }
    
    DISPENSER_CRITICAL("[✓] Dispenser system initialized");
    dispenser_publish_status_event(true);

    /* Arm boot-time safety self-clean if configured and last clean is stale.
     * Only fires when CCH appears absent (i.e. no clean has been commanded
     * within self_clean_safety_max_hours). The actual cycle runs on first
     * IDLE tick of the task loop. */
    {
        const DispenserLogic_Config_t* dcfg = &g_system_config.dispenser_logic;
        if (dcfg->self_clean_safety_boot_enabled) {
            time_t now = RTC_GetUnixTime();
            uint32_t last = dcfg->last_clean_unix_time;
            uint32_t threshold_sec = dcfg->self_clean_safety_max_hours * 3600u;
            if ((now > 0) && (last == 0 || ((uint32_t)now - last) > threshold_sec)) {
                g_self_clean_pending_safety = true;
                DISPENSER_CRITICAL("[→] Boot safety self-clean armed (last=%lu, now=%lu)",
                                   (unsigned long)last, (unsigned long)now);
            }
        }
    }

    return DISPENSER_RESULT_OK;
}

/**
 * @brief Check if card has balance available
 * @return true if balance_ml > 0
 */
static bool dispenser_has_balance(void)
{
    bool has_balance = (g_auth_card_balance > 0);
    DISPENSER_DEBUG("has_balance: balance=%lu ml, result=%s", g_auth_card_balance, has_balance ? "YES" : "NO");
    return has_balance;
}

/**
 * @brief Start the dispense
 * @param no_card_mode If true, dispense without card (for testing)
 * @param target_ml Target volume in ml (only used in no_card_mode, 0 = unlimited)
 */
static void dispenser_start_dispense_internal(bool no_card_mode, uint32_t target_ml)
{
    uint32_t start_balance_ml = 0;

    if (!no_card_mode) {
        if (!g_auth_card_present || g_auth_card_balance == 0) {
            DISPENSER_ERROR("Cannot start dispense - no balance");
            return;
        }
        start_balance_ml = g_auth_card_balance;
    }
    
    g_no_card_mode = no_card_mode;
    g_target_volume_ml = target_ml;
    Dispenser_ClearLastError();
    if (no_card_mode) {
        dispenser_clear_wait_for_flow_mode();
    }
    
    g_dispense_timer.dispense_start_time = xTaskGetTickCount();
    g_dispense_timer.last_deduction_time = g_dispense_timer.dispense_start_time;
    g_dispense_timer.dispense_active = true;
    g_dispense_timer.balance_deducted_ml = 0;
    g_dispense_timer.transaction_counted = false;  /* Will increment counter on first deduction */
    g_dispenser_state = DISPENSER_DISPENSE_IN_PROGRESS;

    // Publish EVT_OPERATION_START event to represent dispense start
    Event_Operation_Start_t* start_evt = (Event_Operation_Start_t*)EventPool_Alloc(EVT_OPERATION_START,
                                                                                   sizeof(Event_Operation_Start_t));
    if (start_evt != NULL) {
        start_evt->operation_kind = EVENT_OPERATION_KIND_DISPENSE;
        start_evt->operation_mode = no_card_mode ? EVENT_OPERATION_MODE_MANUAL : EVENT_OPERATION_MODE_AUTO;
        start_evt->reserved = 0;
        start_evt->target_ml = target_ml;
        start_evt->balance_ml = start_balance_ml;
        EventBroker_Publish((Event_t*)start_evt);
    }
    dispenser_publish_progress_event(0, no_card_mode ? target_ml : start_balance_ml, true);

    
    // Reset flow sensor volume to 0 at the start of each dispense session.
    // This clears any residual pulses (deceleration) from previous sessions.
    if (g_flow_sensor_initialized) {
        YS_S201_Reset(&g_flow_sensor_handle);
        g_dispense_start_volume_ml = 0.0f;
        g_last_flow_volume_ml = 0.0f;
        DISPENSER_DEBUG("Flow sensor reset for new session");

        /* Re-apply flow calibration from current config so a runtime change via
         * `config set dispenser.flow_pulses_per_liter` takes effect on the next
         * session without a reboot. 0 = keep driver default. */
        uint16_t flow_ppl = g_system_config.dispenser_logic.flow_pulses_per_liter;
        if (flow_ppl != 0) {
            YS_S201_SetCalibration(&g_flow_sensor_handle, flow_ppl);
        }
    }
    
    // Initialize flow watchdog timer
    g_last_flow_change_tick = xTaskGetTickCount();
    
    // Open the valve to start dispensing
    dispenser_valve_open();
    dispenser_publish_status_event(true);
    
    if (no_card_mode) {
        if (target_ml > 0) {
            DISPENSER_CRITICAL("[✓] Dispense STARTED - valve OPEN (no-card mode, target: %lu ml)", target_ml);
        } else {
            DISPENSER_CRITICAL("[✓] Dispense STARTED - valve OPEN (no-card mode, unlimited)");
        }
    } else {
        DISPENSER_CRITICAL("[✓] Dispense STARTED - valve OPEN (balance: %lu ml)", g_auth_card_balance);
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
static void dispenser_stop_dispense(const char* reason, uint8_t error_code, Event_Operation_StopReason_t stop_reason)
{
    if (!g_dispense_timer.dispense_active) {
        return;
    }
    bool stopped_no_card_mode = g_no_card_mode;
    
    // Set last error
    g_last_error = error_code;
    
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
    (void)dispensed_ml;

    Event_Operation_Stop_t* stop_evt = (Event_Operation_Stop_t*)EventPool_Alloc(EVT_OPERATION_STOP,
                                                                                sizeof(Event_Operation_Stop_t));
    if (stop_evt != NULL) {
        stop_evt->operation_kind = EVENT_OPERATION_KIND_DISPENSE;
        stop_evt->operation_mode = stopped_no_card_mode ? EVENT_OPERATION_MODE_MANUAL : EVENT_OPERATION_MODE_AUTO;
        stop_evt->stop_reason = (uint8_t)stop_reason;
        stop_evt->error_code = error_code;
        stop_evt->amount_ml = (uint32_t)(dispensed_ml + 0.5f);
        EventBroker_Publish((Event_t*)stop_evt);
    }
    
    if (error_code == RS485_ERR_NONE) {
        g_dispenses_completed++;
        g_total_volume_dispensed_ml += (uint32_t)(dispensed_ml + 0.5f);
    } else {
        g_dispenses_failed++;
    }

    if (g_no_card_mode) {
        // No-card mode: just log completion
        DISPENSER_CRITICAL("[✓] Dispense STOPPED - %s (dispensed: %.0f ml)", reason, dispensed_ml);
    } else {
        DISPENSER_CRITICAL("[✓] Dispense STOPPED - %s (deducted: %lu ml this session)", 
                          reason, g_dispense_timer.balance_deducted_ml);
    }
    
    g_dispense_timer.valve_state = VALVE_CLOSED;

    /* Publish the final status event while g_no_card_mode and g_target_volume_ml
     * still reflect this session.  Clearing them first caused the else-branch of
     * dispenser_publish_status_event to fall back to stale MIFARE user_data,
     * emitting a non-zero balance_ml (e.g. 252 000 ml from the last card session)
     * that briefly corrupted the LCD cardRemaining field. */
    dispenser_publish_status_event(true);

    // Clear no-card mode
    g_no_card_mode = false;
    g_target_volume_ml = 0;
    
    // Clear wait-and-dispense mode
    g_wait_for_flow_mode = false;
    g_max_dispense_volume_ml = 0;
    g_flow_started = false;

    /* Filter usage is tracked at the CCH (single shared filter per site).
     * The CCH derives volume from RS485 status (status.total_dispensed delta
     * on dispense session-end), so the slave does not persist anything here. */

    /* Fault state machine: map error_code -> reason. Successful sessions
     * (RS485_ERR_NONE) and benign stops (CARD_REMOVED, EMERGENCY_STOP) count
     * toward recovery; flow / valve / sensor errors trigger Fault_Report. */
    switch (error_code) {
        case RS485_ERR_NO_FLOW:
            Fault_Manager_Report(RS485_FAULT_REASON_NO_FLOW);
            break;
        case RS485_ERR_VALVE_FAULT:
            Fault_Manager_Report(RS485_FAULT_REASON_VALVE);
            break;
        case RS485_ERR_SENSOR_FAULT:
            Fault_Manager_Report(RS485_FAULT_REASON_FLOW_SENSOR);
            break;
        case RS485_ERR_NONE:
        case RS485_ERR_CARD_REMOVED:
        case RS485_ERR_EMERGENCY_STOP:
            Fault_Manager_NoteSuccess();
            break;
        default:
            /* Other errors (LOW_BALANCE, CARD_WR_FAILED, GENERAL,
             * DAILY_LIMIT) are not hardware faults - leave state alone. */
            break;
    }
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
        dispenser_stop_dispense("No flow timeout", RS485_ERR_NO_FLOW, EVENT_OPERATION_STOP_REASON_NO_FLOW);
        
        // Only set error state if in normal mode AND card is still present
        if (!g_no_card_mode && g_auth_card_present) {
            // Check if this is actually a balance issue
            if (g_auth_card_balance == 0) {
                DISPENSER_CRITICAL("[✗] Insufficient balance to dispense");
                DISPENSER_CRITICAL("[→] Card balance is 0 ml - please top up.");
            } else {
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
        uint32_t remaining_ml = 0;
        if (g_target_volume_ml > g_dispense_timer.balance_deducted_ml) {
            remaining_ml = g_target_volume_ml - g_dispense_timer.balance_deducted_ml;
        }
        dispenser_publish_progress_event(g_dispense_timer.balance_deducted_ml, remaining_ml, false);
        DISPENSER_DEBUG("No-card mode: total_deducted=%lu ml, volume_since_start=%.1f ml", 
                       g_dispense_timer.balance_deducted_ml, volume_since_start);
        
        if (g_target_volume_ml > 0 && volume_since_start >= (float)g_target_volume_ml) {
            dispenser_stop_dispense("Target volume reached", RS485_ERR_NONE, EVENT_OPERATION_STOP_REASON_COMPLETE);
            return false;
        }
        return true;  // Continue dispensing
    }
    
    // STOP CONDITIONS 3-5 (card mode): Card removed, balance exhausted handled below
    
    // Card mode: deduct from balance
    if (!g_auth_card_present) {
        dispenser_stop_dispense("Card removed", RS485_ERR_CARD_REMOVED, EVENT_OPERATION_STOP_REASON_CARD_REMOVED);
        return false;
    }
    
    // Calculate how much NEW volume to deduct (delta since last deduction)
    uint32_t total_used_ml = (uint32_t)(volume_since_start + 0.5f);
    uint32_t to_deduct = 0;
    if (total_used_ml > g_dispense_timer.balance_deducted_ml) {
        to_deduct = total_used_ml - g_dispense_timer.balance_deducted_ml;
    }
    
    // Clamp to available balance
    if (to_deduct > g_auth_card_balance) {
        to_deduct = g_auth_card_balance;  // Don't go negative
    }
    
    if (to_deduct > 0) {
        // Update balance locally
        g_auth_card_balance -= to_deduct;
        
        // Track total deducted this session (in ml)
        g_dispense_timer.balance_deducted_ml += to_deduct;
        
        DISPENSER_DEBUG("Deducted: to_deduct=%lu ml, total_deducted=%lu ml, volume_since_start=%.1f ml", 
                       to_deduct, g_dispense_timer.balance_deducted_ml, volume_since_start);
        dispenser_publish_progress_event(g_dispense_timer.balance_deducted_ml, g_auth_card_balance, false);
        
        // STOP CONDITION: Wait-for-flow mode - volume limit reached (check AFTER deduction)
        if (g_wait_for_flow_mode && g_flow_started) {
            if (g_dispense_timer.balance_deducted_ml >= g_max_dispense_volume_ml) {
                DISPENSER_CRITICAL("[✓] Volume limit reached: %lu ml / %lu ml", 
                                 g_dispense_timer.balance_deducted_ml, g_max_dispense_volume_ml);
                dispenser_stop_dispense("Volume limit reached", RS485_ERR_NONE, EVENT_OPERATION_STOP_REASON_COMPLETE);
                return false;
            }
        }
    }
    
    // Check if balance exhausted
    if (g_auth_card_balance == 0) {
        dispenser_stop_dispense("Balance exhausted", RS485_ERR_LOW_BALANCE, EVENT_OPERATION_STOP_REASON_BALANCE);
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
    if (g_auth_card_ready && dispenser_has_balance()) {
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
    status->card_present = g_auth_card_present;
    status->dispense_bay_id = g_dispense_timer.dispense_bay_id;
    
    // Get balance (0 in no-card mode)
    if (g_no_card_mode) {
        status->balance_ml = 0;
        status->remaining_ml = g_target_volume_ml;  // Target volume in no-card mode
    } else {
        status->balance_ml = g_auth_card_balance;
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
    if (g_self_clean_active) {
        dispenser_self_clean_finish(false, "emergency stop");
        return DISPENSER_RESULT_OK;
    }

    if (!g_dispense_timer.dispense_active && g_wait_for_flow_mode) {
        dispenser_clear_wait_for_flow_mode();
        DISPENSER_CRITICAL("[✓] Wait-and-dispense cancelled - emergency stop");
        return DISPENSER_RESULT_OK;
    }

    dispenser_stop_dispense("Emergency stop", RS485_ERR_EMERGENCY_STOP, EVENT_OPERATION_STOP_REASON_EMERGENCY);
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

    if (g_wait_for_flow_mode) {
        DISPENSER_ERROR("Manual start failed - wait-and-dispense is armed");
        return DISPENSER_RESULT_BUSY;
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
        if (g_wait_for_flow_mode) {
            dispenser_clear_wait_for_flow_mode();
            DISPENSER_CRITICAL("[✓] Wait-and-dispense cancelled - manual stop");
            return DISPENSER_RESULT_OK;
        }

        DISPENSER_DEBUG("Manual stop - no dispense active");
        return DISPENSER_RESULT_OK;
    }
    
    dispenser_stop_dispense("Manual stop", RS485_ERR_NONE, EVENT_OPERATION_STOP_REASON_MANUAL);
    
    return DISPENSER_RESULT_OK;
}

/**
 * @brief Check if dispense is currently active (legacy snapshot helper)
 * @return true if dispense in progress, false otherwise
 */
bool MIFARE_Dispenser_IsDispenseActive(void)
{
    return g_dispense_timer.dispense_active;
}

/**
 * @brief Get amount dispensed in current session (legacy snapshot helper)
 * @return Amount dispensed in milliliters (0 if no dispense or nothing dispensed)
 */
uint32_t Dispenser_GetDispensedAmountML(void)
{
    return g_dispense_timer.balance_deducted_ml;
}

/**
 * @brief Dispenser polling task - UNIFIED STATE MACHINE
 * 
 * 4. Stops on: card removal, balance=0, target reached, no flow, or manual stop.
 */
static void MIFARE_Dispenser_Task(void* argument)
{
    (void)argument;
    
    DISPENSER_CRITICAL("[→] Dispenser task started");
    
    while (1) {
        // Feed watchdog every second
        TASK_HEARTBEAT_EVERY_SECOND("Dispenser");
        System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
        
        // Drain event queue to update auth state
        if (s_dispenser_event_queue != NULL) {
            Event_t* evt = NULL;
            while (xQueueReceive(s_dispenser_event_queue, &evt, 0) == pdTRUE) {
                if (evt != NULL) {
                    process_dispenser_event(evt);
                    Event_Release(evt);
                }
            }
        }
        
        // In no-card mode, skip card-related checks in IDLE state
        bool card_ready = false;
        static bool last_card_ready = false;
        if (!g_no_card_mode) {
            card_ready = g_auth_card_ready;
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
                    if (g_auth_card_admin) {
                        DISPENSER_DEBUG("IDLE: Admin card present - no auto-dispense");
                        break;
                    }
                    
                    DISPENSER_DEBUG("IDLE: Card ready detected - checking for pending commands");
                    CLI_PendingCommandState_t* pending = CLI_GetPendingCommand();
                    if (pending != NULL && pending->active) {
                        DISPENSER_DEBUG("IDLE: Pending USB command found (cmd=%d) - skipping auto-dispense", pending->command);
                        bool command_executed = false;
                        // For RECOVER command, verify UID matches before executing
                        if (pending->command == CLI_PENDING_CMD_RECOVER) {
                            if (g_auth_card_uid_len == pending->target_uid_length &&
                                memcmp(g_auth_card_uid, pending->target_uid, pending->target_uid_length) == 0) {
                                // UID matches - execute recovery
                                CLI_ExecutePendingCommand(pending);
                                command_executed = true;
                            }
                        } else {
                            // Other commands (topup, cardinit) - execute immediately
                            CLI_ExecutePendingCommand(pending);
                            DISPENSER_DEBUG("IDLE: USB command executed");
                            command_executed = true;
                        }
                        // A top-up/init/recover takes priority over dispensing. After it
                        // runs, require the card to be removed before any other operation
                        // (including auto-dispense) can start - prevents the just-topped-up
                        // card from immediately auto-dispensing on the next poll cycle.
                        if (command_executed) {
                            DISPENSER_CRITICAL("[→] Card operation complete - remove card before next operation");
                            g_dispenser_state = DISPENSER_WAITING_FOR_REMOVAL;
                            g_card_last_seen_tick = xTaskGetTickCount();
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
                        bool was_no_card_mode = g_no_card_mode;
                        bool should_continue = dispense(elapsed_since_deduction);
                        uint8_t stop_error = g_last_error;
                        g_dispense_timer.last_deduction_time = current_time;
                        
                        if (!should_continue) {
                            // Dispense stopped - transition based on mode and reason
                            if (!was_no_card_mode && g_auth_card_present) {
                                // Card mode, card still present - wait for removal
                                g_dispenser_state = DISPENSER_WAITING_FOR_REMOVAL;
                                g_card_last_seen_tick = current_time;
                            } else {
                                // No-card mode OR card removed - go to IDLE
                                g_dispenser_state = DISPENSER_IDLE;
                                if (stop_error == RS485_ERR_NONE || stop_error == RS485_ERR_CARD_REMOVED) {
                                    g_last_error = RS485_ERR_NONE;
                                }
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
                    // Check if card is no longer present (removed by user)
                    bool card_is_present = g_auth_card_present;
                    
                    if (card_is_present) {
                        // Card still present - keep waiting for removal
                        DISPENSER_DEBUG("WAITING_FOR_REMOVAL: Card still present");
                    } else {
                        // Card removed - return to IDLE immediately
                        DISPENSER_CRITICAL("[✓] Card removed - returning to IDLE");
                        g_dispenser_state = DISPENSER_IDLE;
                        g_last_error = RS485_ERR_NONE;
                    }
                }
                break;

            case DISPENSER_SELF_CLEANING:
                /* Drives valve + flow watchdog until target / timeout / abort. */
                dispenser_self_clean_step();
                break;
        }
        
        /* Run any pending boot-time safety self-clean exactly once, only when
         * truly idle (no card, no dispense). */
        if (g_self_clean_pending_safety &&
            g_dispenser_state == DISPENSER_IDLE &&
            !g_auth_card_present) {
            g_self_clean_pending_safety = false;
            DISPENSER_CRITICAL("[→] Running boot safety self-clean");
            (void)Dispenser_StartSelfClean(0, 0);  /* use config defaults */
        }

        uint32_t poll_delay_ms =
            (g_dispenser_state == DISPENSER_DISPENSE_IN_PROGRESS ||
             g_dispenser_state == DISPENSER_SELF_CLEANING) ? 20 : 100;
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

    dispenser_task_handle = xTaskCreateStatic(MIFARE_Dispenser_Task, 
                                    "Dispenser",  // Must match WDT tracking name in System.c
                                    DISPENSER_TASK_STACK_WORDS,
                                    NULL, 
                                    tskIDLE_PRIORITY + 1, 
                                    dispenser_task_stack,
                                    &dispenser_task_tcb);
    
    BaseType_t result = (dispenser_task_handle != NULL) ? pdPASS : pdFAIL;
    
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
            dispenser_stop_dispense("Task stopped", RS485_ERR_NONE, EVENT_OPERATION_STOP_REASON_MANUAL);
        }
        vTaskDelete(dispenser_task_handle);
        dispenser_task_handle = NULL;
        DISPENSER_CRITICAL("[✓] Task stopped");
    }
}

TaskHandle_t Dispenser_Task_GetHandle(void)
{
    return dispenser_task_handle;
}

/* Legacy Snapshot Helper Functions -----------------------------------------*/

uint32_t Dispenser_GetBalanceMl(void)
{
    // In no-card mode, no balance tracking
    if (g_no_card_mode) {
        return 0;
    }
    
    return g_auth_card_balance;
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

/* MyWota dispensers always drive booster pump #1 when active.
 * TODO: expose this as a config item (e.g. dispenser.pump_id) so multiple
 *       MyWota slaves on the same bus can be assigned different boosters. */
#ifndef MYWOTA_PERIPHERAL_ID
#define MYWOTA_PERIPHERAL_ID  RS485_PERIPHERAL_BOOSTER_1
#endif

void Dispenser_GetPeripheralRequest(uint8_t *out_pump_id, uint8_t *out_level)
{
    if (out_pump_id == NULL || out_level == NULL) {
        return;
    }

    bool wants_pump = false;

    /* Active dispense always needs the booster pressurized. */
    if (g_dispense_timer.dispense_active) {
        wants_pump = true;
    }
    /* Self-clean cycle drives water through the line - same booster needed. */
    else if (g_self_clean_active) {
        wants_pump = true;
    }
    /* Card validated with balance: prime the booster so water flows
     * immediately when the valve opens. Once balance hits zero or the
     * card is removed, the MIFARE state leaves READY and this clears. */
    else if (g_auth_card_ready && dispenser_has_balance()) {
        wants_pump = true;
    }

    if (wants_pump) {
        *out_pump_id = (uint8_t)MYWOTA_PERIPHERAL_ID;
        *out_level   = RS485_PERIPHERAL_LEVEL_MAX;  /* Boosters are ON/OFF; max = ON */
    } else {
        *out_pump_id = (uint8_t)RS485_PERIPHERAL_NONE;
        *out_level   = RS485_PERIPHERAL_LEVEL_OFF;
    }
}

uint32_t Dispenser_GetTotalDispensesCompleted(void)
{
    return g_dispenses_completed;
}

uint32_t Dispenser_GetTotalVolumePurchasedMl(void)
{
    return g_total_volume_dispensed_ml;
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

/* ---------------------------------------------------------------------------
 * Flow diagnostics ring (consumed by RS485 status responder).
 * Each call to Dispenser_SampleFlowDiagnostics() advances the ring with the
 * current instantaneous flow rate so the master can compute min/max even when
 * its effective sampling period exceeds 1 s (multiple slaves share the bus).
 * ------------------------------------------------------------------------ */

#define FLOW_DIAG_RING_DEPTH 8u
static uint16_t s_flow_diag_ring[FLOW_DIAG_RING_DEPTH] = {0};
static uint8_t  s_flow_diag_ring_head = 0;
static uint8_t  s_flow_diag_ring_count = 0;

void Dispenser_SampleFlowDiagnostics(uint16_t *flow_clpm,
                                     uint16_t *flow_clpm_min,
                                     uint16_t *flow_clpm_max)
{
    /* Convert L/min -> centiL/min, clamp to uint16 range. */
    uint16_t clpm = 0;
    if (g_flow_sensor_initialized) {
        YS_S201_FlowData_t flow_data;
        if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) == YS_S201_OK) {
            float v = flow_data.flow_rate_lpm * 100.0f;
            if (v < 0.0f) v = 0.0f;
            if (v > 65000.0f) v = 65000.0f;   /* leave 0xFFFF for "unknown" */
            clpm = (uint16_t)v;
        }
    }

    /* Push into ring */
    s_flow_diag_ring[s_flow_diag_ring_head] = clpm;
    s_flow_diag_ring_head = (uint8_t)((s_flow_diag_ring_head + 1u) % FLOW_DIAG_RING_DEPTH);
    if (s_flow_diag_ring_count < FLOW_DIAG_RING_DEPTH) {
        s_flow_diag_ring_count++;
    }

    /* Compute min/max across populated entries */
    uint16_t mn = clpm;
    uint16_t mx = clpm;
    for (uint8_t i = 0; i < s_flow_diag_ring_count; i++) {
        uint16_t v = s_flow_diag_ring[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }

    if (flow_clpm)     *flow_clpm     = clpm;
    if (flow_clpm_min) *flow_clpm_min = mn;
    if (flow_clpm_max) *flow_clpm_max = mx;
}

bool Dispenser_IsValveCommanded(void)
{
    return Dispenser_GetValveState() == VALVE_OPEN;
}

/* ---------------------------------------------------------------------------
 * Self-Clean (CCH-orchestrated periodic flush)
 * ------------------------------------------------------------------------ */

bool Dispenser_IsSelfCleaning(void)
{
    return g_self_clean_active;
}

uint32_t Dispenser_GetLastCleanUnixTime(void)
{
    return g_system_config.dispenser_logic.last_clean_unix_time;
}

DispenserState_t Dispenser_GetControllerState(void)
{
    return g_dispenser_state;
}

DispenserResult_t Dispenser_StartSelfClean(uint32_t volume_ml, uint32_t max_duration_sec)
{
    /* Reject if any "real" activity is in progress. Card-present check covers
     * both READY and INITIALIZING states; we never want to flush while a
     * customer's card is on the reader. */
    if (g_self_clean_active) {
        DISPENSER_CRITICAL("[!] Self-clean already in progress");
        return DISPENSER_RESULT_BUSY;
    }
    if (g_dispense_timer.dispense_active || g_dispenser_state != DISPENSER_IDLE) {
        DISPENSER_ERROR("[✗] Self-clean refused: dispenser not idle (state=%d)", g_dispenser_state);
        return DISPENSER_RESULT_BUSY;
    }
    if (g_wait_for_flow_mode) {
        DISPENSER_ERROR("[✗] Self-clean refused: wait-and-dispense is armed");
        return DISPENSER_RESULT_BUSY;
    }
    if (g_auth_card_present) {
        DISPENSER_ERROR("[✗] Self-clean refused: card present");
        return DISPENSER_RESULT_BUSY;
    }
    if (!g_flow_sensor_initialized) {
        DISPENSER_ERROR("[✗] Self-clean refused: flow sensor not initialised");
        return DISPENSER_RESULT_ERROR;
    }

    const DispenserLogic_Config_t* dcfg = &g_system_config.dispenser_logic;
    uint32_t target = (volume_ml > 0) ? volume_ml : dcfg->self_clean_volume_ml;
    uint32_t max_sec = (max_duration_sec > 0) ? max_duration_sec : dcfg->self_clean_max_duration_sec;
    if (target == 0) {
        target = 100;   /* hard-coded floor */
    }
    if (max_sec == 0) {
        max_sec = 30;
    }

    /* Snapshot flow baseline */
    YS_S201_FlowData_t flow_data;
    g_self_clean_start_volume_ml = 0.0f;
    if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) == YS_S201_OK) {
        g_self_clean_start_volume_ml = flow_data.total_volume_ml;
    }
    g_self_clean_last_flow_ml = g_self_clean_start_volume_ml;
    g_self_clean_target_ml = target;
    g_self_clean_max_duration_ms = max_sec * 1000u;
    g_self_clean_start_tick = xTaskGetTickCount();
    g_self_clean_last_flow_tick = g_self_clean_start_tick;
    g_self_clean_active = true;
    g_dispenser_state = DISPENSER_SELF_CLEANING;

    Event_Operation_Start_t* start_evt = (Event_Operation_Start_t*)EventPool_Alloc(EVT_OPERATION_START,
                                                                                   sizeof(Event_Operation_Start_t));
    if (start_evt != NULL) {
        start_evt->operation_kind = EVENT_OPERATION_KIND_SELF_CLEAN;
        start_evt->operation_mode = EVENT_OPERATION_MODE_AUTO;
        start_evt->reserved = 0;
        start_evt->target_ml = target;
        start_evt->balance_ml = 0;
        EventBroker_Publish((Event_t*)start_evt);
    }

    dispenser_valve_open();
    dispenser_publish_status_event(true);
    DISPENSER_CRITICAL("[→] Self-clean STARTED (target=%lu mL, max=%lu s)",
                       (unsigned long)target, (unsigned long)max_sec);
    return DISPENSER_RESULT_OK;
}

void Dispenser_StopSelfClean(void)
{
    if (g_self_clean_active) {
        dispenser_self_clean_finish(false, "manual stop");
    }
}

/**
 * @brief One iteration of the self-clean state.
 * @details Called from the dispenser task while g_dispenser_state ==
 *          DISPENSER_SELF_CLEANING. Stops on target volume, max duration,
 *          flow watchdog, or card-detect (real dispense pre-empts).
 */
static void dispenser_self_clean_step(void)
{
    if (!g_self_clean_active) {
        g_dispenser_state = DISPENSER_IDLE;
        return;
    }

    /* Card pre-empts a clean cycle so a real customer never waits. */
    if (g_auth_card_present) {
        dispenser_self_clean_finish(false, "card detected");
        return;
    }

    uint32_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = pdTICKS_TO_MS(now - g_self_clean_start_tick);

    /* Hard time cap */
    if (elapsed_ms >= g_self_clean_max_duration_ms) {
        dispenser_self_clean_finish(false, "max duration");
        return;
    }

    /* Read flow */
    YS_S201_FlowData_t flow_data;
    if (YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) != YS_S201_OK) {
        return;  /* transient sensor read failure - try again next tick */
    }
    float dispensed_ml = flow_data.total_volume_ml - g_self_clean_start_volume_ml;

    /* Flow watchdog: closed upstream / dry pipe */
    if (flow_data.total_volume_ml > g_self_clean_last_flow_ml) {
        g_self_clean_last_flow_ml = flow_data.total_volume_ml;
        g_self_clean_last_flow_tick = now;
    } else if (pdTICKS_TO_MS(now - g_self_clean_last_flow_tick) >= DISPENSER_SELF_CLEAN_FLOW_WATCHDOG_MS) {
        dispenser_self_clean_finish(false, "no flow");
        return;
    }

    /* Target reached */
    if (dispensed_ml >= (float)g_self_clean_target_ml) {
        dispenser_self_clean_finish(true, "target reached");
        return;
    }
}

static void dispenser_self_clean_finish(bool success, const char* reason)
{
    if (!g_self_clean_active) {
        return;
    }

    dispenser_valve_close();

    float dispensed_ml = 0.0f;
    YS_S201_FlowData_t flow_data;
    if (g_flow_sensor_initialized &&
        YS_S201_GetFlowData(&g_flow_sensor_handle, &flow_data) == YS_S201_OK) {
        dispensed_ml = flow_data.total_volume_ml - g_self_clean_start_volume_ml;
    }

    g_self_clean_active = false;
    g_dispenser_state = DISPENSER_IDLE;
    dispenser_publish_status_event(true);

    Event_Operation_Stop_t* stop_evt = (Event_Operation_Stop_t*)EventPool_Alloc(EVT_OPERATION_STOP,
                                                                                sizeof(Event_Operation_Stop_t));
    if (stop_evt != NULL) {
        stop_evt->operation_kind = EVENT_OPERATION_KIND_SELF_CLEAN;
        stop_evt->operation_mode = EVENT_OPERATION_MODE_AUTO;
        stop_evt->stop_reason = success ? EVENT_OPERATION_STOP_REASON_COMPLETE : EVENT_OPERATION_STOP_REASON_ERROR;
        stop_evt->error_code = success ? RS485_ERR_NONE : RS485_ERR_GENERAL;
        stop_evt->amount_ml = (uint32_t)(dispensed_ml + 0.5f);
        EventBroker_Publish((Event_t*)stop_evt);
    }

    if (success) {
        time_t now = RTC_GetUnixTime();
        if (now > 0) {
            Config_UpdateLastCleanTime((uint32_t)now);
        }
        DISPENSER_CRITICAL("[✓] Self-clean COMPLETE - %s (%.0f mL)", reason, dispensed_ml);
    } else {
        DISPENSER_CRITICAL("[✗] Self-clean ABORTED - %s (%.0f mL)", reason, dispensed_ml);
    }
}

static void dispenser_clear_wait_for_flow_mode(void)
{
    g_wait_for_flow_mode = false;
    g_max_dispense_volume_ml = 0;
    g_flow_started = false;
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
    
    if (s_trans_sem == NULL) {
        return DISPENSER_RESULT_ERROR;
    }
    
    // Allocate the request event
    Event_RFID_InitCustomerRequest_t* req = (Event_RFID_InitCustomerRequest_t*)EventPool_Alloc(
        EVT_RFID_INIT_CUSTOMER_REQUEST, sizeof(Event_RFID_InitCustomerRequest_t));
    if (req == NULL) {
        return DISPENSER_RESULT_ERROR;
    }
    req->initial_balance = initial_balance_ml;
    req->customer_id = customer_id;
    
    // Reset transaction result
    s_trans_result = DISPENSER_RESULT_ERROR;
    
    // Clear semaphore just in case it was left given
    xSemaphoreTake(s_trans_sem, 0);
    
    // Publish request
    EventBroker_Publish((Event_t*)req);
    
    // Wait on semaphore
    if (xSemaphoreTake(s_trans_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        if (s_trans_result == DISPENSER_RESULT_OK) {
            DISPENSER_CRITICAL("[✓] New customer initialized with %lu ml balance", initial_balance_ml);
        } else {
            DISPENSER_ERROR("Failed to initialize customer card");
        }
        return s_trans_result;
    }
    
    DISPENSER_ERROR("Failed to initialize customer: Timeout");
    return DISPENSER_RESULT_ERROR;
}

/**
 * @brief Add volume to card (top-up)
 * @param topup_ml Number of milliliters to add
 * @return DispenserResult_t Operation result
 */
DispenserResult_t MIFARE_Dispenser_TopupCard(uint32_t topup_ml)
{
    if (!g_auth_card_ready) {
        DISPENSER_ERROR("Card not ready for topup");
        return DISPENSER_RESULT_ERROR;
    }
    
    if (s_trans_sem == NULL) {
        return DISPENSER_RESULT_ERROR;
    }
    
    DISPENSER_LOG("Adding %lu ml to card", topup_ml);
    
    // Allocate the request event
    Event_RFID_TopupRequest_t* req = (Event_RFID_TopupRequest_t*)EventPool_Alloc(
        EVT_RFID_TOPUP_REQUEST, sizeof(Event_RFID_TopupRequest_t));
    if (req == NULL) {
        return DISPENSER_RESULT_ERROR;
    }
    req->amount = topup_ml;
    
    // Reset transaction result
    s_trans_result = DISPENSER_RESULT_ERROR;
    
    // Clear semaphore just in case it was left given
    xSemaphoreTake(s_trans_sem, 0);
    
    // Publish request
    EventBroker_Publish((Event_t*)req);
    
    // Wait on semaphore
    if (xSemaphoreTake(s_trans_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (s_trans_result == DISPENSER_RESULT_OK) {
            DISPENSER_CRITICAL("[✓] Added %lu ml to card", topup_ml);
            
            // Publish EVT_BALANCE_UPDATED to play the double beep feedback
            Event_t* updated_evt = EventPool_Alloc(EVT_BALANCE_UPDATED, sizeof(Event_t));
            if (updated_evt != NULL) {
                EventBroker_Publish(updated_evt);
            }
        } else {
            DISPENSER_ERROR("Failed to topup card");
        }
        return s_trans_result;
    }
    
    DISPENSER_ERROR("Failed to topup card: Timeout");
    return DISPENSER_RESULT_ERROR;
}

/* UI Update Functions ------------------------------------------------------*/

void MIFARE_Dispenser_UpdateUI(void)
{
    // UI updates are delivered through EventBroker subscriptions.
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
    if (gpio_pins.valve_control_pin != 0xFF) {
        gpio_put(gpio_pins.valve_control_pin, 1);  // Turn on valve
    }
    g_dispense_timer.valve_state = VALVE_OPEN;
    DISPENSER_CRITICAL("Valve OPEN (GPIO %lu)", gpio_pins.valve_control_pin);
}

/**
 * @brief Close the water dispenser valve
 */
static void dispenser_valve_close(void)
{
    App_GPIO_Pins_t gpio_pins = Get_App_GPIO_Pins();
    if (gpio_pins.valve_control_pin != 0xFF) {
        gpio_put(gpio_pins.valve_control_pin, 0);  // Turn off valve
    }
    g_dispense_timer.valve_state = VALVE_CLOSED;
    DISPENSER_CRITICAL("Valve CLOSED (GPIO %lu)", gpio_pins.valve_control_pin);
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
 * @brief Get current valve state (legacy snapshot helper)
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

/**
 * @brief Application interface: Trigger self-clean cycle
 */
static Application_Result_t dispenser_app_trigger_clean(uint32_t volume_ml, uint32_t max_sec)
{
    DispenserResult_t r = Dispenser_StartSelfClean(volume_ml, max_sec);
    return (r == DISPENSER_RESULT_OK) ? APP_RESULT_OK : APP_RESULT_ERROR;
}

/**
 * @brief Application interface: Abort self-clean cycle
 */
static Application_Result_t dispenser_app_abort_clean(void)
{
    Dispenser_StopSelfClean();
    return APP_RESULT_OK;
}

/**
 * @brief Application interface: Clear latched fault
 */
static Application_Result_t dispenser_app_clear_fault(void)
{
    Dispenser_ClearLastError();
    Fault_Manager_Clear();
    return APP_RESULT_OK;
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
    .task_stop = dispenser_app_task_stop,
    .trigger_clean = dispenser_app_trigger_clean,
    .abort_clean = dispenser_app_abort_clean,
    .clear_fault = dispenser_app_clear_fault
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

bool Dispenser_IsCardPresent(void)
{
    return g_auth_card_present;
}

bool Dispenser_IsCardReady(void)
{
    return g_auth_card_ready;
}

bool Dispenser_GetCardUID(uint8_t *uid_out, uint8_t *len_out)
{
    if (uid_out == NULL || len_out == NULL) {
        return false;
    }
    if (!g_auth_card_present) {
        *len_out = 0;
        return false;
    }
    uint8_t len = (g_auth_card_uid_len <= 7) ? g_auth_card_uid_len : 7;
    memcpy(uid_out, g_auth_card_uid, len);
    *len_out = len;
    return true;
}
