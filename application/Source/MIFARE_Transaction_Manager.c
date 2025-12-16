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
 * @file MIFARE_Transaction_Manager.c
 * @brief Atomic transaction manager for MIFARE cards with corruption protection
 * @details Implements safe card operations during water dispensing with rollback
 *          capabilities and protection against card removal during transactions
 */

/*Includes ----------------------------------------------------------*/
#include "MIFARE_Transaction_Manager.h"
#include "PN532_Driver.h"
#include "USB_Logging.h"
#include "System.h"
#include "mywota_ui_driver.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "ui.h"
#include "ui_Screen1.h"

/*Private defines ---------------------------------------------------*/
#define MIFARE_AUTH_KEY_A               {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}  // Default key for now
#define PN532_POST_RESET_COOLDOWN_MS    1200

/* MIFARE Classic 1K Sector Structure:
 * - 16 sectors (0-15)
 * - Each sector has 4 blocks (blocks 0-63 total)
 * - Block 3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63 are sector trailers
 * - Sector trailers contain Key A, Access Bits, and Key B - MUST NOT BE WRITTEN BY APPLICATION
 */
#define MIFARE_BLOCKS_PER_SECTOR        4
#define MIFARE_IS_SECTOR_TRAILER(block) (((block) % MIFARE_BLOCKS_PER_SECTOR) == 3)
#define MIFARE_GET_SECTOR(block)        ((block) / MIFARE_BLOCKS_PER_SECTOR)

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_MIFARE_TRANSACTION_MANAGER_EN      1
#define LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER_EN   1
#define LOG_ERROR_MIFARE_TRANSACTION_MANAGER_EN      1

#if LOG_DEBUG_MIFARE_TRANSACTION_MANAGER_EN
    #define LOG_DEBUG_MIFARE_TRANSACTION_MANAGER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_MIFARE_TRANSACTION_MANAGER(...)
#endif

#if LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER_EN
    #define LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER(...)
#endif

#if LOG_ERROR_MIFARE_TRANSACTION_MANAGER_EN
    #define LOG_ERROR_MIFARE_TRANSACTION_MANAGER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_MIFARE_TRANSACTION_MANAGER(...)
#endif

/* Legacy macro for compatibility - maps to DEBUG */
#define MIFARE_LOG(fmt, ...) LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: " fmt "\r\n", ##__VA_ARGS__)

/*Private variables -------------------------------------------------*/
MIFARE_TransactionManager_t g_transaction_manager;
static uint8_t mifare_auth_key[6] = MIFARE_AUTH_KEY_A;
static uint32_t transaction_start_timestamp = 0;
static int8_t last_authenticated_sector = -1;  // Cache last authenticated sector (-1 = none)
static TickType_t pn532_recovery_ready_tick = 0;
static bool last_card_write_changed = false;
static bool no_change_log_reported = false;

/*Private function prototypes ---------------------------------------*/
static bool mifare_is_valid_block(uint8_t block_number, bool is_write);
static MIFARE_Result_t mifare_authenticate_block(uint8_t block_number);
static MIFARE_Result_t mifare_read_block_safe(uint8_t block_number, uint8_t *data);
static MIFARE_Result_t mifare_write_block_safe(uint8_t block_number, uint8_t *data);
static void mifare_transition_state(MIFARE_DispenseState_t new_state);
static uint32_t mifare_get_timestamp(void);

static uint32_t mifare_fast_balance_crc32(const uint8_t *data, size_t length);
static void mifare_fast_balance_update(MIFARE_FastBalance_t *fast_balance, uint32_t balance_ml);
static bool mifare_fast_balance_validate(const MIFARE_FastBalance_t *fast_balance);
static void mifare_fast_balance_sync_to_main(MIFARE_CardData_t *card_data);
#define mifare_calculate_crc32            mifare_fast_balance_crc32
#define mifare_update_fast_balance        mifare_fast_balance_update
#define mifare_validate_fast_balance      mifare_fast_balance_validate
#define mifare_sync_fast_balance_to_main  mifare_fast_balance_sync_to_main
static void mifare_clear_write_snapshot(void);
static void mifare_update_write_snapshot(const MIFARE_CardData_t *card_data);
static bool mifare_card_data_changed(const MIFARE_CardData_t *card_data);
static void mifare_encode_account_data(MIFARE_AccountData_t *account_data, const char *phone_str, MIFARE_CardValidity_t validity);
static bool mifare_is_account_data_empty(const MIFARE_AccountData_t *account_data);

/*Utility Functions ---------------------------------------------*/

/**
 * @brief Get string representation of MIFARE result
 * @param result Result code
 * @return const char* Result string
 */
const char* MIFARE_GetResultString(MIFARE_Result_t result)
{
    switch (result) {
        case MIFARE_RESULT_OK:                      return "OK";
        case MIFARE_RESULT_ERROR:                   return "Error";
        case MIFARE_RESULT_CARD_REMOVED:            return "Card Removed";
        case MIFARE_RESULT_INSUFFICIENT_BALANCE:    return "Insufficient Balance";
        case MIFARE_RESULT_CARD_CORRUPTED:          return "Card Corrupted";
        case MIFARE_RESULT_AUTHENTICATION_FAILED:   return "Authentication Failed";
        case MIFARE_RESULT_WRITE_FAILED:            return "Write Failed";
        case MIFARE_RESULT_DATA_MISMATCH:           return "Data Mismatch";
        case MIFARE_RESULT_TIMEOUT:                 return "Timeout";
        case MIFARE_RESULT_BUSY:                    return "Busy";
        case MIFARE_RESULT_PN532_CORRUPTED:         return "PN532 Corrupted";
        default:                                    return "Unknown";
    }
}

/**
 * @brief Get string representation of dispensing state
 * @param state Dispensing state
 * @return const char* State string
 */
const char* MIFARE_GetStateString(MIFARE_DispenseState_t state)
{
    switch (state) {
        case DISPENSE_STATE_IDLE:               return "Idle";
        case DISPENSE_STATE_CARD_DETECTED:      return "Card Detected";
        case DISPENSE_STATE_AUTHENTICATING:     return "Authenticating";
        case DISPENSE_STATE_READING_DATA:       return "Reading Data";
        case DISPENSE_STATE_VALIDATING:         return "Validating";
        case DISPENSE_STATE_READY_TO_DISPENSE:  return "Ready to Dispense";
        case DISPENSE_STATE_DISPENSING:         return "Dispensing";
        case DISPENSE_STATE_UPDATING_CARD:      return "Updating Card";
        case DISPENSE_STATE_FINALIZING:         return "Finalizing";
        case DISPENSE_STATE_ERROR:              return "Error";
        case DISPENSE_STATE_CARD_REMOVED:       return "Card Removed";
        case DISPENSE_STATE_CARD_REMOVED_DURING_DISPENSING: return "Card Removed During Dispensing";
        default:                                return "Unknown";
    }
}

/*Fast balance helpers --------------------------------------------------*/
static uint32_t mifare_fast_balance_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }

    return ~crc;
}

static bool mifare_fast_balance_validate(const MIFARE_FastBalance_t *fast_balance)
{
    // Calculate CRC32 over first 12 bytes (balance + sequence + reserved + timestamp) ONLY
    // Structure: 4 bytes balance + 2 bytes sequence + 2 bytes reserved + 4 bytes timestamp = 12 bytes
    uint32_t calculated_crc = mifare_fast_balance_crc32((const uint8_t*)fast_balance, 12);
    return (calculated_crc == fast_balance->crc32);
}

static void mifare_fast_balance_update(MIFARE_FastBalance_t *fast_balance, uint32_t balance_ml)
{
    fast_balance->balance_ml = balance_ml;
    fast_balance->sequence_number++;
    fast_balance->timestamp = (uint32_t)xTaskGetTickCount();

    // Calculate CRC32 over first 12 bytes (balance + sequence + reserved + timestamp) ONLY
    // Structure: 4 bytes balance + 2 bytes sequence + 2 bytes reserved + 4 bytes timestamp = 12 bytes
    fast_balance->crc32 = mifare_fast_balance_crc32((uint8_t*)fast_balance, 12);
}

static void mifare_fast_balance_sync_to_main(MIFARE_CardData_t *card_data)
{
    // Validate fast balance integrity
    if (!mifare_fast_balance_validate(&card_data->fast_balance_primary)) {
        MIFARE_LOG("Fast balance primary corrupted, rebuilding from main balance");
        // Rebuild corrupted fast balance from main balance
        mifare_fast_balance_update(&card_data->fast_balance_primary, card_data->user_primary.balance_ml);
        MIFARE_LOG("Fast balance rebuilt with %u mL", card_data->user_primary.balance_ml);
        
        // Write repaired fast balance back to card
        MIFARE_Result_t result = mifare_write_block_safe(MIFARE_BLOCK_FAST_BALANCE_PRIMARY, 
                                                         (uint8_t*)&card_data->fast_balance_primary);
        if (result == MIFARE_RESULT_OK) {
            MIFARE_LOG("Fast balance primary written to card");
            // Also update backup
            memcpy(&card_data->fast_balance_backup, &card_data->fast_balance_primary, 
                   sizeof(MIFARE_FastBalance_t));
            mifare_write_block_safe(MIFARE_BLOCK_FAST_BALANCE_BACKUP, 
                                   (uint8_t*)&card_data->fast_balance_backup);
        } else {
            MIFARE_LOG("WARNING: Failed to write repaired fast balance to card");
        }
        return;
    }

    // Check if fast balance is different from main balance
    // We trust fast balance if it's valid (validated above)
    if (card_data->fast_balance_primary.balance_ml != card_data->user_primary.balance_ml) {
        MIFARE_LOG("Syncing fast balance %u mL (seq %u) to main balance %u mL",
                    card_data->fast_balance_primary.balance_ml,
                    card_data->fast_balance_primary.sequence_number,
                    card_data->user_primary.balance_ml);

        card_data->user_primary.balance_ml = card_data->fast_balance_primary.balance_ml;
        card_data->user_backup.balance_ml = card_data->fast_balance_primary.balance_ml;
        
        // CRITICAL: Update CRCs to match the new balance so validation passes
        // The main data has been modified in memory, so the old CRCs read from the card are no longer valid for this data
        card_data->recovery_info.primary_data_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_primary, sizeof(MIFARE_UserData_t));
        card_data->recovery_info.backup_data_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_backup, sizeof(MIFARE_UserData_t));
    }
}

/**
 * @brief Fully re-initializes the PN532 driver to recover from a bad state.
 * @details This function is a critical recovery mechanism. When communication with the
 *          PN532 is lost or corrupted (e.g., due to rapid card removal), this
 *          function resets the driver, preparing it for fresh operations.
 */
void mifare_recover_and_reinit_pn532(void) {
    MIFARE_LOG("CRITICAL: PN532 seems to be in a bad state. Re-initializing driver.");

    // 1. Notify the system about the impending reset
    MIFARE_NotifyPN532Reset();

    // 2. Perform the re-initialization of the PN532 driver
    // This will reset its internal state machine and communication buffers.
    if (PN532_Init() == PN532_STATUS_OK) {
        MIFARE_LOG("PN532 driver re-initialized successfully.");
    } else {
        MIFARE_LOG("ERROR: PN532 driver re-initialization failed.");
        // If re-initialization fails, we are in a deeper trouble.
        // A system reset might be the only way out, but for now, we log it.
    }

    // 3. Clear any cached authentication state, as it's now invalid.
    last_authenticated_sector = -1;
    
    // 4. CRITICAL: Reset write failure tracking - this was an I2C bus error, NOT card removal
    // If we don't reset this, the next write failure will incorrectly trigger card removal
    g_transaction_manager.write_failure_first_tick = 0;
    g_transaction_manager.consecutive_write_failures = 0;
    MIFARE_LOG("Write failure tracking reset after PN532 recovery");
}

/*Core Transaction Manager Functions --------------------------------*/

/**
 * @brief Initialize the MIFARE transaction manager
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_TransactionManager_Init(void)
{
    // Initialize the transaction manager structure
    memset(&g_transaction_manager, 0, sizeof(MIFARE_TransactionManager_t));

    // Create mutex for transaction safety
    g_transaction_manager.transaction_mutex = xSemaphoreCreateMutex();
    if (g_transaction_manager.transaction_mutex == NULL) {
        MIFARE_LOG("ERROR: Failed to create transaction mutex");
        return MIFARE_RESULT_ERROR;
    }
    
    // Initialize state
    g_transaction_manager.dispense_state = DISPENSE_STATE_IDLE;
    g_transaction_manager.card_state = MIFARE_CARD_STATE_ABSENT;
    g_transaction_manager.transaction_active = false;
    g_transaction_manager.consecutive_errors = 0;
    g_transaction_manager.card_removal_abort = false;
    g_transaction_manager.pn532_recovery_until_tick = 0;
    
    // Initialize stability layer
    g_transaction_manager.last_successful_read_tick = 0;
    g_transaction_manager.last_successful_write_tick = 0;
    g_transaction_manager.card_first_detected_tick = 0;
    g_transaction_manager.card_confirmed_present_tick = 0;
    g_transaction_manager.card_first_lost_tick = 0;
    g_transaction_manager.card_presence_confirmed = false;
    g_transaction_manager.ui_state_card_present = false;
    
    mifare_clear_write_snapshot();
    last_card_write_changed = false;
    no_change_log_reported = false;
    
    MIFARE_LOG("Transaction manager initialized successfully");
    return MIFARE_RESULT_OK;
}

/**
 * @brief Process card detection event
 * @param card_info Pointer to detected card information
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_ProcessCardDetected(PN532_CardInfo_t *card_info)
{
    if (card_info == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Check if we're in cooldown period (prevents rapid re-detection after failures)
    TickType_t now = xTaskGetTickCount();
    if (now < g_transaction_manager.pn532_recovery_until_tick) {
        uint32_t remaining_ms = pdTICKS_TO_MS(g_transaction_manager.pn532_recovery_until_tick - now);
        // Don't spam logs - only log every ~5 seconds
        static TickType_t last_cooldown_log = 0;
        if (now - last_cooldown_log > pdMS_TO_TICKS(5000)) {
            MIFARE_LOG("Card detection deferred - in cooldown period (%lu ms remaining)", remaining_ms);
            last_cooldown_log = now;
        }
        return MIFARE_RESULT_BUSY;
    }
    
    // Take mutex for thread safety
    if (xSemaphoreTake(g_transaction_manager.transaction_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return MIFARE_RESULT_BUSY;
    }
    
    MIFARE_Result_t result = MIFARE_RESULT_OK;
    
    // Copy card info with size validation
    if (sizeof(PN532_CardInfo_t) <= sizeof(g_transaction_manager.card_info)) {
        memcpy(&g_transaction_manager.card_info, card_info, sizeof(PN532_CardInfo_t));
    } else {
        MIFARE_LOG("ERROR: Card info structure size mismatch");
        xSemaphoreGive(g_transaction_manager.transaction_mutex);
        return MIFARE_RESULT_ERROR;
    }
    MIFARE_SetCardState(MIFARE_CARD_STATE_INITIALIZING);
    g_transaction_manager.consecutive_errors = 0;
    last_authenticated_sector = -1;  // Clear authentication cache for new card
    
    // CRITICAL: Clear any previous abort flags when a new card is detected
    g_transaction_manager.card_removal_abort = false;
    
    // Transition to appropriate state
    if (g_transaction_manager.dispense_state == DISPENSE_STATE_IDLE ||
        g_transaction_manager.dispense_state == DISPENSE_STATE_ERROR) {
        
        // Clear error state when attempting new card initialization
        if (g_transaction_manager.dispense_state == DISPENSE_STATE_ERROR) {
            MIFARE_LOG("Clearing error state to retry card initialization");
            MIFARE_SetCardState(MIFARE_CARD_STATE_INITIALIZING);
            g_transaction_manager.consecutive_errors = 0;
        }
        
        mifare_transition_state(DISPENSE_STATE_CARD_DETECTED);
        
        // Start stability timer for card presence confirmation
        TickType_t now = xTaskGetTickCount();
        g_transaction_manager.card_first_detected_tick = now;
        MIFARE_LOG("Card first detected - waiting %d ms for stability confirmation", 
                   MIFARE_STABILITY_TIMEOUT_MS);
        
        // Release mutex before calling auto-initialize (it takes its own mutex)
        xSemaphoreGive(g_transaction_manager.transaction_mutex);
        
        // Attempt to detect and auto-initialize card (1000000 mL = 1000L default balance)
        result = MIFARE_DetectAndAutoInitializeCard(&g_transaction_manager.card_info, 1000000);
        if (result == MIFARE_RESULT_OK) {
            MIFARE_LOG("Card detected and validated - Ready for dispensing");
            mifare_transition_state(DISPENSE_STATE_READY_TO_DISPENSE);
            
            // Set card state to PRESENT after successful initialization
            MIFARE_CardState_t current_state = MIFARE_GetCardState();
            MIFARE_LOG("Setting card state to PRESENT (was %d)", current_state);
            MIFARE_SetCardState(MIFARE_CARD_STATE_PRESENT);
        } else if (result == MIFARE_RESULT_PN532_CORRUPTED) {
            mifare_transition_state(DISPENSE_STATE_ERROR);
            MIFARE_SetCardState(MIFARE_CARD_STATE_ERROR);
            MIFARE_LOG("Card validation failed due to PN532 corruption. System in error state.");
            
            // Set cooldown to prevent immediate re-detection loop
            g_transaction_manager.pn532_recovery_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
            MIFARE_LOG("Error state cooldown: 2000ms before retry allowed");
        } else {
            mifare_transition_state(DISPENSE_STATE_IDLE);  // CRITICAL: Reset to IDLE to allow retry
            MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
            MIFARE_LOG("Card validation failed: %s - resetting to IDLE for retry", MIFARE_GetResultString(result));
            
            // Set short cooldown to prevent immediate re-detection loop
            g_transaction_manager.pn532_recovery_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(500);
            MIFARE_LOG("Init failure cooldown: 500ms before retry allowed");
        }
        return result;
    }
    
    xSemaphoreGive(g_transaction_manager.transaction_mutex);
    return result;
}

/**
 * @brief Process card removal event (with stability timeout)
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_ProcessCardRemoved(void)
{
    if (xSemaphoreTake(g_transaction_manager.transaction_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return MIFARE_RESULT_BUSY;
    }

    TickType_t now = xTaskGetTickCount();
    
    // Stability layer: Don't immediately declare card removed
    // Start/update the "card lost" timer
    if (g_transaction_manager.card_first_lost_tick == 0) {
        g_transaction_manager.card_first_lost_tick = now;
        MIFARE_LOG("Card first lost - waiting %d ms for removal confirmation", MIFARE_REMOVAL_STABILITY_MS);
        xSemaphoreGive(g_transaction_manager.transaction_mutex);
        return MIFARE_RESULT_OK;  // Don't act yet
    }
    
    // Check if card has been absent for required stability time
    uint32_t absent_ms = pdTICKS_TO_MS(now - g_transaction_manager.card_first_lost_tick);
    if (absent_ms < MIFARE_REMOVAL_STABILITY_MS) {
        // Not absent long enough yet - might be transient RF issue
        MIFARE_LOG("Card absent for %lu ms (need %d ms)", absent_ms, MIFARE_REMOVAL_STABILITY_MS);
        xSemaphoreGive(g_transaction_manager.transaction_mutex);
        return MIFARE_RESULT_OK;  // Don't act yet
    }
    
    // Card has been absent for >1s - CONFIRMED removal
    MIFARE_LOG("Card removal CONFIRMED after %lu ms - State: %s", 
               absent_ms, MIFARE_GetStateString(g_transaction_manager.dispense_state));
    
    MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
    last_authenticated_sector = -1;  // Clear authentication cache
    mifare_clear_write_snapshot();
    last_card_write_changed = false;
    no_change_log_reported = false;
    
    // Reset write failure tracking when card removed
    g_transaction_manager.write_failure_first_tick = 0;
    g_transaction_manager.consecutive_write_failures = 0;
    
    // Reset stability tracking
    g_transaction_manager.card_first_detected_tick = 0;
    g_transaction_manager.card_confirmed_present_tick = 0;
    g_transaction_manager.card_first_lost_tick = 0;
    g_transaction_manager.card_presence_confirmed = false;

    // Only handle emergency rollback if ACTIVELY DISPENSING (transaction in progress)
    // Don't trigger emergency rollback for cards that are just sitting in ready state
    if (g_transaction_manager.transaction_active &&
        (g_transaction_manager.dispense_state == DISPENSE_STATE_DISPENSING ||
         g_transaction_manager.dispense_state == DISPENSE_STATE_UPDATING_CARD ||
         g_transaction_manager.dispense_state == DISPENSE_STATE_FINALIZING)) {
        mifare_transition_state(DISPENSE_STATE_CARD_REMOVED_DURING_DISPENSING);
        MIFARE_HandleCardRemovalDuringDispense();
    } else {
        // Ensure PN532 is unlocked even if card removed before transaction started
        // This prevents the polling task from staying paused forever
        
        // CLEANUP: Clear card data if removed while idle/ready
        memset(&g_transaction_manager.current_card, 0, sizeof(MIFARE_CardData_t));
    }

    // Update UI - card is confirmed removed
    if (g_transaction_manager.ui_state_card_present) {
        ui_set_label_text(ui_cardRemaining, "0mL");
        ui_set_bar_value(ui_totalRemainingBar, 0, LV_ANIM_ON);
        // Clear customer ID display
        ui_set_visibility(ui_customerID, false);
        g_transaction_manager.ui_state_card_present = false;
    }
    
    // Reset the dispense state machine
    g_transaction_manager.dispense_state = DISPENSE_STATE_IDLE;

    xSemaphoreGive(g_transaction_manager.transaction_mutex);
    return MIFARE_RESULT_OK;
}

/**
 * @brief Notify transaction manager that PN532 was reset
 * This sets the recovery cooldown to prevent premature write operations
 */
void MIFARE_NotifyPN532Reset(void)
{
    pn532_recovery_ready_tick = xTaskGetTickCount() + pdMS_TO_TICKS(PN532_POST_RESET_COOLDOWN_MS);
    USB_Log_Printf("MIFARE: PN532 recovery cooldown started for %u ms\r\n", PN532_POST_RESET_COOLDOWN_MS);
}

/*Card Data Operations ----------------------------------------------*/

/**
 * @brief Read and validate card data with error recovery
 * @param card_data Pointer to card data structure
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_ReadCardData(MIFARE_CardData_t *card_data)
{
    if (card_data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    MIFARE_Result_t result;
    uint8_t block_data[16];
    
    // Verify card is still present
    MIFARE_LOG("[READ CARD DATA] Verifying card presence before read...");
    result = MIFARE_VerifyCardPresence();
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("[READ CARD DATA] Card presence check FAILED: %s", MIFARE_GetResultString(result));
        return result;
    }
    MIFARE_LOG("[READ CARD DATA] Card presence verified OK");
    
    // Read header block (authentication is handled internally by mifare_read_block_safe)
    result = mifare_read_block_safe(MIFARE_BLOCK_HEADER, block_data);
    if (result != MIFARE_RESULT_OK) {
        return result;
    }
    
    // Ensure we don't copy more than the block size or destination size
    size_t copy_size = sizeof(MIFARE_CardHeader_t);
    if (copy_size > 16) {  // MIFARE block size is 16 bytes
        MIFARE_LOG("ERROR: Header size exceeds block size");
        return MIFARE_RESULT_ERROR;
    }
    memcpy(&card_data->header, block_data, copy_size);
    
    // Validate header
    if (card_data->header.magic_bytes != MIFARE_MAGIC_BYTES) {
        MIFARE_LOG("Invalid magic bytes: 0x%08lX", card_data->header.magic_bytes);
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    if (card_data->header.format_version != MIFARE_FORMAT_VERSION) {
        MIFARE_LOG("Unsupported format version: %d", card_data->header.format_version);
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    // OPTIMIZED: Read recovery info FIRST to get CRCs for validation
    // This allows us to validate primary/backup immediately as we read them
    result = mifare_read_block_safe(MIFARE_BLOCK_RECOVERY_INFO, block_data);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to read recovery info block - cannot validate data");
        return result;
    }
    
    copy_size = sizeof(MIFARE_RecoveryInfo_t);
    if (copy_size > 16) {
        MIFARE_LOG("ERROR: Recovery info size exceeds block size");
        return MIFARE_RESULT_ERROR;
    }
    memcpy(&card_data->recovery_info, block_data, copy_size);
    
    USB_Log_Printf("MIFARE: Recovery info read - Primary CRC: 0x%04X, Backup CRC: 0x%04X\r\n",
                   card_data->recovery_info.primary_data_crc,
                   card_data->recovery_info.backup_data_crc);
    
    // OPTIMIZED: Read and validate primary user data immediately
    result = mifare_read_block_safe(MIFARE_BLOCK_USER_PRIMARY, block_data);
    bool primary_valid = false;
    
    if (result == MIFARE_RESULT_OK) {
        copy_size = sizeof(MIFARE_UserData_t);
        if (copy_size > 16) {
            MIFARE_LOG("ERROR: User data size exceeds block size");
            return MIFARE_RESULT_ERROR;
        }
        memcpy(&card_data->user_primary, block_data, copy_size);
        
        // Validate CRC immediately
        uint16_t calculated_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_primary, 
                                                          sizeof(MIFARE_UserData_t));
        if (calculated_crc == card_data->recovery_info.primary_data_crc) {
            primary_valid = true;
            MIFARE_LOG("Primary data CRC valid - proceeding immediately");
        } else {
            MIFARE_LOG("Primary CRC mismatch: calc=0x%04X, stored=0x%04X", 
                      calculated_crc, card_data->recovery_info.primary_data_crc);
        }
    }
    
    // OPTIMIZED: Only read backup if primary is invalid
    // If primary is valid, backup will be overwritten during next write operation
    if (!primary_valid) {
        MIFARE_LOG("Primary invalid, checking backup...");
        result = mifare_read_block_safe(MIFARE_BLOCK_USER_BACKUP, block_data);
        
        if (result == MIFARE_RESULT_OK) {
            copy_size = sizeof(MIFARE_UserData_t);
            if (copy_size > 16) {
                MIFARE_LOG("ERROR: Backup user data size exceeds block size");
                return MIFARE_RESULT_ERROR;
            }
            memcpy(&card_data->user_backup, block_data, copy_size);
            
            // Validate backup CRC
            uint16_t calculated_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_backup, 
                                                              sizeof(MIFARE_UserData_t));
            if (calculated_crc == card_data->recovery_info.backup_data_crc) {
                MIFARE_LOG("Backup data CRC valid - using backup");
                // Copy backup to primary (backup will be fixed during next write)
                memcpy(&card_data->user_primary, &card_data->user_backup, sizeof(MIFARE_UserData_t));
                // Sync CRC so validation passes
                card_data->recovery_info.primary_data_crc = card_data->recovery_info.backup_data_crc;
                primary_valid = true;  // Now we have valid data in primary
            } else {
                MIFARE_LOG("Backup CRC mismatch: calc=0x%04X, stored=0x%04X", 
                          calculated_crc, card_data->recovery_info.backup_data_crc);
                // Both primary and backup invalid - try recovery
                MIFARE_LOG("Neither primary nor backup CRC valid - attempting recovery");
                return MIFARE_RESULT_CARD_CORRUPTED;
            }
        } else {
            // Failed to read backup
            MIFARE_LOG("Failed to read backup block - attempting recovery");
            return MIFARE_RESULT_CARD_CORRUPTED;
        }
    } else {
        // Primary is valid, so we skipped reading backup.
        // Copy primary to backup so that validation passes (since we assume backup is irrelevant if primary is good)
        memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
        // Sync CRC so validation passes
        card_data->recovery_info.backup_data_crc = card_data->recovery_info.primary_data_crc;
    }
    // If primary is valid, we're done - skip reading backup entirely (will be overwritten later)
    
    // Read usage data block (lifetime statistics)
    result = mifare_read_block_safe(MIFARE_BLOCK_USAGE_DATA, block_data);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to read usage data block");
        // Initialize to zero if read fails (non-critical for operation)
        memset(&card_data->usage_data, 0, sizeof(MIFARE_UsageData_t));
    } else {
        copy_size = sizeof(MIFARE_UsageData_t);
        if (copy_size > 16) {  // MIFARE block size is 16 bytes
            MIFARE_LOG("ERROR: Usage data size exceeds block size");
            return MIFARE_RESULT_ERROR;
        }
        memcpy(&card_data->usage_data, block_data, copy_size);
    }
    
    // Recovery info already read earlier for CRC validation (skip duplicate read)
    
    // Read fast balance cache (non-critical, continue if fails)
    result = mifare_read_block_safe(MIFARE_BLOCK_FAST_BALANCE_PRIMARY, block_data);
    if (result == MIFARE_RESULT_OK) {
        copy_size = sizeof(MIFARE_FastBalance_t);
        if (copy_size <= 16) {
            memcpy(&card_data->fast_balance_primary, block_data, copy_size);
            
            // Sync fast balance to main if it's newer
            mifare_fast_balance_sync_to_main(card_data);
        }
    } else {
        MIFARE_LOG("Fast balance cache read failed (non-critical)");
    }
    
    // Read fast balance backup (non-critical)
    result = mifare_read_block_safe(MIFARE_BLOCK_FAST_BALANCE_BACKUP, block_data);
    if (result == MIFARE_RESULT_OK) {
        copy_size = sizeof(MIFARE_FastBalance_t);
        if (copy_size <= 16) {
            memcpy(&card_data->fast_balance_backup, block_data, copy_size);
        }
    }
    
    // Read account data (phone number, validity)
    result = mifare_read_block_safe(MIFARE_BLOCK_ACCOUNT_DATA, block_data);
    if (result == MIFARE_RESULT_OK) {
        copy_size = sizeof(MIFARE_AccountData_t);
        if (copy_size <= 16) {
            memcpy(&card_data->account_data, block_data, copy_size);
            MIFARE_LOG("Account data read successfully");
        }
    } else {
        MIFARE_LOG("Account data read failed - will initialize on validation");
        // Clear account data if read fails
        memset(&card_data->account_data, 0, sizeof(MIFARE_AccountData_t));
    }
    
    // Validate data integrity
    result = MIFARE_ValidateCardData(card_data);
    if (result == MIFARE_RESULT_OK) {
        card_data->data_valid = true;
        card_data->last_read_time = mifare_get_timestamp();
        mifare_update_write_snapshot(card_data);
        last_card_write_changed = false;
        no_change_log_reported = false;
        
        // Check if phone number is empty (all zeros) and initialize with default
        bool phone_is_empty = mifare_is_account_data_empty(&card_data->account_data);
        
        if (phone_is_empty) {
            MIFARE_LOG("Phone number empty, initializing with default: 07970242024");
            
            // Encode default phone number "07970242024" with Normal validity into raw_data
            mifare_encode_account_data(&card_data->account_data, "07970242024", CARD_VALIDITY_NORMAL);
            
            // Debug log
            static char phone_debug[12];
            memcpy(phone_debug, &card_data->account_data.raw_data[ACCOUNT_DATA_PHONE_OFFSET], ACCOUNT_DATA_PHONE_SIZE);
            phone_debug[ACCOUNT_DATA_PHONE_SIZE] = '\0';
            uint16_t crc = card_data->account_data.raw_data[ACCOUNT_DATA_CRC_OFFSET] | 
                          (card_data->account_data.raw_data[ACCOUNT_DATA_CRC_OFFSET + 1] << 8);
            MIFARE_LOG("Writing account data: phone=%s, validity=%u, CRC=0x%04X",
                      phone_debug,
                      card_data->account_data.raw_data[ACCOUNT_DATA_VALIDITY_OFFSET],
                      crc);
            
            // Write account data to card
            result = mifare_write_block_safe(MIFARE_BLOCK_ACCOUNT_DATA, 
                                             card_data->account_data.raw_data);
            if (result == MIFARE_RESULT_OK) {
                MIFARE_LOG("Default phone number written to card successfully");
                
                // Verify by reading it back
                uint8_t verify_block[16];
                vTaskDelay(pdMS_TO_TICKS(20)); // Small delay for card to settle
                result = mifare_read_block_safe(MIFARE_BLOCK_ACCOUNT_DATA, verify_block);
                if (result == MIFARE_RESULT_OK) {
                    char phone_verify[12];
                    memcpy(phone_verify, &verify_block[ACCOUNT_DATA_PHONE_OFFSET], ACCOUNT_DATA_PHONE_SIZE);
                    phone_verify[ACCOUNT_DATA_PHONE_SIZE] = '\0';
                    MIFARE_LOG("Verification read: phone=%s, validity=%u",
                              phone_verify,
                              verify_block[ACCOUNT_DATA_VALIDITY_OFFSET]);
                } else {
                    MIFARE_LOG("Failed to verify account data write");
                }
            } else {
                MIFARE_LOG("Failed to write default phone number to card");
            }
        } else {
            // Log existing phone number
            static char phone_existing[12];
            memcpy(phone_existing, &card_data->account_data.raw_data[ACCOUNT_DATA_PHONE_OFFSET], ACCOUNT_DATA_PHONE_SIZE);
            phone_existing[ACCOUNT_DATA_PHONE_SIZE] = '\0';
            MIFARE_LOG("Phone number already set: '%s' (validity=%u)", 
                      phone_existing,
                      card_data->account_data.raw_data[ACCOUNT_DATA_VALIDITY_OFFSET]);
            // Debug: dump raw bytes
            MIFARE_LOG("Raw phone bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      card_data->account_data.raw_data[0], card_data->account_data.raw_data[1], 
                      card_data->account_data.raw_data[2], card_data->account_data.raw_data[3],
                      card_data->account_data.raw_data[4], card_data->account_data.raw_data[5],
                      card_data->account_data.raw_data[6], card_data->account_data.raw_data[7],
                      card_data->account_data.raw_data[8], card_data->account_data.raw_data[9],
                      card_data->account_data.raw_data[10]);
        }
        
        return MIFARE_RESULT_OK;
    }
    
    return result;
}

/**
 * @brief Update transaction progress during dispensing
 * @param dispensed_ml Amount dispensed since last update
 * @param flow_rate_lpm Current flow rate in liters per minute
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_UpdateTransactionProgress(uint32_t dispensed_ml, float flow_rate_lpm)
{
    if (!g_transaction_manager.transaction_active) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Check if System.c polling detected card removal
    // This catches removals even when PN532 writes succeed due to caching
    if (g_transaction_manager.card_state != MIFARE_CARD_STATE_PRESENT) {
        MIFARE_LOG("[UPDATE PROGRESS] Card removed (state=%d) - aborting", 
                   g_transaction_manager.card_state);
        return MIFARE_HandleCardRemovalDuringDispense();
    }
    
    MIFARE_Result_t result = MIFARE_RESULT_OK;
    
    // Update dispensed amounts with underflow protection
    g_transaction_manager.total_dispensed_this_session += dispensed_ml;
    
    // Prevent underflow: ensure dispensed_ml doesn't exceed balance
    uint32_t current_balance = g_transaction_manager.current_card.user_primary.balance_ml;
    if (dispensed_ml > current_balance) {
        MIFARE_LOG("WARNING: Dispensed (%lu mL) exceeds balance (%lu mL), clamping to zero", 
                   dispensed_ml, current_balance);
        g_transaction_manager.current_card.user_primary.balance_ml = 0;
    } else {
        g_transaction_manager.current_card.user_primary.balance_ml -= dispensed_ml;
    }
    
    g_transaction_manager.current_card.usage_data.total_dispensed_ml += dispensed_ml;
    g_transaction_manager.current_card.user_primary.transaction_state = TRANSACTION_STATE_IN_PROGRESS;
    
    bool fast_balance_updated = false;

    // Update fast balance cache every 500ms (lightweight, balance-only write)
    uint32_t current_time = mifare_get_timestamp();
    if ((current_time - g_transaction_manager.last_fast_balance_update_time) >= MIFARE_FAST_BALANCE_UPDATE_MS) {
        // Update fast balance structure
        mifare_fast_balance_update(&g_transaction_manager.current_card.fast_balance_primary,
                                   g_transaction_manager.current_card.user_primary.balance_ml);
        
        // Alternating write pattern for fast balance: write primary OR backup, not both
        // This reduces I2C traffic during high-frequency updates
        bool write_primary = (g_transaction_manager.write_cycle_counter == 0);
        uint8_t target_block = write_primary ? MIFARE_BLOCK_FAST_BALANCE_PRIMARY : MIFARE_BLOCK_FAST_BALANCE_BACKUP;
        
        uint8_t block_data[16];
        memcpy(block_data, &g_transaction_manager.current_card.fast_balance_primary, sizeof(MIFARE_FastBalance_t));
        
        MIFARE_Result_t fast_result = mifare_write_block_safe(target_block, block_data);
        if (fast_result == MIFARE_RESULT_OK) {
            g_transaction_manager.last_fast_balance_update_time = current_time;
            fast_balance_updated = true;
        } else if (fast_result == MIFARE_RESULT_CARD_REMOVED) {
            // Card removed during write - detected by mifare_write_block_safe()
            MIFARE_LOG("Fast balance write failed - card removed during write");
            return MIFARE_HandleCardRemovalDuringDispense();
        } else if (fast_result == MIFARE_RESULT_WRITE_FAILED) {
            // Write failed but card is still present (I2C/PN532 issue)
            // This is non-critical for fast balance updates - continue dispensing
            MIFARE_LOG("Fast balance write failed (I2C issue, card present, non-critical)");
            g_transaction_manager.last_fast_balance_update_time = current_time; 
        } else {
            // Other error types - log but continue
            MIFARE_LOG("Fast balance write failed (non-critical), result: %s", MIFARE_GetResultString(fast_result));
            g_transaction_manager.last_fast_balance_update_time = current_time; 
        }
    }
    
    // Update main user data every 2000ms (full data write)
    // STAGGER LOGIC: Only update main data if we didn't just update fast balance
    // This prevents "double writes" in the same tick which can overwhelm the I2C bus/PN532
    if (!fast_balance_updated && (current_time - g_transaction_manager.last_card_update_time) >= MIFARE_MAIN_DATA_UPDATE_MS) {
        result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
        if (result == MIFARE_RESULT_OK) {
            g_transaction_manager.last_card_update_time = current_time;
            g_transaction_manager.consecutive_errors = 0;
            if (last_card_write_changed) {
                MIFARE_LOG("Main data updated: %lu mL dispensed, %lu mL remaining (%.2f L/min)", 
                           g_transaction_manager.total_dispensed_this_session,
                           g_transaction_manager.current_card.user_primary.balance_ml,
                           flow_rate_lpm);
            } else if (!no_change_log_reported) {
                MIFARE_LOG("Main data unchanged - skipping PN532 write (dispensed=%lu mL)",
                           g_transaction_manager.total_dispensed_this_session);
                no_change_log_reported = true;
            }
        } else if (result == MIFARE_RESULT_CARD_REMOVED) {
            // Card removed during write - detected immediately
            MIFARE_LOG("Main data write failed - card removed");
            return MIFARE_HandleCardRemovalDuringDispense();
        } else {
            // Other errors (WRITE_FAILED, etc) - card still present but I2C issues
            g_transaction_manager.consecutive_errors++;
            MIFARE_LOG("Failed to update main data (error %u/%u): %s", 
                       g_transaction_manager.consecutive_errors,
                       MIFARE_CARD_REMOVAL_FAIL_COUNT,
                       MIFARE_GetResultString(result));
            
            // If multiple consecutive I2C errors, something is seriously wrong
            if (g_transaction_manager.consecutive_errors >= MIFARE_CARD_REMOVAL_FAIL_COUNT) {
                MIFARE_LOG("Excessive write failures despite card present - critical I2C error");
                return MIFARE_HandleCardRemovalDuringDispense();
            }
        }
    }
    
    return result;
}

/**
 * @brief Commit the transaction (mark as completed)
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_CommitTransaction(void)
{
    if (!g_transaction_manager.transaction_active) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Set commit ready state
    g_transaction_manager.current_card.user_primary.transaction_state = TRANSACTION_STATE_COMMIT_READY;
    
    MIFARE_Result_t result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write commit ready state");
        g_transaction_manager.transaction_active = false;
        mifare_transition_state(DISPENSE_STATE_ERROR);
        return result;
    }
    
    // Log the transaction
    MIFARE_LogTransaction(2, g_transaction_manager.total_dispensed_this_session, 0);
    
    // Final commit
    g_transaction_manager.current_card.user_primary.transaction_state = TRANSACTION_STATE_COMMITTED;
    result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
    
    if (result == MIFARE_RESULT_OK) {
        g_transaction_manager.transaction_active = false;
        mifare_transition_state(DISPENSE_STATE_READY_TO_DISPENSE);
        MIFARE_LOG("Transaction committed successfully - %lu mL dispensed", 
                   g_transaction_manager.total_dispensed_this_session);
    } else {
        g_transaction_manager.transaction_active = false;
        mifare_transition_state(DISPENSE_STATE_ERROR);
        MIFARE_LOG("Transaction commit failed");
    }
    
    
    return result;
}

/**
 * @brief Rollback an active transaction
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_RollbackTransaction(void)
{
    if (!g_transaction_manager.transaction_active) {
        MIFARE_LOG("No active transaction to rollback");
        return MIFARE_RESULT_ERROR;
    }
    
    // Check if card removal abort flag is set - if so, discard transaction data
    if (g_transaction_manager.card_removal_abort) {
        MIFARE_LOG("Card removed during transaction - DISCARDING rollback data (different card may be placed)");
        
        // DO NOT restore balance or modify current_card data
        // The card that was removed has whatever balance it had at last successful write
        // A different card may be placed next, so we must not keep stale data
        
        // Log the aborted/discarded transaction
        MIFARE_LogTransaction(3, g_transaction_manager.total_dispensed_this_session, 0); // Type 3 = Rollback/Abort
        
        // Clear ALL transaction state and card data
        g_transaction_manager.transaction_active = false;
        g_transaction_manager.total_dispensed_this_session = 0;
        g_transaction_manager.has_last_written_snapshot = false;  // Invalidate snapshot
        
        // Zero out card data to prevent confusion with next card
        memset(&g_transaction_manager.current_card, 0, sizeof(MIFARE_CardData_t));
        memset(&g_transaction_manager.last_written_user_data, 0, sizeof(MIFARE_UserData_t));
        memset(&g_transaction_manager.last_written_recovery_info, 0, sizeof(MIFARE_RecoveryInfo_t));
        
        // Transition to idle state (card is gone, ready for new card)
        mifare_transition_state(DISPENSE_STATE_IDLE);
        MIFARE_LOG("Transaction data discarded - system ready for new card");
        
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    // Card is still present - perform normal rollback
    MIFARE_LOG("Rolling back transaction - restoring previous balance");
    
    // Restore original balance by undoing any dispensed amount
    g_transaction_manager.current_card.user_primary.balance_ml += g_transaction_manager.total_dispensed_this_session;
    
    // Reset transaction state
    g_transaction_manager.current_card.user_primary.transaction_state = TRANSACTION_STATE_IDLE;
    
    // Card is still present - attempt to write rollback to card
    MIFARE_LOG("Card still present - attempting to write rollback to card");
    
    // CRITICAL: Wait for any in-progress I2C operations to complete
    // If safety timeout occurred during a write, the I2C semaphore may still be held
    // Give it time to complete and release the semaphore
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Wake PN532 if it entered sleep mode during long dispense operation
    PN532_Status_t pn532_status = PN532_Wakeup();
    if (pn532_status != PN532_STATUS_OK) {
        MIFARE_LOG("Failed to wake PN532 for rollback, continuing anyway...");
        // Give it another chance - maybe still recovering from interrupted operation
        vTaskDelay(pdMS_TO_TICKS(100));
        pn532_status = PN532_Wakeup();
        if (pn532_status != PN532_STATUS_OK) {
            MIFARE_LOG("PN532 wakeup failed again - I2C may be stuck");
        }
    }
    
    // Additional delay to ensure PN532 is fully ready
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Try to write the rollback to the card with retry in case I2C is still busy
    MIFARE_Result_t result = MIFARE_RESULT_ERROR;
    const uint8_t max_retries = 3;
    for (uint8_t retry = 0; retry < max_retries; retry++) {
        result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
        if (result == MIFARE_RESULT_OK) {
            break;
        }
        
        if (retry < max_retries - 1) {
            MIFARE_LOG("Rollback write failed (attempt %d/%d), retrying...", retry + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(100));  // Wait before retry
        }
    }
    
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write rollback to card after %d attempts - balance restored in memory only", max_retries);
    }
    
    // Log the rollback transaction
    MIFARE_LogTransaction(3, g_transaction_manager.total_dispensed_this_session, 0); // Type 3 = Rollback
    
    // Clear transaction state
    g_transaction_manager.transaction_active = false;
    g_transaction_manager.total_dispensed_this_session = 0;
    
    // Transition to appropriate state
    if (result == MIFARE_RESULT_OK) {
        mifare_transition_state(DISPENSE_STATE_READY_TO_DISPENSE);
        MIFARE_LOG("Transaction rollback completed successfully");
    } else {
        mifare_transition_state(DISPENSE_STATE_ERROR);
        MIFARE_LOG("Transaction rollback completed with errors");
    }

    
    return result;
}

/**
 * @brief Handle card removal during dispensing with rollback
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_HandleCardRemovalDuringDispense(void)
{
    MIFARE_LOG("CRITICAL: Card removed during dispensing - attempting emergency rollback");

    // CRITICAL: Set abort flag to prevent any further write attempts
    // This is essential because the card is gone and PN532 may be in bad state
    g_transaction_manager.card_removal_abort = true;
    
    // Set recovery delay - PN532 needs time to recover from failed operations
    // Prevents immediate re-detection causing further errors
    g_transaction_manager.pn532_recovery_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(150);
    
    // Set card state to ABSENT so polling can resume
    MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
    last_authenticated_sector = -1;  // Clear authentication cache
    
    mifare_clear_write_snapshot();
    last_card_write_changed = false;
    no_change_log_reported = false;
    
    // Reset write failure tracking
    g_transaction_manager.write_failure_first_tick = 0;
    g_transaction_manager.consecutive_write_failures = 0;
    
    // Mark transaction as needing rollback
    g_transaction_manager.current_card.user_primary.transaction_state = TRANSACTION_STATE_ROLLBACK;
    
    // If we can't write to the card (it's gone), we need to store the rollback information
    // for when the card is reinserted
    // This is where a system-side transaction log would be critical
    
    g_transaction_manager.transaction_active = false;
    
    // Update UI - card removed during dispensing
    if (g_transaction_manager.ui_state_card_present) {
        ui_set_label_text(ui_cardRemaining, "0mL");
        ui_set_bar_value(ui_totalRemainingBar, 0, LV_ANIM_ON);
        g_transaction_manager.ui_state_card_present = false;
        MIFARE_LOG("UI updated - card absent");
    }
    
    // Log the incomplete transaction for audit purposes
    MIFARE_LOG("Emergency rollback: %lu mL was dispensed before card removal", 
               g_transaction_manager.total_dispensed_this_session);
    
    // CLEANUP: Clear transaction session data
    g_transaction_manager.total_dispensed_this_session = 0;
    
    // CLEANUP: Clear card data to prevent stale data usage
    memset(&g_transaction_manager.current_card, 0, sizeof(MIFARE_CardData_t));
    
    // Reset to IDLE state so system can accept new cards
    // This must be done AFTER logging/cleanup but before returning
    g_transaction_manager.dispense_state = DISPENSE_STATE_IDLE;
    MIFARE_LOG("State transition: Card Removed -> Idle (ready for new card)");

    
    return MIFARE_RESULT_CARD_REMOVED;
}

/*Private helper functions --------------------------------------*/

/**
 * @brief Validate if a block number is safe to access
 * @param block_number Block number to validate
 * @param is_write True if validating for write operation, false for read
 * @return bool True if block is valid and safe to access
 * 
 * @details MIFARE Classic 1K has blocks 0-63 organized in 16 sectors.
 *          Sector trailers (blocks 3, 7, 11, 15, etc.) contain authentication keys
 *          and access bits. Writing to these blocks can permanently lock the card.
 *          This function prevents accidental writes to sector trailers.
 */
static bool mifare_is_valid_block(uint8_t block_number, bool is_write)
{
    // Block 0 is manufacturer data - read-only, should never be written
    if (is_write && block_number == 0) {
        MIFARE_LOG("ERROR: Attempted to write to manufacturer block 0");
        return false;
    }
    
    // MIFARE Classic 1K has blocks 0-63
    if (block_number > 63) {
        MIFARE_LOG("ERROR: Invalid block number %d (max 63)", block_number);
        return false;
    }
    
    // Sector trailers contain keys and access bits - MUST NOT be written
    if (is_write && MIFARE_IS_SECTOR_TRAILER(block_number)) {
        MIFARE_LOG("ERROR: Attempted to write to sector trailer block %d", block_number);
        MIFARE_LOG("       Sector trailers contain authentication keys and must not be modified");
        return false;
    }
    
    return true;
}

/**
 * @brief Unified card presence verification - SINGLE SOURCE OF TRUTH for all presence checks
 * 
 * @details This is the ONLY function that should be used to verify card presence via hardware.
 *          It performs both PN532 hardware detection and UID matching in a single call.
 * 
 *          ARCHITECTURE:
 *          - All transaction operations call this function before critical operations
 *          - Replaces all previous individual mifare_verify_card_presence() calls
 *          - Used by: authentication retry, write operations, read operations, monitoring
 * 
 *          DO NOT USE MIFARE_IsCardPresent() for hardware verification - that's state-based only.
 *          Use this function for actual PN532 hardware card detection.
 * 
 * @return MIFARE_Result_t 
 *         - MIFARE_RESULT_OK: Same card is physically present and responsive
 *         - MIFARE_RESULT_CARD_REMOVED: Card not detected or different card detected
 */
MIFARE_Result_t MIFARE_VerifyCardPresence(void)
{
    PN532_CardInfo_t current_card;
    PN532_Status_t status = PN532_DetectCard(&current_card);
    
    if (status != PN532_STATUS_CARD_DETECTED) {
        MIFARE_LOG("[PRESENCE CHECK] Card not detected (status=%d)", status);
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    // Verify it's the same card with bounds checking
    if (current_card.uid_length != g_transaction_manager.card_info.uid_length ||
        current_card.uid_length > sizeof(current_card.uid) ||
        g_transaction_manager.card_info.uid_length > sizeof(g_transaction_manager.card_info.uid) ||
        memcmp(current_card.uid, g_transaction_manager.card_info.uid, current_card.uid_length) != 0) {
        MIFARE_LOG("[PRESENCE CHECK] Different card detected (UID mismatch)");
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    // Only log success during critical operations (removed routine OK logging)
    return MIFARE_RESULT_OK;
}

/**
 * @brief Authenticate MIFARE block with sector caching
 * @param block_number Block to authenticate
 * @return MIFARE_Result_t Operation result
 * 
 * @details This function caches the last authenticated sector to avoid
 *          unnecessary re-authentication. MIFARE Classic keeps authentication
 *          active for a sector until another sector is accessed or card is removed.
 */
static MIFARE_Result_t mifare_authenticate_block(uint8_t block_number)
{
    uint8_t sector = MIFARE_GET_SECTOR(block_number);
    
    // Skip authentication if we're already authenticated to this sector
    if (last_authenticated_sector == (int8_t)sector) {
        // Using cached auth - no logging to reduce spam
        return MIFARE_RESULT_OK;
    }
    
    MIFARE_LOG("Authenticating block %d (sector %d) with UID length %d", 
               block_number, sector, g_transaction_manager.card_info.uid_length);
    
    // Retry authentication up to 3 times with delay between attempts
    // This handles transient I2C communication errors
    const uint8_t max_retries = 3;
    PN532_Status_t status = PN532_STATUS_ERROR;
    
    for (uint8_t retry = 0; retry < max_retries; retry++) {
        if (retry > 0) {
            // Check if card is still present before retrying - fail fast if removed
            MIFARE_LOG("[AUTH RETRY] Checking card presence before retry %d/%d...", retry, max_retries);
            MIFARE_Result_t presence = MIFARE_VerifyCardPresence();
            if (presence != MIFARE_RESULT_OK) {
                MIFARE_LOG("[AUTH RETRY] Card removed during authentication retry - aborting");
                last_authenticated_sector = -1;
                return MIFARE_RESULT_CARD_REMOVED;
            }
            MIFARE_LOG("[AUTH RETRY] Card still present, proceeding with retry");
            
            USB_Log_Printf("MIFARE: Authentication retry %d/%d for block %d\r\n", 
                          retry, max_retries - 1, block_number);
            vTaskDelay(pdMS_TO_TICKS(50));  // Allow I2C/RF recovery
        }
        

        status = PN532_MifareAuthenticate(
            block_number, 
            g_transaction_manager.card_info.uid,
            g_transaction_manager.card_info.uid_length,
            mifare_auth_key
        );
        
        if (status == PN532_STATUS_OK) {
            if (retry > 0) {
                 MIFARE_LOG("MIFARE: Authentication succeeded on retry %d\r\n", retry);
            }
            last_authenticated_sector = (int8_t)sector;
            MIFARE_LOG("Authentication successful for sector %d", sector);
            return MIFARE_RESULT_OK;
        }
    }
    
    // All retries failed
    last_authenticated_sector = -1;  // Clear cache on failure
    MIFARE_LOG("Authentication FAILED for block %d (sector %d) after %d attempts, PN532 status: %d", 
               block_number, sector, max_retries, status);
    return MIFARE_RESULT_AUTHENTICATION_FAILED;
}

/**
 * @brief Safely read MIFARE block with authentication and verification
 * @param block_number Block to read
 * @param data Buffer to store data (16 bytes)
 * @return MIFARE_Result_t Operation result
 * 
 * @details This function performs the following safety checks:
 *          1. Validates the block number
 *          2. Authenticates the block before reading
 *          3. Reads the block data
 */
static MIFARE_Result_t mifare_read_block_safe(uint8_t block_number, uint8_t *data)
{
    if (data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Validate block number (reads can access any block except invalid ones)
    if (!mifare_is_valid_block(block_number, false)) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Authenticate the block before reading
    MIFARE_Result_t auth_result = mifare_authenticate_block(block_number);
    if (auth_result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Authentication failed for block %d", block_number);
        return auth_result;
    }
    
    // Read the block with retries
    PN532_Status_t status = PN532_STATUS_ERROR;
    const uint8_t max_retries = 3;
    
    for (uint8_t retry = 0; retry < max_retries; retry++) {
        status = PN532_MifareReadBlock(block_number, data);
        if (status == PN532_STATUS_OK) {
            return MIFARE_RESULT_OK;
        }
        
        if (retry < max_retries - 1) {
            // If read failed, maybe we need to re-authenticate?
            // Some cards lose auth state on error
            // Force re-authentication by clearing cache
            last_authenticated_sector = -1;
            vTaskDelay(pdMS_TO_TICKS(10));
            mifare_authenticate_block(block_number);
        }
    }
    
    MIFARE_LOG("Read failed for block %d after %d attempts, status: 0x%02X", block_number, max_retries, status);
    return MIFARE_RESULT_ERROR;
}

/**
 * @brief Safely write MIFARE block with authentication and protection
 * @param block_number Block to write
 * @param data Data to write (16 bytes)
 * @return MIFARE_Result_t Operation result
 * 
 * @details This function performs critical safety checks:
 *          1. Validates the block number is not a sector trailer (contains keys)
 *          2. Validates the block number is not block 0 (manufacturer data)
 *          3. Authenticates the block before writing
 *          4. Writes the block data
 * 
 * @warning This function will REJECT writes to:
 *          - Block 0 (manufacturer block)
 *          - Blocks 3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63
 *            (sector trailers containing authentication keys and access bits)
 */
static MIFARE_Result_t mifare_write_block_safe(uint8_t block_number, uint8_t *data)
{
    if (data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    // CRITICAL: Validate block number to prevent writing to key blocks
    if (!mifare_is_valid_block(block_number, true)) {
        MIFARE_LOG("CRITICAL: Blocked write to protected block %d", block_number);
        return MIFARE_RESULT_ERROR;
    }
    
    // Only log full writes, not fast balance updates (block 13/14)
    if (block_number != MIFARE_BLOCK_FAST_BALANCE_PRIMARY && block_number != MIFARE_BLOCK_FAST_BALANCE_BACKUP) {
        MIFARE_LOG("Writing to block %d (sector %d)", block_number, MIFARE_GET_SECTOR(block_number));
    }
    
    // Authenticate the block before writing
    // The mifare_authenticate_block function handles caching properly:
    // - It will use cached auth if the sector matches and auth is still valid
    // - It will re-auth if needed (different sector or cache invalidated)
    MIFARE_Result_t auth_result = mifare_authenticate_block(block_number);
    if (auth_result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Authentication failed for write to block %d", block_number);
        return auth_result;
    }
    
    /* Write the block */
    PN532_Status_t status = PN532_MifareWriteBlock(block_number, data);
    if (status != PN532_STATUS_OK) {
        /* Track consecutive write failures across ALL write attempts (not just this function call) */
        TickType_t now = xTaskGetTickCount();
        
        // Check if we're in an existing failure period or starting a new one
        if (g_transaction_manager.write_failure_first_tick == 0) {
            // First failure - check immediately if card removed
            MIFARE_LOG("[WRITE FAILURE #1] Checking if card present...");
            if (MIFARE_VerifyCardPresence() != MIFARE_RESULT_OK) {
                MIFARE_LOG("[WRITE FAILURE #1] Card NOT present - returning CARD_REMOVED");
                return MIFARE_RESULT_CARD_REMOVED;
            }
            // Card present - start tracking
            g_transaction_manager.write_failure_first_tick = now;
            g_transaction_manager.consecutive_write_failures = 1;
            MIFARE_LOG("[WRITE FAILURE #1] Card still present - starting failure tracking");
        } else {
            // Check if this failure is part of the same failure period (within last 1 second)
            uint32_t time_since_first_failure = pdTICKS_TO_MS(now - g_transaction_manager.write_failure_first_tick);
            if (time_since_first_failure < 1000) {
                // Still within the same failure period - increment counter
                g_transaction_manager.consecutive_write_failures++;
            } else {
                // Old failure period expired, start new one
                g_transaction_manager.write_failure_first_tick = now;
                g_transaction_manager.consecutive_write_failures = 1;
                MIFARE_LOG("Previous failure period expired, starting new tracking");
            }
        }
        
        // Check card presence more aggressively - every failure after the first
        uint32_t failure_duration_ms = pdTICKS_TO_MS(now - g_transaction_manager.write_failure_first_tick);
        if (g_transaction_manager.consecutive_write_failures > 1) {
            // Multiple failures - check if card actually removed
            MIFARE_LOG("[WRITE FAILURE #%d] Checking card presence after %lu ms...", 
                       g_transaction_manager.consecutive_write_failures, failure_duration_ms);
            if (MIFARE_VerifyCardPresence() != MIFARE_RESULT_OK) {
                MIFARE_LOG("[WRITE FAILURE #%d] Card REMOVED after %lu ms (count: %d)", 
                           g_transaction_manager.consecutive_write_failures, failure_duration_ms, 
                           g_transaction_manager.consecutive_write_failures);
                return MIFARE_RESULT_CARD_REMOVED;
            }
            MIFARE_LOG("[WRITE FAILURE #%d] Card still PRESENT at %lu ms - I2C issue", 
                       g_transaction_manager.consecutive_write_failures, failure_duration_ms);
        }

        // Force re-authentication for this sector before retrying
        last_authenticated_sector = -1;

        // Brief pause to let PN532/I2C recover from the failed exchange
        vTaskDelay(pdMS_TO_TICKS(10));

        MIFARE_LOG("Retrying write to block %d after clearing auth cache", block_number);
        MIFARE_Result_t retry_auth = mifare_authenticate_block(block_number);
        if (retry_auth == MIFARE_RESULT_OK) {
            status = PN532_MifareWriteBlock(block_number, data);
            if (status == PN532_STATUS_OK) {
                MIFARE_LOG("Write to block %d succeeded after re-authentication", block_number);
                // Reset write failure tracking on success
                g_transaction_manager.write_failure_first_tick = 0;
                g_transaction_manager.consecutive_write_failures = 0;
                return MIFARE_RESULT_OK;
            }
        } else if (retry_auth == MIFARE_RESULT_CARD_REMOVED) {
            MIFARE_LOG("Card removed during write retry authentication");
            return MIFARE_RESULT_CARD_REMOVED;
        } else {
            MIFARE_LOG("Re-authentication failed for block %d during write retry", block_number);
        }

        /* If write fails, perform recovery based on error type */
        if (status != PN532_STATUS_OK) {
            if (status == PN532_STATUS_TIMEOUT) {
                // Card timeout (0x27) - likely card removed
                MIFARE_LOG("Card timeout detected - card likely removed");
            } else {
                // Other error types - log and treat as transient
                MIFARE_LOG("Write failed with status %d", status);
            }
            
            // When write fails after authentication succeeds, actively verify card presence
            // Don't rely on state flags - check PN532 directly for immediate detection
            MIFARE_LOG("[WRITE RETRY] Performing active card presence check via PN532...");
            MIFARE_Result_t presence_check = MIFARE_VerifyCardPresence();
            if (presence_check != MIFARE_RESULT_OK) {
                MIFARE_LOG("[WRITE RETRY] Active presence check FAILED - card removed");
                return MIFARE_RESULT_CARD_REMOVED;
            }
            MIFARE_LOG("[WRITE RETRY] Active presence check OK - card still present");
            
            return MIFARE_RESULT_WRITE_FAILED;
        }
    }
    
    // Only log full data writes, not fast balance updates
    if (block_number != MIFARE_BLOCK_FAST_BALANCE_PRIMARY && block_number != MIFARE_BLOCK_FAST_BALANCE_BACKUP) {
        MIFARE_LOG("Successfully wrote to block %d", block_number);
    }
    
    // Reset write failure tracking on success
    g_transaction_manager.write_failure_first_tick = 0;
    g_transaction_manager.consecutive_write_failures = 0;
    
    return MIFARE_RESULT_OK;
}

/**
 * @brief Transition to new dispensing state
 * @param new_state New state to transition to
 */
static void mifare_transition_state(MIFARE_DispenseState_t new_state)
{
    MIFARE_DispenseState_t old_state = g_transaction_manager.dispense_state;
    g_transaction_manager.dispense_state = new_state;
    
    if (old_state != new_state) {
        MIFARE_LOG("State transition: %s -> %s", 
                   MIFARE_GetStateString(old_state), 
                   MIFARE_GetStateString(new_state));
        
        // Initialize stability timer when entering READY_TO_DISPENSE if not already set
        if (new_state == DISPENSE_STATE_READY_TO_DISPENSE && 
            g_transaction_manager.card_first_detected_tick == 0) {
            TickType_t now = xTaskGetTickCount();
            g_transaction_manager.card_first_detected_tick = now;
            MIFARE_LOG("Card ready - stability timer started (wait %d ms for confirmation)", 
                       MIFARE_STABILITY_TIMEOUT_MS);
        }
        
        // Clear card removal abort flag when entering READY_TO_DISPENSE
        // The card has been verified present and valid, ready for new transaction
        if (new_state == DISPENSE_STATE_READY_TO_DISPENSE) {
            if (g_transaction_manager.card_removal_abort) {
                MIFARE_LOG("Clearing card removal abort flag - card verified present and ready");
                g_transaction_manager.card_removal_abort = false;
            }
        }
    }
}

/**
 * @brief Get current system timestamp
 * @return uint32_t Current timestamp in milliseconds
 */
static uint32_t mifare_get_timestamp(void)
{
    return (uint32_t)xTaskGetTickCount();
}

/*Write Snapshot Helpers ------------------------------------------*/

static void mifare_clear_write_snapshot(void)
{
    memset(&g_transaction_manager.last_written_user_data, 0, sizeof(MIFARE_UserData_t));
    memset(&g_transaction_manager.last_written_recovery_info, 0, sizeof(MIFARE_RecoveryInfo_t));
    g_transaction_manager.has_last_written_snapshot = false;
}

static void mifare_update_write_snapshot(const MIFARE_CardData_t *card_data)
{
    if (card_data == NULL) {
        return;
    }

    memcpy(&g_transaction_manager.last_written_user_data,
           &card_data->user_primary,
           sizeof(MIFARE_UserData_t));
    memcpy(&g_transaction_manager.last_written_recovery_info,
           &card_data->recovery_info,
           sizeof(MIFARE_RecoveryInfo_t));
    g_transaction_manager.has_last_written_snapshot = true;
}

static bool mifare_card_data_changed(const MIFARE_CardData_t *card_data)
{
    if (card_data == NULL) {
        return true;
    }

    if (!g_transaction_manager.has_last_written_snapshot) {
        return true;
    }

    if (memcmp(&card_data->user_primary,
               &g_transaction_manager.last_written_user_data,
               sizeof(MIFARE_UserData_t)) != 0) {
        return true;
    }

    if (card_data->recovery_info.primary_data_crc !=
            g_transaction_manager.last_written_recovery_info.primary_data_crc ||
        card_data->recovery_info.backup_data_crc !=
            g_transaction_manager.last_written_recovery_info.backup_data_crc ||
        card_data->recovery_info.sequence_number !=
            g_transaction_manager.last_written_recovery_info.sequence_number ||
        card_data->recovery_info.recovery_attempts !=
            g_transaction_manager.last_written_recovery_info.recovery_attempts ||
        card_data->recovery_info.integrity_flags !=
            g_transaction_manager.last_written_recovery_info.integrity_flags)
    {
        return true;
    }

    return false;
}

/**
 * @brief Check if account data is empty or invalid (phone number all zeros or non-ASCII)
 * @param account_data Pointer to account data structure
 * @return true if phone number is empty or invalid, false if valid
 */
static bool mifare_is_account_data_empty(const MIFARE_AccountData_t *account_data)
{
    bool all_zeros = true;
    bool has_valid_ascii = true;
    
    // Check phone number bytes (bytes 0-10)
    for (uint8_t i = 0; i < ACCOUNT_DATA_PHONE_SIZE; i++) {
        uint8_t byte = account_data->raw_data[ACCOUNT_DATA_PHONE_OFFSET + i];
        
        if (byte != 0) {
            all_zeros = false;
        }
        
        // Check if byte is a valid ASCII digit ('0'-'9')
        if (byte < '0' || byte > '9') {
            has_valid_ascii = false;
        }
    }
    
    // Phone is considered empty if all zeros OR contains invalid ASCII
    bool is_empty = all_zeros || !has_valid_ascii;
    
    if (is_empty) {
        MIFARE_LOG("Account data invalid - all_zeros=%d, has_valid_ascii=%d", all_zeros, has_valid_ascii);
    }
    
    return is_empty;
}

/**
 * @brief Encode account data into raw_data array
 * @param account_data Pointer to account data structure
 * @param phone_str Phone number string (11 digits, e.g. "07970242024")
 * @param validity Card validity enum value
 * 
 * Layout: bytes 0-10 phone (ASCII), byte 11 validity, bytes 12-13 CRC16, bytes 14-15 reserved
 */
static void mifare_encode_account_data(MIFARE_AccountData_t *account_data, const char *phone_str, MIFARE_CardValidity_t validity)
{
    // Copy phone number (11 bytes) to bytes 0-10
    memcpy(&account_data->raw_data[ACCOUNT_DATA_PHONE_OFFSET], phone_str, ACCOUNT_DATA_PHONE_SIZE);
    
    // Set validity (1 byte) at byte 11
    account_data->raw_data[ACCOUNT_DATA_VALIDITY_OFFSET] = (uint8_t)validity;
    
    // Clear reserved bytes (bytes 14-15)
    account_data->raw_data[ACCOUNT_DATA_RESERVED_OFFSET] = 0;
    account_data->raw_data[ACCOUNT_DATA_RESERVED_OFFSET + 1] = 0;
    
    // Calculate CRC16 over bytes 0-11 (phone + validity)
    uint16_t crc = MIFARE_CALCULATE_CRC16(account_data->raw_data, ACCOUNT_DATA_CRC_OFFSET);
    
    // Store CRC16 in bytes 12-13 (little-endian)
    account_data->raw_data[ACCOUNT_DATA_CRC_OFFSET] = (uint8_t)(crc & 0xFF);
    account_data->raw_data[ACCOUNT_DATA_CRC_OFFSET + 1] = (uint8_t)(crc >> 8);
}

/**
 * @brief Write card data with atomic transaction support
 * @param card_data Pointer to card data structure
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_WriteCardData(MIFARE_CardData_t *card_data)
{
    if (card_data == NULL || !card_data->data_valid) {
        return MIFARE_RESULT_ERROR;
    }
    
    // CRITICAL: If card removal was detected mid-operation, abort immediately
    if (g_transaction_manager.card_removal_abort) {
        MIFARE_LOG("Write aborted - card removal flag set");
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    // Check if PN532 is still recovering from previous write failure
    TickType_t now = xTaskGetTickCount();
    if (now < g_transaction_manager.pn532_recovery_until_tick) {
        uint32_t remaining_ms = pdTICKS_TO_MS(g_transaction_manager.pn532_recovery_until_tick - now);
        MIFARE_LOG("Write deferred - PN532 recovering for %lu ms more", remaining_ms);
        return MIFARE_RESULT_BUSY;
    }
    
    MIFARE_Result_t result;
    uint8_t block_data[16];
    
    if (!mifare_card_data_changed(card_data)) {
        last_card_write_changed = false;
        return MIFARE_RESULT_OK;
    }
    
    // Update timestamps and sequence numbers
    card_data->user_primary.transaction_counter++;
    card_data->recovery_info.last_update_time = mifare_get_timestamp();
    card_data->recovery_info.sequence_number = (card_data->recovery_info.sequence_number + 1) & 0x01;
    
    // Alternating write pattern
    bool write_primary = (g_transaction_manager.write_cycle_counter == 0);
    uint8_t target_block = write_primary ? MIFARE_BLOCK_USER_PRIMARY : MIFARE_BLOCK_USER_BACKUP;
    const char *target_name = write_primary ? "primary" : "backup";
    
    // Update the backup copy in RAM to match primary
    if (sizeof(MIFARE_UserData_t) <= sizeof(card_data->user_backup)) {
        memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    } else {
        MIFARE_LOG("ERROR: User data structure size mismatch");
        return MIFARE_RESULT_ERROR;
    }
    
    // Update CRC
    uint16_t current_data_crc = MIFARE_CALCULATE_CRC16(&card_data->user_primary, sizeof(MIFARE_UserData_t));
    if (write_primary) {
        card_data->recovery_info.primary_data_crc = current_data_crc;
    } else {
        card_data->recovery_info.backup_data_crc = current_data_crc;
    }
    
    // Write the selected data block
    size_t data_size = sizeof(MIFARE_UserData_t);
    memcpy(block_data, write_primary ? (uint8_t*)&card_data->user_primary : (uint8_t*)&card_data->user_backup, data_size);
    
    // Retry loop for robustness against transient noise (up to 500ms)
    TickType_t loop_start_tick = xTaskGetTickCount();
    do {
        // Check for external abort signals (e.g. from polling task)
        if (g_transaction_manager.card_removal_abort || 
            g_transaction_manager.card_state == MIFARE_CARD_STATE_ABSENT) {
            MIFARE_LOG("Write aborted - card removed during retry loop");
            result = MIFARE_RESULT_CARD_REMOVED;
            break;
        }

        result = mifare_write_block_safe(target_block, block_data);
        if (result == MIFARE_RESULT_OK) break;
        if (result == MIFARE_RESULT_CARD_REMOVED) {
            MIFARE_LOG("Card removed during %s data write", target_name);
            break;
        }
        
        // Check local timeout (robust against other tasks resetting global flags)
        // Increased from 1000ms to 2000ms to allow more retries in noisy environments
        if (pdTICKS_TO_MS(xTaskGetTickCount() - loop_start_tick) > 2000) {
             MIFARE_LOG("Write timeout exceeded (2000ms) - persistent I2C errors");
             result = MIFARE_RESULT_TIMEOUT; // Explicitly set result to timeout
             break;
        }
        
        // Small delay before retry to let I2C bus settle
        vTaskDelay(pdMS_TO_TICKS(20));
        
    } while (result != MIFARE_RESULT_OK);

    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write %s data (block %u)", target_name, target_block);
        last_card_write_changed = false;
        return result;
    }
    
    // CRITICAL: Small delay between writes
    vTaskDelay(pdMS_TO_TICKS(5));
    
    // Write recovery info
    size_t recovery_size = sizeof(MIFARE_RecoveryInfo_t);
    memcpy(block_data, &card_data->recovery_info, recovery_size);
    
    // Retry loop for recovery info
    loop_start_tick = xTaskGetTickCount();
    do {
        // Check for external abort signals
        if (g_transaction_manager.card_removal_abort || 
            g_transaction_manager.card_state == MIFARE_CARD_STATE_ABSENT) {
            MIFARE_LOG("Write aborted - card removed during retry loop");
            result = MIFARE_RESULT_CARD_REMOVED;
            break;
        }

        result = mifare_write_block_safe(MIFARE_BLOCK_RECOVERY_INFO, block_data);
        if (result == MIFARE_RESULT_OK) break;
        if (result == MIFARE_RESULT_CARD_REMOVED) {
            MIFARE_LOG("Card removed during recovery info write");
            break;
        }
        
        // Check local timeout
        // Increased from 1000ms to 2000ms
        if (pdTICKS_TO_MS(xTaskGetTickCount() - loop_start_tick) > 2000) {
             MIFARE_LOG("Write timeout exceeded (2000ms) - persistent I2C errors");
             result = MIFARE_RESULT_TIMEOUT; // Explicitly set result to timeout
             break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (result != MIFARE_RESULT_OK);

    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write recovery info");
        last_card_write_changed = false;
        return result;
    }
    
    mifare_update_write_snapshot(card_data);
    last_card_write_changed = true;
    no_change_log_reported = false;
    
    // Update stability tracking
    g_transaction_manager.last_successful_write_tick = xTaskGetTickCount();
    g_transaction_manager.card_first_lost_tick = 0;
    
    // Advance write cycle counter
    g_transaction_manager.write_cycle_counter = (g_transaction_manager.write_cycle_counter + 1) & 0x01;
    
    MIFARE_LOG("Card data written successfully (%s)", 
               (g_transaction_manager.write_cycle_counter == 0) ? "next: primary" : "next: backup");
    return MIFARE_RESULT_OK;
}

/**
 * @brief Get card state
 * @return MIFARE_CardState_t Current card state
 */
MIFARE_CardState_t MIFARE_GetCardState(void)
{
    return g_transaction_manager.card_state;
}

/**
 * @brief Set card state
 * @param new_state New state to set
 */
void MIFARE_SetCardState(MIFARE_CardState_t new_state)
{
    g_transaction_manager.card_state = new_state;
}

/**
 * @brief Check if card is present based on STATE only (no hardware check)
 * @details This is a lightweight state-based check. For actual hardware verification
 *          of card presence via PN532, use MIFARE_VerifyCardPresence() instead.
 * @return true if card state indicates present, false otherwise
 */
bool MIFARE_IsCardPresent(void)
{
    bool is_present = (g_transaction_manager.card_state == MIFARE_CARD_STATE_PRESENT || 
                       g_transaction_manager.card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE);
    
    return is_present;
}

/**
 * @brief Check if card presence is confirmed
 * @return true if card presence is confirmed, false otherwise
 */
bool MIFARE_IsCardPresenceConfirmed(void)
{
    return g_transaction_manager.card_presence_confirmed;
}

/**
 * @brief Update card stability check
 * @details This function checks if the card has been stably detected for the required time
 */
void MIFARE_UpdateStabilityCheck(void)
{
    TickType_t now = xTaskGetTickCount();
    
    // If card is detected but not yet confirmed
    if (g_transaction_manager.card_first_detected_tick != 0 && !g_transaction_manager.card_presence_confirmed) {
        uint32_t duration = pdTICKS_TO_MS(now - g_transaction_manager.card_first_detected_tick);
        if (duration >= MIFARE_STABILITY_TIMEOUT_MS) {
            g_transaction_manager.card_presence_confirmed = true;
            g_transaction_manager.card_confirmed_present_tick = now;
            MIFARE_LOG("Card presence CONFIRMED (stable for %lu ms)", duration);
            
            // Update UI state
            if (!g_transaction_manager.ui_state_card_present) {
                g_transaction_manager.ui_state_card_present = true;
                // UI update logic here if needed, or handled by main loop
            }
        }
    }
}

/**
 * @brief Confirm card is ready after polling cycle
 * @details This function transitions the card state to PRESENT after a polling cycle
 */
void MIFARE_ConfirmReadyAfterPolling(void)
{
    if (g_transaction_manager.card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
        MIFARE_SetCardState(MIFARE_CARD_STATE_PRESENT);
        MIFARE_LOG("Card state transitioned to PRESENT after polling cycle");
    }
}

/*Atomic Transaction Operations ----------------------------------*/

MIFARE_Result_t MIFARE_BeginTransaction(uint32_t amount_ml)
{
    if (g_transaction_manager.transaction_active) {
        MIFARE_LOG("Transaction already active");
        return MIFARE_RESULT_BUSY;
    }

    if (g_transaction_manager.current_card.user_primary.balance_ml < amount_ml) {
        MIFARE_LOG("Insufficient balance");
        return MIFARE_RESULT_INSUFFICIENT_BALANCE;
    }
    
    // Save previous state to revert on failure
    uint8_t prev_state = g_transaction_manager.current_card.user_primary.transaction_state;
    
    g_transaction_manager.current_card.user_primary.transaction_state = TRANSACTION_STATE_STARTED;
    g_transaction_manager.transaction_active = true;
    g_transaction_manager.dispense_start_time = mifare_get_timestamp();
    g_transaction_manager.total_dispensed_this_session = 0;
    g_transaction_manager.last_fast_balance_update_time = 0;
    transaction_start_timestamp = g_transaction_manager.dispense_start_time;
    
    if (pn532_recovery_ready_tick != 0) {
        TickType_t now = xTaskGetTickCount();
        if (now < pn532_recovery_ready_tick) {
            vTaskDelay(pn532_recovery_ready_tick - now);
        }
        pn532_recovery_ready_tick = 0;
    }
    
    MIFARE_Result_t result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
    if (result == MIFARE_RESULT_OK) {
        mifare_transition_state(DISPENSE_STATE_DISPENSING);
        MIFARE_LOG("Transaction started for %u mL", amount_ml);
    } else {
        MIFARE_LOG("Failed to start transaction: %s", MIFARE_GetResultString(result));
        g_transaction_manager.transaction_active = false;
        
        // Revert in-memory state
        g_transaction_manager.current_card.user_primary.transaction_state = prev_state;
        
        // Transition to ERROR state to prevent immediate retry loop and force re-validation
        mifare_transition_state(DISPENSE_STATE_ERROR);
    }
    
    return result;
}



/**
 * @brief Initialize a MIFARE card for a new customer
 * @param initial_balance_ml Initial balance to set on the card in milliliters
 * @param customer_id Unique customer identifier (can be derived from card serial)
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_InitializeNewCustomerCard(uint32_t initial_balance_ml, uint64_t customer_id)
{
    MIFARE_Result_t result;
    PN532_CardInfo_t card_info;
    
    MIFARE_LOG("Initializing new customer card with balance: %u mL, Customer ID: %llu", 
               initial_balance_ml, customer_id);
    
    // First, detect the card
    PN532_Status_t status = PN532_DetectCard(&card_info);
    if (status != PN532_STATUS_CARD_DETECTED) {
        MIFARE_LOG("No card detected for initialization");
        return MIFARE_RESULT_ERROR;
    }
    
    // Verify it's a MIFARE Classic card
    if (card_info.card_type != PN532_CARD_MIFARE_CLASSIC_1K &&
        card_info.card_type != PN532_CARD_MIFARE_CLASSIC_4K) {
        MIFARE_LOG("Unsupported card type for initialization: %d", card_info.card_type);
        return MIFARE_RESULT_ERROR;
    }
    
    // Store card info with size validation
    if (sizeof(PN532_CardInfo_t) <= sizeof(g_transaction_manager.card_info)) {
        memcpy(&g_transaction_manager.card_info, &card_info, sizeof(PN532_CardInfo_t));
    } else {
        MIFARE_LOG("ERROR: Card info structure size mismatch during initialization");
        return MIFARE_RESULT_ERROR;
    }
    
    // Initialize card data structure
    MIFARE_CardData_t *card_data = &g_transaction_manager.current_card;
    memset(card_data, 0, sizeof(MIFARE_CardData_t));
    
    uint32_t current_time = mifare_get_timestamp();
    
    // 1. Initialize card header
    card_data->header.magic_bytes = MIFARE_MAGIC_BYTES;
    card_data->header.format_version = MIFARE_FORMAT_VERSION;
    card_data->header.card_type = card_info.card_type;
    card_data->header.card_serial = customer_id;
    card_data->header.header_crc = 0; // Ensure 0 for CRC calculation
    card_data->header.header_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->header, 
                                                          sizeof(MIFARE_CardHeader_t));
    
    // 2. Initialize primary user data (balance, status, state)
    card_data->user_primary.balance_ml = initial_balance_ml;
    card_data->user_primary.last_topup_amount_ml = (uint16_t)(initial_balance_ml > 65535 ? 65535 : initial_balance_ml);
    card_data->user_primary.transaction_counter = 0;
    card_data->user_primary.status_flags = CARD_STATUS_ACTIVE;
    card_data->user_primary.transaction_state = TRANSACTION_STATE_IDLE;
    memset(card_data->user_primary.reserved, 0, sizeof(card_data->user_primary.reserved));
    
    // 3. Initialize usage data (lifetime statistics)
    card_data->usage_data.total_purchased_ml = initial_balance_ml;
    card_data->usage_data.total_dispensed_ml = 0;
    card_data->usage_data.reserved1 = 0;
    card_data->usage_data.reserved2 = 0;
    
    // 4. Initialize backup user data (identical to primary) with size validation
    if (sizeof(MIFARE_UserData_t) <= sizeof(card_data->user_backup)) {
        memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    } else {
        MIFARE_LOG("ERROR: User data structure size mismatch during initialization");
        return MIFARE_RESULT_ERROR;
    }
    
    // 5. Initialize transaction log
    memset(&card_data->transaction_log, 0, sizeof(MIFARE_TransactionLog_t));
    card_data->transaction_log.head_index = 0;
    card_data->transaction_log.count = 0;
    card_data->transaction_log.log_crc = MIFARE_CALCULATE_CRC16(
        (uint8_t*)card_data->transaction_log.records,
        sizeof(MIFARE_TransactionRecord_t) * MIFARE_MAX_TRANSACTIONS);
    
    // 6. Initialize recovery information
    card_data->recovery_info.last_update_time = current_time;
    card_data->recovery_info.primary_data_crc = MIFARE_CALCULATE_CRC16(
        (uint8_t*)&card_data->user_primary, sizeof(MIFARE_UserData_t));
    card_data->recovery_info.backup_data_crc = MIFARE_CALCULATE_CRC16(
        (uint8_t*)&card_data->user_backup, sizeof(MIFARE_UserData_t));
    card_data->recovery_info.sequence_number = 0;
    card_data->recovery_info.recovery_attempts = 0;
    card_data->recovery_info.integrity_flags = 0x0000; // All OK
    
    // 5.5. Initialize fast balance cache
    memset(&card_data->fast_balance_primary, 0, sizeof(MIFARE_FastBalance_t));
    mifare_fast_balance_update(&card_data->fast_balance_primary, initial_balance_ml);
    
    // Copy primary to backup
    memcpy(&card_data->fast_balance_backup, &card_data->fast_balance_primary, sizeof(MIFARE_FastBalance_t));
    
    // 6. Mark data as valid
    card_data->data_valid = true;
    card_data->last_read_time = current_time;
    
    // 7. Write the initialized data to the card
    MIFARE_LOG("Writing initial data structure to card...");
    
    // Write header block (authentication handled internally)
    result = mifare_write_block_safe(MIFARE_BLOCK_HEADER, (uint8_t*)&card_data->header);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write header block");
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // Write primary user data block
    result = mifare_write_block_safe(MIFARE_BLOCK_USER_PRIMARY, (uint8_t*)&card_data->user_primary);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write primary data block");
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // Write backup user data block
    result = mifare_write_block_safe(MIFARE_BLOCK_USER_BACKUP, (uint8_t*)&card_data->user_backup);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write backup data block");
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // Write usage data block
    result = mifare_write_block_safe(MIFARE_BLOCK_USAGE_DATA, (uint8_t*)&card_data->usage_data);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write usage data block");
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // Write transaction log (may span multiple blocks)
    uint8_t *log_data = (uint8_t*)&card_data->transaction_log;
    size_t log_total_size = sizeof(MIFARE_TransactionLog_t);
    uint8_t blocks_needed = (log_total_size + 15) / 16;
    
    uint8_t block_num = MIFARE_BLOCK_TRANSACTION_LOG;
    
    for (uint8_t i = 0; i < blocks_needed; i++) {
        if ((block_num % 4) == 3) {
            block_num++;
        }
        
        result = mifare_write_block_safe(block_num, &log_data[i * 16]);
        if (result != MIFARE_RESULT_OK) {
            MIFARE_LOG("Failed to write transaction log block %d", i);
            return result;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
        
        block_num++;
    }
    
    // Write recovery information block
    result = mifare_write_block_safe(MIFARE_BLOCK_RECOVERY_INFO, (uint8_t*)&card_data->recovery_info);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write recovery info block");
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    // FIX: Explicitly write Sector 3 Trailer (Block 15) to ensure correct access bits
    uint8_t default_trailer[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Key A
        0xFF, 0x07, 0x80, 0x69,             // Access Bits
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF  // Key B
    };
    
    MIFARE_LOG("Initializing Sector 3 Trailer (Block 15)...");
    result = mifare_authenticate_block(15);
    if (result == MIFARE_RESULT_OK) {
        PN532_Status_t status = PN532_MifareWriteBlock(15, default_trailer);
        if (status != PN532_STATUS_OK) {
            MIFARE_LOG("Failed to write Sector 3 Trailer");
        }
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    // Write fast balance cache
    result = mifare_write_block_safe(MIFARE_BLOCK_FAST_BALANCE_PRIMARY, (uint8_t*)&card_data->fast_balance_primary);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write fast balance primary block");
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    
    result = mifare_write_block_safe(MIFARE_BLOCK_FAST_BALANCE_BACKUP, (uint8_t*)&card_data->fast_balance_backup);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to write fast balance backup block");
        return result;
    }
    
    // Verify
    MIFARE_LOG("Verifying written data...");
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow card to stabilize after heavy writing
    MIFARE_CardData_t verify_data;
    result = MIFARE_ReadCardData(&verify_data);
    if (result != MIFARE_RESULT_OK) {
        MIFARE_LOG("Failed to read back initialized data");
        return result;
    }
    
    if (verify_data.header.magic_bytes != MIFARE_MAGIC_BYTES ||
        verify_data.header.card_serial != customer_id ||
        verify_data.user_primary.balance_ml != initial_balance_ml) {
        MIFARE_LOG("Data verification failed after initialization");
        return MIFARE_RESULT_DATA_MISMATCH;
    }

    MIFARE_LogTransaction(1, initial_balance_ml, 0xFF);
    
    last_authenticated_sector = -1;
    mifare_transition_state(DISPENSE_STATE_CARD_DETECTED);
    g_transaction_manager.last_card_update_time = current_time;
    MIFARE_SetCardState(MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE);
    
    MIFARE_LOG("Card initialization completed successfully");
    return MIFARE_RESULT_OK;
}

MIFARE_Result_t MIFARE_TopupCardBalance(uint32_t topup_amount_ml)
{
    MIFARE_Result_t result;
    
    if (topup_amount_ml == 0) {
        return MIFARE_RESULT_ERROR;
    }
    
    if (MIFARE_GetCardState() != MIFARE_CARD_STATE_PRESENT || !g_transaction_manager.current_card.data_valid) {
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    MIFARE_CardData_t *card_data = &g_transaction_manager.current_card;
    uint32_t old_balance = card_data->user_primary.balance_ml;
    uint32_t new_balance = old_balance + topup_amount_ml;
    
    if (new_balance < old_balance) {
        return MIFARE_RESULT_ERROR;
    }
    
    result = MIFARE_BeginTransaction(0);
    if (result != MIFARE_RESULT_OK) {
        return result;
    }
    
    card_data->user_primary.balance_ml = new_balance;
    card_data->user_primary.last_topup_amount_ml = (uint16_t)(topup_amount_ml > 65535 ? 65535 : topup_amount_ml);
    card_data->user_primary.transaction_counter++;
    card_data->usage_data.total_purchased_ml += topup_amount_ml;
    
    if (sizeof(MIFARE_UserData_t) <= sizeof(card_data->user_backup)) {
        memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    }
    
    uint32_t current_time = mifare_get_timestamp();
    card_data->recovery_info.last_update_time = current_time;
    card_data->recovery_info.primary_data_crc = MIFARE_CALCULATE_CRC16(
        (uint8_t*)&card_data->user_primary, sizeof(MIFARE_UserData_t));
    card_data->recovery_info.backup_data_crc = MIFARE_CALCULATE_CRC16(
        (uint8_t*)&card_data->user_backup, sizeof(MIFARE_UserData_t));
    card_data->recovery_info.sequence_number++;
    
    result = MIFARE_CommitTransaction();
    if (result != MIFARE_RESULT_OK) {
        return result;
    }
    
    MIFARE_LogTransaction(1, (uint16_t)(topup_amount_ml > 65535 ? 65535 : topup_amount_ml), 0xFF);
    
    return MIFARE_RESULT_OK;
}

MIFARE_Result_t MIFARE_ValidateCardData(MIFARE_CardData_t *card_data)
{
    if (card_data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    if (card_data->header.magic_bytes != MIFARE_MAGIC_BYTES) {
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    if (card_data->header.format_version != MIFARE_FORMAT_VERSION) {
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    // Save read CRC
    uint16_t read_crc = card_data->header.header_crc;
    // Zero CRC field for calculation
    card_data->header.header_crc = 0;
    
    uint16_t calculated_header_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->header, 
                                                          sizeof(MIFARE_CardHeader_t));
    
    // Restore CRC field
    card_data->header.header_crc = read_crc;
    
    if (read_crc != calculated_header_crc) {
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    uint16_t calculated_primary_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_primary, 
                                                              sizeof(MIFARE_UserData_t));
    uint16_t calculated_backup_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_backup, 
                                                             sizeof(MIFARE_UserData_t));
    
    bool primary_valid = (card_data->recovery_info.primary_data_crc == calculated_primary_crc);
    bool backup_valid = (card_data->recovery_info.backup_data_crc == calculated_backup_crc);
    
    if (primary_valid && backup_valid) {
        return MIFARE_RESULT_OK;
    } else if (primary_valid || backup_valid) {
        return MIFARE_RESULT_DATA_MISMATCH;
    } else {
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    card_data->data_valid = true;
    return MIFARE_RESULT_OK;
}

MIFARE_Result_t MIFARE_RecoverCardData(MIFARE_CardData_t *card_data)
{
    if (card_data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    MIFARE_LOG("RecoverCardData: Attempting to recover card data");
    
    uint16_t calculated_primary_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_primary, 
                                                             sizeof(MIFARE_UserData_t));
    uint16_t calculated_backup_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->user_backup, 
                                                             sizeof(MIFARE_UserData_t));
    
    bool primary_valid = (card_data->recovery_info.primary_data_crc == calculated_primary_crc);
    bool backup_valid = (card_data->recovery_info.backup_data_crc == calculated_backup_crc);
    
    MIFARE_UserData_t *good_data = NULL;
    uint16_t good_crc = 0;
    
    if (primary_valid) {
        good_data = &card_data->user_primary;
        good_crc = calculated_primary_crc;
    } else if (backup_valid) {
        good_data = &card_data->user_backup;
        good_crc = calculated_backup_crc;
    } else {
        card_data->data_valid = false;
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    memcpy(&card_data->user_primary, good_data, sizeof(MIFARE_UserData_t));
    memcpy(&card_data->user_backup, good_data, sizeof(MIFARE_UserData_t));
    
    card_data->recovery_info.primary_data_crc = good_crc;
    card_data->recovery_info.backup_data_crc = good_crc;
    card_data->recovery_info.recovery_attempts++;
    
    card_data->data_valid = true;
    
    MIFARE_Result_t result;
    uint8_t block_data[16];
    
    memset(block_data, 0, sizeof(block_data));
    memcpy(block_data, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    result = mifare_write_block_safe(MIFARE_BLOCK_USER_PRIMARY, block_data);
    if (result != MIFARE_RESULT_OK) return result;
    
    vTaskDelay(pdMS_TO_TICKS(15));
    
    memset(block_data, 0, sizeof(block_data));
    memcpy(block_data, &card_data->user_backup, sizeof(MIFARE_UserData_t));
    result = mifare_write_block_safe(MIFARE_BLOCK_USER_BACKUP, block_data);
    if (result != MIFARE_RESULT_OK) return result;
    
    vTaskDelay(pdMS_TO_TICKS(15));
    
    memset(block_data, 0, sizeof(block_data));
    memcpy(block_data, &card_data->recovery_info, sizeof(MIFARE_RecoveryInfo_t));
    result = mifare_write_block_safe(MIFARE_BLOCK_RECOVERY_INFO, block_data);
    if (result != MIFARE_RESULT_OK) return result;
    
    bool phone_is_empty = mifare_is_account_data_empty(&card_data->account_data);
    
    if (phone_is_empty) {
        mifare_encode_account_data(&card_data->account_data, "07970242024", CARD_VALIDITY_NORMAL);
        result = mifare_write_block_safe(MIFARE_BLOCK_ACCOUNT_DATA, card_data->account_data.raw_data);
    }
    
    return MIFARE_RESULT_OK;
}

/* Data Integrity Functions -------------------------------------------------*/

uint16_t mifare_calculate_crc16(uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

MIFARE_Result_t MIFARE_DetectAndAutoInitializeCard(const PN532_CardInfo_t *card_info, uint32_t default_balance_ml)
{
    PN532_Status_t status;
    
    // Check if card is blank (default keys)
    // Try to authenticate with default key (FF FF FF FF FF FF)
    uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    // Try to authenticate block MIFARE_BLOCK_HEADER (header)
    status = PN532_MifareAuthenticate( 
                                      MIFARE_BLOCK_HEADER, 
                                      (uint8_t*)card_info->uid, 
                                      card_info->uid_length, 
                                      default_key);
                                          
    if (status == PN532_STATUS_OK) {
        // Authenticated successfully. Now check if it's already initialized.
        uint8_t header_data[16];
        status = PN532_MifareReadBlock(MIFARE_BLOCK_HEADER, header_data);
        
        if (status == PN532_STATUS_OK) {
            // Check for Magic Bytes (Little Endian)
            uint32_t magic = (header_data[0]) | (header_data[1] << 8) | (header_data[2] << 16) | (header_data[3] << 24);
            
            if (magic == MIFARE_MAGIC_BYTES) {
                MIFARE_LOG("Existing initialized card detected. Loading data...");
                return MIFARE_ReadCardData(&g_transaction_manager.current_card);
            }
        } else {
            // Read failed - do NOT assume it's blank. It might be a communication error.
            // If we proceed, we might overwrite a valid card.
            MIFARE_LOG("Failed to read block %d (status 0x%02X). Aborting auto-init to protect card data.", MIFARE_BLOCK_HEADER, status);
            return MIFARE_RESULT_ERROR;
        }

        // Card accepts default key AND read succeeded AND magic bytes didn't match.
        // This is likely a blank card (or at least one not formatted by us).
        MIFARE_LOG("Blank/Unformatted card detected (Magic Bytes mismatch), initializing...");
        
        // Initialize with default values
        // Customer ID derived from UID
        uint64_t customer_id = 0;
        for (int i = 0; i < card_info->uid_length; i++) {
            customer_id = (customer_id << 8) | card_info->uid[i];
        }
        
        return MIFARE_InitializeNewCustomerCard(default_balance_ml, customer_id);
    }
    
    return MIFARE_RESULT_ERROR;
}

/**
 * @brief Get current dispense state
 * @return MIFARE_DispenseState_t Current dispense state
 */
MIFARE_DispenseState_t MIFARE_GetDispenseState(void)
{
    return g_transaction_manager.dispense_state;
}

/**
 * @brief Get current balance in milliliters
 * @return uint32_t Current balance (in mL)
 */
uint32_t MIFARE_GetBalanceML(void)
{
    return g_transaction_manager.current_card.user_primary.balance_ml;
}

/**
 * @brief Get last top-up amount in milliliters
 * @return uint32_t Last top-up amount (in mL)
 */
uint32_t MIFARE_GetLastTopupAmountML(void)
{
    return g_transaction_manager.current_card.user_primary.last_topup_amount_ml;
}

/**
 * @brief Get total dispensed amount in the current session
 * @return uint32_t Total dispensed amount (in mL)
 */
uint32_t MIFARE_GetTotalDispensedThisSession(void)
{
    return g_transaction_manager.total_dispensed_this_session;
}

/**
 * @brief Get card status for UI (returns true if card present and ready)
 * @return bool True if card is present and ready to dispense
 */
bool MIFARE_GetCardStatus(void)
{
    return (g_transaction_manager.dispense_state == DISPENSE_STATE_READY_TO_DISPENSE ||
            g_transaction_manager.dispense_state == DISPENSE_STATE_DISPENSING);
}

/**
 * @brief Get card status flags byte
 * @return uint8_t Card status flags (see CARD_STATUS_* defines)
 */
uint8_t MIFARE_GetCardStatusFlags(void)
{
    return g_transaction_manager.current_card.user_primary.status_flags;
}

/**
 * @brief Get total purchased amount (lifetime)
 * @return uint32_t Total purchased in mL
 */
uint32_t MIFARE_GetTotalPurchasedML(void)
{
    return g_transaction_manager.current_card.usage_data.total_purchased_ml;
}

/**
 * @brief Get total dispensed amount (lifetime)
 * @return uint32_t Total dispensed in mL
 */
uint32_t MIFARE_GetTotalDispensedML(void)
{
    return g_transaction_manager.current_card.usage_data.total_dispensed_ml;
}

void MIFARE_LogTransaction(uint8_t type, uint16_t amount_ml, uint8_t dispenser_id)
{
    MIFARE_CardData_t *card_data = &g_transaction_manager.current_card;
    MIFARE_TransactionLog_t *log = &card_data->transaction_log;
    
    // Update log
    uint8_t index = log->head_index;
    log->records[index].timestamp = mifare_get_timestamp();
    log->records[index].amount_ml = amount_ml;
    log->records[index].transaction_type = type;
    log->records[index].dispenser_id = dispenser_id;
    
    log->head_index = (index + 1) % MIFARE_MAX_TRANSACTIONS;
    if (log->count < MIFARE_MAX_TRANSACTIONS) {
        log->count++;
    }
    
    // Update CRC
    log->log_crc = MIFARE_CALCULATE_CRC16((uint8_t*)log->records, 
                                          sizeof(MIFARE_TransactionRecord_t) * MIFARE_MAX_TRANSACTIONS);
    
    MIFARE_LOG("Transaction logged: Type=%d, Amount=%u mL", type, amount_ml);
}


/*Card Polling Task ----------------------------------------------*/
static TaskHandle_t mifare_polling_task_handle = NULL;

/**
 * @brief MIFARE card polling task
 * @details Continuously polls for card presence/removal
 */
static void MIFARE_Polling_Task(void* argument)
{
    (void)argument;
    
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t periodTicks = pdMS_TO_TICKS(100);  // Poll every 100ms
    
    MIFARE_LOG("MIFARE polling task started");
    
    for (;;)
    {
        vTaskDelayUntil(&lastWake, periodTicks);
        
        // Get current states
        MIFARE_CardState_t current_card_state = MIFARE_GetCardState();
        MIFARE_DispenseState_t current_dispense_state = MIFARE_GetDispenseState();
        
        // Skip polling during active transaction (performance optimization)
        if (g_transaction_manager.transaction_active) {
            continue;
        }
        
        // Poll for card
        PN532_CardInfo_t card_info;
        PN532_Status_t status = PN532_DetectCard(&card_info);
        
        if (status == PN532_STATUS_CARD_DETECTED) {
            // Card detected
            if (current_card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
                USB_Log_Printf("[SYSTEM POLL] Card confirmed after polling cycle, setting to PRESENT\r\n");
                MIFARE_ConfirmReadyAfterPolling();
            } else if (current_card_state == MIFARE_CARD_STATE_PRESENT) {
                USB_Log_Printf("[SYSTEM POLL] Card still PRESENT (idle polling)\r\n");
            } else {
                // New card detected
                if (current_dispense_state == DISPENSE_STATE_ERROR) {
                    USB_Log_Printf("SYSTEM: Card detected in error state, attempting recovery\r\n");
                } else {
                    USB_Log_Printf("SYSTEM: New card detected, notifying MIFARE manager\r\n");
                }
                MIFARE_ProcessCardDetected(&card_info);
            }
        } else {
            // No card detected
            if (current_card_state == MIFARE_CARD_STATE_PRESENT) {
                USB_Log_Printf("[SYSTEM POLL] Card REMOVED (detected by polling)\r\n");
                MIFARE_ProcessCardRemoved();
            } else if (current_card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
                USB_Log_Printf("MIFARE POLL: Card in NEEDS_POLLING_CYCLE but not detected - resetting to ABSENT\r\n");
                MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
            } else if (current_card_state == MIFARE_CARD_STATE_ERROR && status != PN532_STATUS_CARD_DETECTED) {
                // Error state but no card - reset after delay
                static uint32_t error_no_card_counter = 0;
                error_no_card_counter++;
                if (error_no_card_counter >= 10) {
                    USB_Log_Printf("MIFARE POLL: No card in error state, resetting to ABSENT\r\n");
                    MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
                    error_no_card_counter = 0;
                }
            }
        }
        
        // Update stability checks
        MIFARE_UpdateStabilityCheck();
    }
}

/**
 * @brief Start the MIFARE card polling task
 */
void MIFARE_StartPollingTask(void)
{
    BaseType_t result = xTaskCreate(
        MIFARE_Polling_Task,
        "MIFARE_Poll",
        2048,  // Stack size in words
        NULL,
        (tskIDLE_PRIORITY + 2),  // Same priority as System and Dispenser
        &mifare_polling_task_handle
    );
    
    if (result == pdPASS) {
        MIFARE_LOG("MIFARE polling task created successfully");
    } else {
        MIFARE_LOG("ERROR: Failed to create MIFARE polling task");
    }
}

