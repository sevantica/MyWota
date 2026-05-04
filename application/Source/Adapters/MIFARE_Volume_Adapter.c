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
#include <string.h>

/* Daily-limit packing helpers ----------------------------------------------*/
/* card_data->usage_data.reserved layout:
 *   bits 31..16 : epoch_day_lo (UTC days since 1970-01-01, mod 65536)
 *   bits 15..0  : daily_used_l (liters dispensed today, capped 65535)
 */
static inline uint16_t today_epoch_day_lo(void) {
    time_t now = RTC_GetUnixTime();
    uint32_t days = (uint32_t)(now / 86400);
    return (uint16_t)(days & 0xFFFFu);
}

static inline void daily_unpack(uint32_t raw, uint16_t *day, uint16_t *used_l) {
    if (day)    *day    = (uint16_t)((raw >> 16) & 0xFFFFu);
    if (used_l) *used_l = (uint16_t)(raw & 0xFFFFu);
}

static inline uint32_t daily_pack(uint16_t day, uint16_t used_l) {
    return ((uint32_t)day << 16) | (uint32_t)used_l;
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
    .continuous_updates = false,        // No continuous updates during dispense
    .fast_update_ms = 0,                // Not used for volume system
    
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
        uint16_t card_day = 0, used_l = 0;
        daily_unpack(card_data->usage_data.reserved, &card_day, &used_l);
        if (card_day != today) {
            /* New day - reset counter */
            card_day = today;
            used_l   = 0;
        }
        /* Add this session's mL, rounded up to whole liters with carry. */
        uint32_t used_ml = (uint32_t)used_l * 1000u + amount;
        uint32_t new_l   = used_ml / 1000u;
        if (new_l > 0xFFFFu) new_l = 0xFFFFu;
        card_data->usage_data.reserved = daily_pack(card_day, (uint16_t)new_l);
        LOG_DEBUG_VOLUME("[VOLUME] Daily counter -> %lu L (day %u)\r\n",
                         (unsigned long)new_l, (unsigned)card_day);
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
        uint16_t card_day = 0, used_l = 0;
        daily_unpack(card_data->usage_data.reserved, &card_day, &used_l);
        if (card_day == today) {
            uint32_t used_ml = (uint32_t)used_l * 1000u;
            if (used_ml >= max_ml) {
                LOG_CRITICAL_VOLUME("[✗] Daily limit reached (%lu/%lu ml today)\r\n",
                                    used_ml, max_ml);
                return MIFARE_RESULT_INSUFFICIENT_BALANCE;
            }
            LOG_DEBUG_VOLUME("[VOLUME] Daily used today: %u L (limit %lu mL)\r\n",
                             used_l, max_ml);
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

/* Public API ----------------------------------------------------------------*/

/**
 * @brief Initialize Volume adapter and register with core
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_Volume_Adapter_Init(void)
{
    LOG_CRITICAL_VOLUME("[→] Initializing Volume Adapter (MyWota)\r\n");
    
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
