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
 * @file MIFARE_Volume_Adapter.c
 * @brief Volume-based adapter for MyWota water dispenser system
 * @details Implements volume-based balance tracking in milliliters
 *          - Deduct ml based on actual water dispensed
 *          - Balance stored in ml units
 */

/* Includes ------------------------------------------------------------------*/
#include "MIFARE_Transaction_Core.h"
#include "MIFARE_Card_Interface.h"
#include "System_Config.h"
#include "USB_Logging.h"
#include "RTC_Manager.h"
#include "Event_Broker.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "Task_Stack_Config.h"
#include "MIFARE_Async_Mailbox.h"
#include <string.h>

/* Daily-limit packing helpers ----------------------------------------------*/
/* card_data->usage_data.reserved layout:
 *   bits 31..16 : epoch_day_lo (UTC days since 1970-01-01, mod 65536)
 *   bits 15..0  : daily_used_cl (centiliters dispensed today, capped 65535, i.e., 655.35L)
 */
static inline uint16_t today_epoch_day_lo(void) {
    time_t now = RTC_GetUnixTime();
    uint32_t days = (uint32_t)(now / 86400);
    return (uint16_t)(days & 0xFFFFu);
}

static inline void daily_unpack(uint32_t raw, uint16_t *day, uint16_t *used_cl) {
    if (day)      *day      = (uint16_t)((raw >> 16) & 0xFFFFu);
    if (used_cl)  *used_cl  = (uint16_t)(raw & 0xFFFFu);
}

static inline uint32_t daily_pack(uint16_t day, uint16_t used_cl) {
    return ((uint32_t)day << 16) | (uint32_t)used_cl;
}

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_VOLUME_ADAPTER_EN      0
#define LOG_CRITICAL_VOLUME_ADAPTER_EN   1

#if LOG_DEBUG_VOLUME_ADAPTER_EN
    #define LOG_DEBUG_VOLUME(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_VOLUME(...)
#endif

#if LOG_CRITICAL_VOLUME_ADAPTER_EN
    #define LOG_CRITICAL_VOLUME(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_VOLUME(...)
#endif

/* Private function prototypes -----------------------------------------------*/
static MIFARE_Result_t volume_read_balance(MIFARE_CardData_t *card_data, uint32_t *balance_out);
static MIFARE_Result_t volume_write_balance(MIFARE_CardData_t *card_data, uint32_t new_balance);
static MIFARE_Result_t volume_deduct_balance(MIFARE_CardData_t *card_data, uint32_t amount);
static MIFARE_Result_t volume_add_balance(MIFARE_CardData_t *card_data, uint32_t amount);
static MIFARE_Result_t volume_format_card(MIFARE_CardData_t *card_data, uint32_t initial_balance, uint64_t customer_id);
static MIFARE_Result_t volume_on_card_detected(MIFARE_CardData_t *card_data);
static MIFARE_Result_t volume_on_card_removed(MIFARE_CardData_t *card_data);
static MIFARE_Result_t volume_on_transaction_start(MIFARE_CardData_t *card_data);
static MIFARE_Result_t volume_on_transaction_end(MIFARE_CardData_t *card_data);

/* Volume Interface Definition ------------------------------------------------*/
static const MIFARE_CardInterface_t volume_interface = {
    .name = "Volume",
    .balance_unit = "ml",
    .continuous_updates = false,        // Periodic writes handled by adapter task
    .fast_update_ms = 0,                // Not used
    
    // Virtual functions
    .read_balance = volume_read_balance,
    .write_balance = volume_write_balance,
    .deduct_balance = volume_deduct_balance,
    .add_balance = volume_add_balance,
    .format_card = volume_format_card,
    
    // Event callbacks
    .on_card_detected = volume_on_card_detected,
    .on_card_removed = volume_on_card_removed,
    .on_transaction_start = volume_on_transaction_start,
    .on_transaction_end = volume_on_transaction_end
};

/* Virtual Function Implementations ------------------------------------------*/

/**
 * @brief Read volume balance from card (in ml)
 */
static MIFARE_Result_t volume_read_balance(MIFARE_CardData_t *card_data, uint32_t *balance_out)
{
    if (!card_data || !balance_out) {
        return MIFARE_RESULT_ERROR;
    }
    
    *balance_out = card_data->user_primary.balance;
    LOG_DEBUG_VOLUME("[VOLUME] Read balance: %lu ml\r\n", *balance_out);
    return MIFARE_RESULT_OK;
}

/**
 * @brief Write volume balance to card (in ml)
 */
static MIFARE_Result_t volume_write_balance(MIFARE_CardData_t *card_data, uint32_t new_balance)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    card_data->user_primary.balance = new_balance;
    card_data->user_backup.balance = new_balance;
    
    // Write to card using core functions
    MIFARE_Result_t result = MIFARE_WriteCardData(card_data);
    
    if (result == MIFARE_RESULT_OK) {
        LOG_CRITICAL_VOLUME("[✓] Volume balance written: %lu ml\r\n", new_balance);
    } else {
        LOG_CRITICAL_VOLUME("[✗] Volume balance write failed: %s\r\n", MIFARE_GetResultString(result));
    }
    
    return result;
}

/**
 * @brief Deduct ml from balance
 */
static MIFARE_Result_t volume_deduct_balance(MIFARE_CardData_t *card_data, uint32_t amount)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    if (card_data->user_primary.balance < amount) {
        LOG_CRITICAL_VOLUME("[✗] Insufficient balance: need %lu ml, have %lu ml\r\n", 
                          amount, card_data->user_primary.balance);
        return MIFARE_RESULT_INSUFFICIENT_BALANCE;
    }
    
    uint32_t old_balance = card_data->user_primary.balance;
    uint32_t new_balance = old_balance - amount;

    /* Update per-card daily counter only if the limit is enabled. We bump
     * the in-memory usage_data BEFORE the write so it persists in the same
     * card transaction (no extra block writes). */
    if (g_system_config.mifare.max_daily_volume_ml > 0 && amount > 0) {
        uint16_t today = today_epoch_day_lo();
        uint16_t card_day = 0, used_cl = 0;
        daily_unpack(card_data->usage_data.reserved, &card_day, &used_cl);
        if (card_day != today) {
            /* New day - reset counter */
            card_day = today;
            used_cl  = 0;
        }
        /* Add this session's mL converted to cL. Carry any remainder. */
        uint32_t used_ml = (uint32_t)used_cl * 10u + amount;
        uint32_t new_cl   = used_ml / 10u;
        if (new_cl > 0xFFFFu) new_cl = 0xFFFFu;
        card_data->usage_data.reserved = daily_pack(card_day, (uint16_t)new_cl);
        LOG_DEBUG_VOLUME("[VOLUME] Daily counter -> %lu cL (day %u)\r\n",
                         (unsigned long)new_cl, (unsigned)card_day);
    }
    
    return volume_write_balance(card_data, new_balance);
}

/**
 * @brief Add ml to balance (topup)
 */
static MIFARE_Result_t volume_add_balance(MIFARE_CardData_t *card_data, uint32_t amount)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    uint32_t new_balance = card_data->user_primary.balance + amount;
    card_data->user_primary.last_topup = amount;
    
    LOG_CRITICAL_VOLUME("[→] Adding %lu ml (total: %lu ml)\r\n", amount, new_balance);
    return volume_write_balance(card_data, new_balance);
}

/**
 * @brief Format card with initial ml balance
 */
static MIFARE_Result_t volume_format_card(MIFARE_CardData_t *card_data, uint32_t initial_balance, uint64_t customer_id)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_CRITICAL_VOLUME("[→] Formatting card: %lu ml, customer ID: %llu\r\n", 
                      initial_balance, customer_id);
    
    // Use core's initialization function
    return MIFARE_InitializeNewCustomerCard(initial_balance, customer_id, false);
}

/* Event Callback Implementations --------------------------------------------*/

/**
 * @brief Called when card is first detected
 * @note VOLUME BEHAVIOR: Check if balance is sufficient for dispensing
 */
static MIFARE_Result_t volume_on_card_detected(MIFARE_CardData_t *card_data)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_CRITICAL_VOLUME("[→] Card detected - checking balance\r\n");

    // Publish card detected event
    Event_RFID_Detected_t* evt = (Event_RFID_Detected_t*)EventPool_Alloc(EVT_RFID_CARD_DETECTED, sizeof(Event_RFID_Detected_t));
    if (evt != NULL) {
        PN532_CardInfo_t info;
        if (MIFARE_GetCurrentCardInfo(&info)) {
            memcpy(evt->uid, info.uid, (info.uid_length <= 7) ? info.uid_length : 7);
            evt->uid_len = info.uid_length;
        } else {
            evt->uid_len = 0;
        }
        EventBroker_Publish((Event_t*)evt);
    }
    
    // Check if we have any balance
    if (card_data->user_primary.balance < 1) {
        LOG_CRITICAL_VOLUME("[✗] No balance available (balance: %lu ml)\r\n", 
                          card_data->user_primary.balance);
        return MIFARE_RESULT_INSUFFICIENT_BALANCE;
    }

    /* Per-card daily rate limit (config-gated). 0 = disabled. */
    uint32_t max_ml = g_system_config.mifare.max_daily_volume_ml;
    if (max_ml > 0) {
        uint16_t today = today_epoch_day_lo();
        uint16_t card_day = 0, used_cl = 0;
        daily_unpack(card_data->usage_data.reserved, &card_day, &used_cl);
        if (card_day == today) {
            uint32_t used_ml = (uint32_t)used_cl * 10u;
            if (used_ml >= max_ml) {
                LOG_CRITICAL_VOLUME("[✗] Daily limit reached (%lu/%lu ml today)\r\n",
                                    used_ml, max_ml);
                return MIFARE_RESULT_INSUFFICIENT_BALANCE;
            }
            LOG_DEBUG_VOLUME("[VOLUME] Daily used today: %.2f L (limit %lu mL)\r\n",
                             (float)used_cl / 100.0f, max_ml);
        }
    }

    // Balance deduction is handled by Dispenser_Controller based on actual ml dispensed
    return MIFARE_RESULT_OK;
}

/**
 * @brief Called when card is removed from field
 */
static MIFARE_Result_t volume_on_card_removed(MIFARE_CardData_t *card_data)
{
    (void)card_data;  // Dispenser_Controller handles balance deduction on removal
    
    LOG_DEBUG_VOLUME("[VOLUME] Card removed\r\n");

    // Publish card removed event
    Event_t* evt = EventPool_Alloc(EVT_RFID_CARD_REMOVED, sizeof(Event_t));
    if (evt != NULL) {
        EventBroker_Publish(evt);
    }
    
    return MIFARE_RESULT_OK;
}

/**
 * @brief Called when transaction starts (dispense cycle begins)
 * @note VOLUME BEHAVIOR: No-op - balance updated on card removal
 */
static MIFARE_Result_t volume_on_transaction_start(MIFARE_CardData_t *card_data)
{
    (void)card_data;  // Balance updated when card is removed
    
    LOG_DEBUG_VOLUME("[VOLUME] Transaction started\r\n");
    return MIFARE_RESULT_OK;
}

/**
 * @brief Called when transaction ends (dispense cycle complete)
 * @note VOLUME BEHAVIOR: No-op - balance already updated
 */
static MIFARE_Result_t volume_on_transaction_end(MIFARE_CardData_t *card_data)
{
    (void)card_data;  // Balance already updated
    
    LOG_DEBUG_VOLUME("[VOLUME] Transaction ended\r\n");
    return MIFARE_RESULT_OK;
}

/* Periodic write intervals during active dispense.
 * Primary-only write keeps the live balance safe quickly.
 * Full (primary+backup) write is less frequent to reduce RF bus time.
 * If a queued write fails the card is assumed removed and dispensing fails closed. */
#define VOLUME_PRIMARY_WRITE_MS   250u   /* primary-only write interval         */
#define VOLUME_FULL_WRITE_MS     1000u   /* primary+backup write interval       */
#define VOLUME_ADAPTER_QUEUE_LENGTH 32u
#define VOLUME_ADAPTER_TASK_STACK_WORDS 256

#ifndef VOLUME_ADAPTER_TASK_PRIORITY
#define VOLUME_ADAPTER_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#endif

static TaskHandle_t s_volume_adapter_task_handle = NULL;
static StaticTask_t s_volume_adapter_task_tcb;
static StackType_t s_volume_adapter_task_stack[VOLUME_ADAPTER_TASK_STACK_WORDS];
static QueueHandle_t s_volume_adapter_queue = NULL;
static StaticQueue_t s_volume_adapter_queue_buffer;
static uint8_t s_volume_adapter_queue_storage[VOLUME_ADAPTER_QUEUE_LENGTH * sizeof(Event_t*)];
static uint32_t s_last_dispensed_ml = 0;
static uint32_t s_last_primary_write_tick = 0;
static uint32_t s_last_full_write_tick = 0;
static bool s_write_pending = false;
static bool s_pending_write_full = false;
static uint8_t s_consecutive_write_failures = 0;
static bool s_dispense_active = false;

static void volume_reset_write_trackers(void)
{
    s_last_dispensed_ml = 0;
    s_last_primary_write_tick = 0;
    s_last_full_write_tick = 0;
    s_consecutive_write_failures = 0;
    s_dispense_active = false;
}

static void volume_publish_confirmed_balance(void)
{
    Event_Balance_Updated_t* balance_evt = (Event_Balance_Updated_t*)EventPool_Alloc(EVT_RFID_BALANCE_CONFIRMED,
                                                                                      sizeof(Event_Balance_Updated_t));
    if (balance_evt != NULL) {
        balance_evt->balance = MIFARE_GetBalance();
        balance_evt->last_topup = MIFARE_GetLastTopup();
        EventBroker_Publish((Event_t*)balance_evt);
    }
}

static void volume_publish_card_removed_from_write_failure(MIFARE_AsyncStatus_t status, MIFARE_Result_t result)
{
    LOG_CRITICAL_VOLUME("[Volume Adapter] Card write failed during dispense (status=%d result=%d) - treating as card removed\r\n",
                        status, result);

    MIFARE_Result_t cleanup_result = MIFARE_ForceCardRemoval();
    if (cleanup_result != MIFARE_RESULT_OK) {
        Event_t* removed_evt = EventPool_Alloc(EVT_RFID_CARD_REMOVED, sizeof(Event_t));
        if (removed_evt != NULL) {
            EventBroker_Publish(removed_evt);
        }
    }
}

static void volume_complete_pending_write(MIFARE_AsyncStatus_t status, MIFARE_Result_t result)
{
    bool write_full = s_pending_write_full;
    s_write_pending = false;

    if (status == MIFARE_ASYNC_STATUS_SUCCESS && result == MIFARE_RESULT_OK) {
        uint32_t now = xTaskGetTickCount();
        s_last_primary_write_tick = now;
        if (write_full) {
            s_last_full_write_tick = now;
        }
        s_consecutive_write_failures = 0;
        volume_publish_confirmed_balance();
        LOG_DEBUG_VOLUME("[Volume Adapter] Write OK (%s)\r\n",
                         write_full ? "full" : "fast");
    } else if (result == MIFARE_RESULT_CARD_REMOVED) {
        /* Authoritative removal from the MIFARE core (state/RF reports the card is
         * gone). Fail closed immediately. */
        LOG_CRITICAL_VOLUME("[Volume Adapter] Write reported CARD_REMOVED - failing closed\r\n");
        volume_publish_card_removed_from_write_failure(status, result);
        volume_reset_write_trackers();
    } else {
        /* Transient write error (BUSY / TIMEOUT / WRITE_FAILED) while the card is
         * still present - e.g. a PN532 recovery window or momentary RF noise. Do
         * NOT treat this as a removal: the periodic timer simply retries, and the
         * MIFARE polling task independently detects genuine removal via hardware
         * presence checks (publishing EVT_RFID_CARD_REMOVED / returning
         * MIFARE_RESULT_CARD_REMOVED). Counting transient errors here previously
         * caused false removals governed by mifare.card_removal_fail_count. */
        if (s_consecutive_write_failures < UINT8_MAX) {
            s_consecutive_write_failures++;
        }
        LOG_CRITICAL_VOLUME("[Volume Adapter] Write transient error (status=%d result=%d retry#%u) - card assumed present\r\n",
                            status, result, s_consecutive_write_failures);
    }
}

static void volume_poll_pending_write(void)
{
    if (!s_write_pending) {
        return;
    }

    MIFARE_Result_t result = MIFARE_RESULT_BUSY;
    MIFARE_AsyncStatus_t status = MIFARE_GetAsyncStatus(&result);
    if (status == MIFARE_ASYNC_STATUS_PENDING) {
        return;
    }

    volume_complete_pending_write(status, result);
}

static void volume_request_card_write(bool write_full)
{
    if (s_write_pending) {
        return;
    }

    bool is_fast = !write_full;
    LOG_DEBUG_VOLUME("[Volume Adapter] Periodic write: %s\r\n",
                     write_full ? "primary+backup" : "primary-only");

    if (MIFARE_RequestUpdateCardAsync(is_fast, 2500)) {
        s_write_pending = true;
        s_pending_write_full = write_full;
    } else {
        LOG_CRITICAL_VOLUME("[Volume Adapter] Write skipped - MIFARE mailbox busy\r\n");
    }
}

/* Service time-based card writes during an active dispense.
 * Runs on a wall-clock timer independently of progress events, so slow or
 * paused flow still refreshes the card (and acts as a fail-closed card
 * presence check). No-op until at least 1 ml has been dispensed. */
static void volume_service_periodic_write(void)
{
    if (s_write_pending || s_last_dispensed_ml == 0) {
        return;
    }

    uint32_t now = xTaskGetTickCount();
    bool do_write   = false;
    bool write_full = false;  /* false = primary only, true = primary+backup */

    if (pdTICKS_TO_MS(now - s_last_full_write_tick) >= VOLUME_FULL_WRITE_MS) {
        do_write   = true;
        write_full = true;   /* time for a full primary+backup write */
    } else if (pdTICKS_TO_MS(now - s_last_primary_write_tick) >= VOLUME_PRIMARY_WRITE_MS) {
        do_write   = true;
        write_full = false;  /* quick primary-only write */
    }

    if (do_write) {
        volume_request_card_write(write_full);
    }
}

static void volume_adapter_task_loop(void* param)
{
    (void)param;
    
    LOG_CRITICAL_VOLUME("[Volume Adapter] Event task started\r\n");
    
    while (1) {
        Event_t* evt = NULL;
        volume_poll_pending_write();
        volume_service_periodic_write();

        TickType_t wait_ticks = (s_write_pending || s_dispense_active) ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        if (xQueueReceive(s_volume_adapter_queue, &evt, wait_ticks) == pdTRUE) {
            if (evt == NULL) continue;

            volume_poll_pending_write();
            
            switch (evt->header.id) {
                case EVT_OPERATION_START: {
                    Event_Operation_Start_t* start = (Event_Operation_Start_t*)evt;
                    if (start->operation_kind == EVENT_OPERATION_KIND_DISPENSE &&
                        start->operation_mode == EVENT_OPERATION_MODE_AUTO) {
                        volume_reset_write_trackers();
                        s_dispense_active = true;
                    }
                    break;
                }

                case EVT_OPERATION_PROGRESS: {
                    Event_Operation_Progress_t* progress = (Event_Operation_Progress_t*)evt;
                    if (progress->operation_kind == EVENT_OPERATION_KIND_DISPENSE &&
                        progress->operation_mode == EVENT_OPERATION_MODE_AUTO) {
                        
                        /* ── 1. Update in-memory balance ───────────────────── */
                        uint32_t current_dispensed = progress->amount_ml;
                        if (current_dispensed > s_last_dispensed_ml) {
                            uint32_t delta = current_dispensed - s_last_dispensed_ml;
                            MIFARE_UserData_t* user_data = MIFARE_GetUserData();
                            if (user_data != NULL) {
                                MIFARE_UserData_t updated = *user_data;
                                if (updated.balance >= delta) {
                                    updated.balance -= delta;
                                } else {
                                    updated.balance = 0;
                                }
                                MIFARE_SetUserData(&updated);
                            }
                            s_last_dispensed_ml = current_dispensed;
                        }
                        
                        /* ── 2. Card writes are driven on a wall-clock timer in
                         *       the task loop (volume_service_periodic_write), so
                         *       they continue even when flow is slow and no further
                         *       progress events arrive. Service once here so a fresh
                         *       balance is written promptly after a deduction. */
                        volume_service_periodic_write();
                    }
                    break;
                }
                
                case EVT_OPERATION_STOP: {
                    Event_Operation_Stop_t* stop = (Event_Operation_Stop_t*)evt;
                    if (stop->operation_kind == EVENT_OPERATION_KIND_DISPENSE &&
                        stop->operation_mode == EVENT_OPERATION_MODE_AUTO) {
                        
                        uint32_t current_dispensed = stop->amount_ml;
                        if (current_dispensed > s_last_dispensed_ml) {
                            uint32_t delta = current_dispensed - s_last_dispensed_ml;
                            MIFARE_UserData_t* user_data = MIFARE_GetUserData();
                            if (user_data != NULL) {
                                MIFARE_UserData_t updated = *user_data;
                                if (updated.balance >= delta) {
                                    updated.balance -= delta;
                                } else {
                                    updated.balance = 0;
                                }
                                MIFARE_SetUserData(&updated);
                            }
                        }
                        
                        LOG_CRITICAL_VOLUME("[Volume Adapter] Dispense stopped: triggering final full write\r\n");
                        volume_request_card_write(true);
                        
                        if (stop->stop_reason == EVENT_OPERATION_STOP_REASON_NO_FLOW) {
                            MIFARE_SetTransactionState(TRANSACTION_STATE_ERROR_NO_FLOW);
                            LOG_CRITICAL_VOLUME("[Volume Adapter] Set transaction state to ERROR_NO_FLOW\r\n");
                        }
                        
                        volume_reset_write_trackers();
                    }
                    break;
                }
                
                case EVT_RFID_TOPUP_REQUEST: {
                    Event_RFID_TopupRequest_t* req = (Event_RFID_TopupRequest_t*)evt;
                    LOG_CRITICAL_VOLUME("[Volume Adapter] Processing topup request of %lu ml\r\n", req->amount);
                    
                    MIFARE_Result_t res = MIFARE_RESULT_BUSY;
                    if (MIFARE_RequestTopupAsync(req->amount, 5000)) {
                        MIFARE_AsyncStatus_t status = MIFARE_WaitForAsyncResult(&res, 5000);
                        if (status == MIFARE_ASYNC_STATUS_SUCCESS && res == MIFARE_RESULT_OK) {
                            Event_t* success = EventPool_Alloc(EVT_RFID_TRANSACTION_SUCCESS, sizeof(Event_t));
                            if (success != NULL) EventBroker_Publish(success);
                        } else {
                            Event_t* fail = EventPool_Alloc(EVT_RFID_TRANSACTION_FAILED, sizeof(Event_t));
                            if (fail != NULL) EventBroker_Publish(fail);
                        }
                    } else {
                        Event_t* fail = EventPool_Alloc(EVT_RFID_TRANSACTION_FAILED, sizeof(Event_t));
                        if (fail != NULL) EventBroker_Publish(fail);
                    }
                    break;
                }
                
                case EVT_RFID_INIT_CUSTOMER_REQUEST: {
                    Event_RFID_InitCustomerRequest_t* req = (Event_RFID_InitCustomerRequest_t*)evt;
                    LOG_CRITICAL_VOLUME("[Volume Adapter] Processing init customer request (balance=%lu, customer_id=%llu)\r\n", 
                                   req->initial_balance, req->customer_id);
                                   
                    MIFARE_Result_t res = MIFARE_RESULT_BUSY;
                    if (MIFARE_RequestInitCardAsync(req->initial_balance, req->customer_id, 10000)) {
                        MIFARE_AsyncStatus_t status = MIFARE_WaitForAsyncResult(&res, 10000);
                        if (status == MIFARE_ASYNC_STATUS_SUCCESS && res == MIFARE_RESULT_OK) {
                            Event_t* success = EventPool_Alloc(EVT_RFID_TRANSACTION_SUCCESS, sizeof(Event_t));
                            if (success != NULL) EventBroker_Publish(success);
                        } else {
                            Event_t* fail = EventPool_Alloc(EVT_RFID_TRANSACTION_FAILED, sizeof(Event_t));
                            if (fail != NULL) EventBroker_Publish(fail);
                        }
                    } else {
                        Event_t* fail = EventPool_Alloc(EVT_RFID_TRANSACTION_FAILED, sizeof(Event_t));
                        if (fail != NULL) EventBroker_Publish(fail);
                    }
                    break;
                }
                
                default:
                    break;
            }
            Event_Release(evt);
        }
    }
}

/* Public API ----------------------------------------------------------------*/

/**
 * @brief Initialize Volume adapter and register with core
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_Volume_Adapter_Init(void)
{
    LOG_CRITICAL_VOLUME("[→] Initializing Volume Adapter (MyWota)\r\n");
    
    s_volume_adapter_queue = xQueueCreateStatic(VOLUME_ADAPTER_QUEUE_LENGTH, sizeof(Event_t*), s_volume_adapter_queue_storage, &s_volume_adapter_queue_buffer);
    if (s_volume_adapter_queue != NULL) {
        EventBroker_Subscribe(s_volume_adapter_queue, EVT_OPERATION_START);
        EventBroker_Subscribe(s_volume_adapter_queue, EVT_OPERATION_PROGRESS);
        EventBroker_Subscribe(s_volume_adapter_queue, EVT_OPERATION_STOP);
        EventBroker_Subscribe(s_volume_adapter_queue, EVT_RFID_TOPUP_REQUEST);
        EventBroker_Subscribe(s_volume_adapter_queue, EVT_RFID_INIT_CUSTOMER_REQUEST);
        
        s_volume_adapter_task_handle = xTaskCreateStatic(
            volume_adapter_task_loop,
            "VolAdapter",
            VOLUME_ADAPTER_TASK_STACK_WORDS,
            NULL,
            VOLUME_ADAPTER_TASK_PRIORITY,
            s_volume_adapter_task_stack,
            &s_volume_adapter_task_tcb
        );
    }
    
    // Initialize core with volume interface
    MIFARE_Result_t result = MIFARE_TransactionManager_Init(&volume_interface, &g_system_config.mifare);
    
    if (result == MIFARE_RESULT_OK) {
        LOG_CRITICAL_VOLUME("[✓] Volume Adapter initialized successfully\r\n");
    } else {
        LOG_CRITICAL_VOLUME("[✗] Volume Adapter initialization failed: %s\r\n", 
                          MIFARE_GetResultString(result));
    }
    
    return result;
}
