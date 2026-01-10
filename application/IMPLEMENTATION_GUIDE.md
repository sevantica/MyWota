# Implementation Guide: Converting to Car Wash Token System

This guide shows the key implementation changes needed to convert the codebase from liquid dispensing to car wash tokens, following the polling architecture.

---

## Critical Find-and-Replace Operations

### 1. MIFARE_Transaction_Manager.c (Generic Card I/O)

#### State Machine Updates
```
Find: MIFARE_DispenseState_t
Replace: MIFARE_TransactionState_t

Find: dispense_state
Replace: transaction_state

Find: DISPENSE_STATE_IDLE
Replace: TRANSACTION_STATE_IDLE

Find: DISPENSE_STATE_CARD_DETECTED
Replace: TRANSACTION_STATE_CARD_DETECTED

Find: DISPENSE_STATE_AUTHENTICATING
Replace: TRANSACTION_STATE_AUTHENTICATING

Find: DISPENSE_STATE_READING_DATA
Replace: TRANSACTION_STATE_READING_DATA

Find: DISPENSE_STATE_VALIDATING
Replace: TRANSACTION_STATE_VALIDATING

Find: DISPENSE_STATE_READY_TO_DISPENSE
Replace: TRANSACTION_STATE_READY

Find: DISPENSE_STATE_DISPENSING
Replace: TRANSACTION_STATE_WRITING_DATA

Find: DISPENSE_STATE_UPDATING_CARD
Replace: TRANSACTION_STATE_WRITING_DATA

Find: DISPENSE_STATE_FINALIZING
Replace: TRANSACTION_STATE_FINALIZING

Find: DISPENSE_STATE_ERROR
Replace: TRANSACTION_STATE_ERROR

Find: DISPENSE_STATE_CARD_REMOVED
Replace: TRANSACTION_STATE_CARD_REMOVED

Find: DISPENSE_STATE_CARD_REMOVED_DURING_DISPENSING
Replace: TRANSACTION_STATE_CARD_REMOVED
```

#### Data Structure Updates
```
Find: balance_ml
Replace: token_count

Find: last_topup_amount_ml
Replace: last_topup_tokens

Find: total_purchased_ml
Replace: total_tokens_purchased

Find: total_dispensed_ml
Replace: total_washes_completed

Find: MIFARE_FastBalance_t
Replace: MIFARE_TokenCache_t

Find: fast_balance
Replace: token_cache

Find: mifare_fast_balance_
Replace: mifare_token_cache_

Find: MIFARE_BLOCK_FAST_BALANCE_PRIMARY
Replace: MIFARE_BLOCK_TOKEN_CACHE_PRIMARY

Find: MIFARE_BLOCK_FAST_BALANCE_BACKUP
Replace: MIFARE_BLOCK_TOKEN_CACHE_BACKUP
```

#### Function Renames
```
Find: MIFARE_GetStateString(MIFARE_DispenseState_t
Replace: MIFARE_GetTransactionStateString(MIFARE_TransactionState_t

Find: MIFARE_GetDispenseState(
Replace: MIFARE_GetTransactionState(

Find: MIFARE_GetBalanceML(
Replace with getter that returns pointer: MIFARE_GetUserData()->token_count

Find: MIFARE_TopupCardBalance(
Replace: MIFARE_TopupCardTokens(

Find: MIFARE_BeginTransaction(uint32_t amount_ml)
Replace: MIFARE_BeginTransaction(void)

Find: MIFARE_UpdateTransactionProgress(uint32_t dispensed_ml, float flow_rate_lpm)
Replace: MIFARE_UpdateCardData(void)
```

#### Remove Business Logic References
- Delete: `total_dispensed_this_session` field
- Delete: `dispense_start_time` field
- Delete: `last_fast_balance_update_time` field
- Add: `last_token_cache_update_time` field (if needed for optimization)

---

### 2. New Getter/Setter Functions (Add to MIFARE_Transaction_Manager.c)

```c
/**
 * @brief Get pointer to user data (token count, status, etc.)
 * @return Pointer to user data structure (read-only for caller)
 */
MIFARE_UserData_t* MIFARE_GetUserData(void)
{
    if (!MIFARE_IsCardReady()) {
        return NULL;
    }
    return &g_transaction_manager.current_card.user_primary;
}

/**
 * @brief Get pointer to usage data (lifetime statistics)
 * @return Pointer to usage data structure (read-only for caller)
 */
MIFARE_UsageData_t* MIFARE_GetUsageData(void)
{
    if (!MIFARE_IsCardReady()) {
        return NULL;
    }
    return &g_transaction_manager.current_card.usage_data;
}

/**
 * @brief Get pointer to account data (phone, validity)
 * @return Pointer to account data structure (read-only for caller)
 */
MIFARE_AccountData_t* MIFARE_GetAccountData(void)
{
    if (!MIFARE_IsCardReady()) {
        return NULL;
    }
    return &g_transaction_manager.current_card.account_data;
}

/**
 * @brief Check if card is ready for operations
 * @return true if card present and data valid
 */
bool MIFARE_IsCardReady(void)
{
    return (g_transaction_manager.card_state == MIFARE_CARD_STATE_PRESENT ||
            g_transaction_manager.card_state == MIFARE_CARD_STATE_READY) &&
           g_transaction_manager.current_card.data_valid &&
           (g_transaction_manager.transaction_state == TRANSACTION_STATE_READY);
}

/**
 * @brief Set user data (business logic modifies via this)
 * @param user_data Pointer to new user data
 */
void MIFARE_SetUserData(const MIFARE_UserData_t *user_data)
{
    if (!user_data) return;
    
    // Copy to in-memory structure
    memcpy(&g_transaction_manager.current_card.user_primary, user_data, sizeof(MIFARE_UserData_t));
    memcpy(&g_transaction_manager.current_card.user_backup, user_data, sizeof(MIFARE_UserData_t));
    
    // Update token cache
    mifare_update_token_cache(&g_transaction_manager.current_card.token_cache_primary, 
                             user_data->token_count);
    memcpy(&g_transaction_manager.current_card.token_cache_backup,
           &g_transaction_manager.current_card.token_cache_primary,
           sizeof(MIFARE_TokenCache_t));
}

/**
 * @brief Set usage data (business logic modifies via this)
 * @param usage_data Pointer to new usage data
 */
void MIFARE_SetUsageData(const MIFARE_UsageData_t *usage_data)
{
    if (!usage_data) return;
    memcpy(&g_transaction_manager.current_card.usage_data, usage_data, sizeof(MIFARE_UsageData_t));
}

/**
 * @brief Get current transaction state
 * @return Current transaction state
 */
MIFARE_TransactionState_t MIFARE_GetTransactionState(void)
{
    return g_transaction_manager.transaction_state;
}

/**
 * @brief Update card data - write in-memory data to physical card
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_UpdateCardData(void)
{
    // This function writes the in-memory card data structures to the physical card
    // Business logic calls this after modifying data via setters
    
    if (!MIFARE_IsCardPresent()) {
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    MIFARE_Result_t result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
    return result;
}
```

---

### 3. MIFARE_CarWash_Integration.c (New Business Logic File)

This is a NEW file that contains all the car wash business logic.

```c
/*
 * @attention
 * Copyright (c) Sevantica 2025.
 * All rights reserved.
 */

/**
 * @file MIFARE_CarWash_Integration.c
 * @brief Car wash business logic layer - manages tokens and wash timers
 * @details This layer polls MIFARE_Transaction_Manager for card data,
 *          makes business decisions, and updates card data accordingly.
 */

#include "MIFARE_Dispenser_Integration.h"
#include "MIFARE_Transaction_Manager.h"
#include "USB_Logging.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/*Private defines ---------------------------------------------------*/
#define LOG_DEBUG_CARWASH_EN      1

#if LOG_DEBUG_CARWASH_EN
    #define CARWASH_LOG(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define CARWASH_LOG(...)
#endif

/*Private variables -------------------------------------------------*/
static CarWash_TimerState_t g_wash_timer = {0};
static CarWashState_t g_carwash_state = CARWASH_IDLE;
static TaskHandle_t carwash_task_handle = NULL;

/*Private function prototypes ---------------------------------------*/
static void carwash_update_timer(void);
static bool carwash_has_tokens(void);

/**
 * @brief Initialize car wash system
 */
CarWashResult_t MIFARE_CarWash_Init(void)
{
    // Initialize wash timer state
    memset(&g_wash_timer, 0, sizeof(CarWash_TimerState_t));
    g_wash_timer.wash_duration_seconds = WASH_DURATION_SECONDS;
    g_carwash_state = CARWASH_IDLE;
    
    CARWASH_LOG("CarWash: Initialized\\r\\n");
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
    return (user_data->token_count > 0);
}

/**
 * @brief Start a car wash (deduct token, start timer)
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_StartWash(void)
{
    // Check if card is ready
    if (!MIFARE_IsCardReady()) {
        CARWASH_LOG("CarWash: Card not ready\\r\\n");
        return CARWASH_RESULT_CARD_NOT_READY;
    }
    
    // Check if wash already active
    if (g_wash_timer.wash_active) {
        CARWASH_LOG("CarWash: Wash already in progress\\r\\n");
        return CARWASH_RESULT_BUSY;
    }
    
    // Get user data
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    if (!user_data) {
        return CARWASH_RESULT_ERROR;
    }
    
    // Check tokens
    if (user_data->token_count == 0) {
        CARWASH_LOG("CarWash: Insufficient tokens\\r\\n");
        return CARWASH_RESULT_INSUFFICIENT_TOKENS;
    }
    
    // Deduct 1 token
    MIFARE_UserData_t updated_user_data;
    memcpy(&updated_user_data, user_data, sizeof(MIFARE_UserData_t));
    updated_user_data.token_count--;
    updated_user_data.transaction_counter++;
    
    // Update usage statistics
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    if (usage_data) {
        MIFARE_UsageData_t updated_usage;
        memcpy(&updated_usage, usage_data, sizeof(MIFARE_UsageData_t));
        updated_usage.total_washes_completed++;
        updated_usage.total_wash_time_minutes += (WASH_DURATION_SECONDS / 60);
        
        MIFARE_SetUsageData(&updated_usage);
    }
    
    // Write updated data to card
    MIFARE_SetUserData(&updated_user_data);
    
    MIFARE_Result_t result = MIFARE_BeginTransaction();
    if (result != MIFARE_RESULT_OK) {
        CARWASH_LOG("CarWash: Failed to begin transaction\\r\\n");
        return CARWASH_RESULT_ERROR;
    }
    
    result = MIFARE_UpdateCardData();
    if (result != MIFARE_RESULT_OK) {
        MIFARE_RollbackTransaction();
        CARWASH_LOG("CarWash: Failed to write card data\\r\\n");
        return CARWASH_RESULT_ERROR;
    }
    
    result = MIFARE_CommitTransaction();
    if (result != MIFARE_RESULT_OK) {
        CARWASH_LOG("CarWash: Failed to commit transaction\\r\\n");
        return CARWASH_RESULT_ERROR;
    }
    
    // Start wash timer
    g_wash_timer.wash_start_time = xTaskGetTickCount();
    g_wash_timer.wash_active = true;
    g_wash_timer.tokens_used_this_session++;
    g_carwash_state = CARWASH_WASH_IN_PROGRESS;
    
    CARWASH_LOG("CarWash: Started - %lu tokens remaining\\r\\n", updated_user_data.token_count);
    
    return CARWASH_RESULT_OK;
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
    status->token_count = user_data ? user_data->token_count : 0;
    
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
    
    CARWASH_LOG("CarWash: Emergency stop\\r\\n");
    
    return CARWASH_RESULT_OK;
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
    uint32_t elapsed_ticks = xTaskGetTickCount() - g_wash_timer.wash_start_time;
    uint32_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;
    
    if (elapsed_seconds >= g_wash_timer.wash_duration_seconds) {
        // Timer expired - finish wash
        g_wash_timer.wash_active = false;
        g_carwash_state = CARWASH_IDLE;
        
        CARWASH_LOG("CarWash: Timer expired - wash complete\\r\\n");
    }
}

/**
 * @brief Car wash polling task
 * @param argument Task argument (unused)
 */
void MIFARE_CarWash_Task(void* argument)
{
    (void)argument;
    
    CARWASH_LOG("CarWash: Task started\\r\\n");
    
    while (1) {
        // Update wash timer
        carwash_update_timer();
        
        // Update state machine based on card presence
        if (MIFARE_IsCardReady() && g_carwash_state == CARWASH_IDLE) {
            g_carwash_state = CARWASH_CARD_READY;
        } else if (!MIFARE_IsCardPresent() && g_carwash_state == CARWASH_CARD_READY) {
            g_carwash_state = CARWASH_IDLE;
        }
        
        // Sleep for 100ms
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Start car wash polling task
 */
void Task_Start_CarWash_Task(void)
{
    xTaskCreate(MIFARE_CarWash_Task, 
                "CarWash_Task", 
                512,  // Stack size in words
                NULL, 
                tskIDLE_PRIORITY + 1, 
                &carwash_task_handle);
}

/* UI Getter Functions */

uint32_t CarWash_GetTokenCount(void)
{
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    return user_data ? user_data->token_count : 0;
}

uint32_t CarWash_GetWashTimeRemaining(void)
{
    if (!g_wash_timer.wash_active) {
        return 0;
    }
    
    uint32_t elapsed_ticks = xTaskGetTickCount() - g_wash_timer.wash_start_time;
    uint32_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;
    
    if (elapsed_seconds < g_wash_timer.wash_duration_seconds) {
        return g_wash_timer.wash_duration_seconds - elapsed_seconds;
    }
    return 0;
}

bool CarWash_IsWashActive(void)
{
    return g_wash_timer.wash_active;
}

uint32_t CarWash_GetTotalWashesCompleted(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_washes_completed : 0;
}

uint32_t CarWash_GetTotalTokensPurchased(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_tokens_purchased : 0;
}

/**
 * @brief Initialize new customer card with tokens
 * @param initial_tokens Number of tokens to add
 * @param customer_id Customer ID (0 = auto-generate)
 * @return CarWashResult_t Operation result
 */
CarWashResult_t MIFARE_CarWash_InitializeNewCustomer(uint32_t initial_tokens, uint64_t customer_id)
{
    MIFARE_Result_t result = MIFARE_InitializeNewCustomerCard(initial_tokens, customer_id);
    
    if (result == MIFARE_RESULT_OK) {
        return CARWASH_RESULT_OK;
    }
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
        return CARWASH_RESULT_CARD_NOT_READY;
    }
    
    MIFARE_Result_t result = MIFARE_TopupCardTokens(topup_tokens);
    
    if (result == MIFARE_RESULT_OK) {
        return CARWASH_RESULT_OK;
    }
    return CARWASH_RESULT_ERROR;
}
```

---

## Summary

The implementation involves:

1. **Find-and-replace operations** in MIFARE_Transaction_Manager.c to genericize the state machine
2. **Add getter/setter functions** to Transaction Manager for data access
3. **Create new MIFARE_CarWash_Integration.c** file with all business logic
4. **Update System.c** to start the CarWash task instead of any dispenser task

The key principle: **Transaction Manager = Card I/O only, CarWash Integration = Business logic**
