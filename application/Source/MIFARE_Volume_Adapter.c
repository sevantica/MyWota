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
 * @file MIFARE_Token_Adapter.c
 * @brief Token-based adapter for BigYellow car wash system
 * @details Implements one-time token deduction on card detection
 *          - No continuous updates during transaction
 *          - Deduct 1 token immediately when card detected
 *          - User removes card, then wash cycle starts
 */

/* Includes ------------------------------------------------------------------*/
#include "MIFARE_Transaction_Core.h"
#include "MIFARE_Card_Interface.h"
#include "USB_Logging.h"
#include <string.h>

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_TOKEN_ADAPTER_EN      0
#define LOG_CRITICAL_TOKEN_ADAPTER_EN   1

#if LOG_DEBUG_TOKEN_ADAPTER_EN
    #define LOG_DEBUG_TOKEN(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_TOKEN(...)
#endif

#if LOG_CRITICAL_TOKEN_ADAPTER_EN
    #define LOG_CRITICAL_TOKEN(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_TOKEN(...)
#endif

/* Private function prototypes -----------------------------------------------*/
static MIFARE_Result_t token_read_balance(MIFARE_CardData_t *card_data, uint32_t *balance_out);
static MIFARE_Result_t token_write_balance(MIFARE_CardData_t *card_data, uint32_t new_balance);
static MIFARE_Result_t token_deduct_balance(MIFARE_CardData_t *card_data, uint32_t amount);
static MIFARE_Result_t token_add_balance(MIFARE_CardData_t *card_data, uint32_t amount);
static MIFARE_Result_t token_format_card(MIFARE_CardData_t *card_data, uint32_t initial_balance, uint64_t customer_id);
static MIFARE_Result_t token_on_card_detected(MIFARE_CardData_t *card_data);
static MIFARE_Result_t token_on_card_removed(MIFARE_CardData_t *card_data);
static MIFARE_Result_t token_on_transaction_start(MIFARE_CardData_t *card_data);
static MIFARE_Result_t token_on_transaction_end(MIFARE_CardData_t *card_data);

/* Token Interface Definition ------------------------------------------------*/
static const MIFARE_CardInterface_t token_interface = {
    .name = "Token",
    .balance_unit = "tokens",
    .continuous_updates = false,        // No continuous updates - one-time deduction
    .fast_update_ms = 0,                // Not used for token system
    
    // Virtual functions
    .read_balance = token_read_balance,
    .write_balance = token_write_balance,
    .deduct_balance = token_deduct_balance,
    .add_balance = token_add_balance,
    .format_card = token_format_card,
    
    // Event callbacks
    .on_card_detected = token_on_card_detected,
    .on_card_removed = token_on_card_removed,
    .on_transaction_start = token_on_transaction_start,
    .on_transaction_end = token_on_transaction_end
};

/* Virtual Function Implementations ------------------------------------------*/

/**
 * @brief Read token balance from card
 */
static MIFARE_Result_t token_read_balance(MIFARE_CardData_t *card_data, uint32_t *balance_out)
{
    if (!card_data || !balance_out) {
        return MIFARE_RESULT_ERROR;
    }
    
    *balance_out = card_data->user_primary.balance;
    LOG_DEBUG_TOKEN("[TOKEN] Read balance: %lu tokens\r\n", *balance_out);
    return MIFARE_RESULT_OK;
}

/**
 * @brief Write token balance to card
 */
static MIFARE_Result_t token_write_balance(MIFARE_CardData_t *card_data, uint32_t new_balance)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    card_data->user_primary.balance = new_balance;
    card_data->user_backup.balance = new_balance;
    
    // Write to card using core functions
    MIFARE_Result_t result = MIFARE_WriteCardData(card_data);
    
    if (result == MIFARE_RESULT_OK) {
        LOG_CRITICAL_TOKEN("[✓] Token balance written: %lu tokens\r\n", new_balance);
    } else {
        LOG_CRITICAL_TOKEN("[✗] Token balance write failed: %s\r\n", MIFARE_GetResultString(result));
    }
    
    return result;
}

/**
 * @brief Deduct tokens from balance
 */
static MIFARE_Result_t token_deduct_balance(MIFARE_CardData_t *card_data, uint32_t amount)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    if (card_data->user_primary.balance < amount) {
        LOG_CRITICAL_TOKEN("[✗] Insufficient tokens: need %lu, have %lu\r\n", 
                          amount, card_data->user_primary.balance);
        return MIFARE_RESULT_INSUFFICIENT_BALANCE;
    }
    
    uint32_t old_balance = card_data->user_primary.balance;
    uint32_t new_balance = old_balance - amount;
    
    return token_write_balance(card_data, new_balance);
}

/**
 * @brief Add tokens to balance
 */
static MIFARE_Result_t token_add_balance(MIFARE_CardData_t *card_data, uint32_t amount)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    uint32_t new_balance = card_data->user_primary.balance + amount;
    card_data->user_primary.last_topup = amount;
    
    LOG_CRITICAL_TOKEN("[→] Adding %lu tokens (total: %lu tokens)\r\n", amount, new_balance);
    return token_write_balance(card_data, new_balance);
}

/**
 * @brief Format card with initial token balance
 */
static MIFARE_Result_t token_format_card(MIFARE_CardData_t *card_data, uint32_t initial_balance, uint64_t customer_id)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_CRITICAL_TOKEN("[→] Formatting card: %lu tokens, customer ID: %llu\r\n", 
                      initial_balance, customer_id);
    
    // Use core's initialization function
    return MIFARE_InitializeNewCustomerCard(initial_balance, customer_id, false);
}

/* Event Callback Implementations --------------------------------------------*/

/**
 * @brief Called when card is first detected
 * @note TOKEN BEHAVIOR: Immediately deduct 1 token and write back
 */
static MIFARE_Result_t token_on_card_detected(MIFARE_CardData_t *card_data)
{
    if (!card_data) {
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_CRITICAL_TOKEN("[→] Card detected - checking token balance\r\n");
    
    // Check if we have at least 1 token
    if (card_data->user_primary.balance < 1) {
        LOG_CRITICAL_TOKEN("[✗] No tokens available (balance: %lu)\r\n", 
                          card_data->user_primary.balance);
        return MIFARE_RESULT_INSUFFICIENT_BALANCE;
    }
    
    // Deduct 1 token immediately
    // DISABLE: Logic moved to Car_Wash_Controller.c to prevent double deduction
    /*
    LOG_CRITICAL_TOKEN("[→] Deducting 1 token (current: %lu)\r\n", 
                      card_data->user_primary.balance);
    
    MIFARE_Result_t result = token_deduct_balance(card_data, 1);
    
    if (result == MIFARE_RESULT_OK) {
        LOG_CRITICAL_TOKEN("[✓] Token deducted - wash authorized (remaining: %lu tokens)\r\n",
                          card_data->user_primary.balance);
        
        // Update usage statistics
        card_data->usage_data.total_dispenses_completed++;
    } else {
        LOG_CRITICAL_TOKEN("[✗] Token deduction failed: %s\r\n", 
                          MIFARE_GetResultString(result));
    }
    
    return result;
    */
    return MIFARE_RESULT_OK;
}

/**
 * @brief Called when card is removed from field
 */
static MIFARE_Result_t token_on_card_removed(MIFARE_CardData_t *card_data)
{
    (void)card_data;  // No action needed - token already deducted
    
    LOG_DEBUG_TOKEN("[TOKEN] Card removed\r\n");
    return MIFARE_RESULT_OK;
}

/**
 * @brief Called when transaction starts (wash cycle begins)
 * @note TOKEN BEHAVIOR: No-op - token already deducted on card detection
 */
static MIFARE_Result_t token_on_transaction_start(MIFARE_CardData_t *card_data)
{
    (void)card_data;  // Token already deducted - nothing to do
    
    LOG_DEBUG_TOKEN("[TOKEN] Transaction started (token already deducted)\r\n");
    return MIFARE_RESULT_OK;
}

/**
 * @brief Called when transaction ends (wash cycle complete)
 * @note TOKEN BEHAVIOR: No-op - no final write needed
 */
static MIFARE_Result_t token_on_transaction_end(MIFARE_CardData_t *card_data)
{
    (void)card_data;  // No final write needed
    
    LOG_DEBUG_TOKEN("[TOKEN] Transaction ended\r\n");
    return MIFARE_RESULT_OK;
}

/* Public API ----------------------------------------------------------------*/

/**
 * @brief Initialize Token adapter and register with core
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_Volume_Adapter_Init(void)
{
    LOG_CRITICAL_TOKEN("[→] Initializing Token Adapter (BigYellow)\r\n");
    
    // Initialize core with token interface
    MIFARE_Result_t result = MIFARE_TransactionManager_Init(&token_interface);
    
    if (result == MIFARE_RESULT_OK) {
        LOG_CRITICAL_TOKEN("[✓] Token Adapter initialized successfully\r\n");
    } else {
        LOG_CRITICAL_TOKEN("[✗] Token Adapter initialization failed: %s\r\n", 
                          MIFARE_GetResultString(result));
    }
    
    return result;
}
