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

/* Includes ------------------------------------------------------------------*/
#include "MIFARE_Transaction_Manager.h"
#include "MIFARE_Classic_Driver.h"
#include "MIFARE_Security.h"
#include "System_Config.h"
#include "PN532_Driver.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "System.h"
#include "SD_Logger_Task.h"
#include "SHA256_Crypto.h"
#include "Dispenser_Controller.h"
#include "task_stack_config.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "ui.h"
#include "ui_Screen1.h"
#ifdef PICO_BOARD
#include "pico/unique_id.h"
#endif

/*Private defines ---------------------------------------------------*/
#define PN532_POST_RESET_COOLDOWN_MS    1200

/* Legacy macros - map to driver definitions */
#define MIFARE_BLOCKS_PER_SECTOR        MIFARE_CLASSIC_BLOCKS_PER_SECTOR
#define MIFARE_IS_SECTOR_TRAILER(block) MIFARE_CLASSIC_IS_SECTOR_TRAILER(block)
#define MIFARE_GET_SECTOR(block)        MIFARE_CLASSIC_GET_SECTOR(block)

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_MIFARE_TRANSACTION_MANAGER_EN      0  // Disabled for production (set to 1 for debugging)
#define LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER_EN   1
#define LOG_ERROR_MIFARE_TRANSACTION_MANAGER_EN      1

/* Feature Configuration -----------------------------------------------------*/
#define MIFARE_ERROR_STATE_AUTO_RECOVERY_EN          0  // 0=Stay in ERROR until card removed, 1=Auto-retry

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

/*Private variables -------------------------------------------------*/
static MIFARE_TransactionManager_t g_transaction_manager;
static int8_t last_authenticated_sector = -1;  // Cache last authenticated sector (-1 = none)
static uint8_t derived_sector_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // Derived per-card sector key
static bool use_factory_keys_for_writes = false;  // Temporary override for blank card initialization
static bool last_card_write_changed = false;
static bool no_change_log_reported = false;
static bool skip_sd_logging = false;  // Flag to skip SD logging during verification reads

/*Private function prototypes ---------------------------------------*/
static MIFARE_Result_t mifare_read_block(uint8_t block_number, uint8_t *data);
static MIFARE_Result_t mifare_write_block(uint8_t block_number, uint8_t *data, bool allow_trailer_write);
static void mifare_transition_state(MIFARE_TransactionState_t new_state);
static MIFARE_Result_t MIFARE_HandleCardRemovalDuringTransaction(void);

static void mifare_clear_write_snapshot(void);
static bool mifare_is_block_encrypted(uint8_t block_number);
static void mifare_update_write_snapshot(const MIFARE_CardData_t *card_data);
static bool mifare_card_data_changed(const MIFARE_CardData_t *card_data);
static void mifare_encode_account_data(MIFARE_AccountData_t *account_data, const char *phone_str, MIFARE_CardValidity_t validity);
static bool mifare_is_account_data_empty(const MIFARE_AccountData_t *account_data);

/*Utility Functions ---------------------------------------------*/

/**
 * @brief Check if a block should be encrypted based on config
 */
static bool mifare_is_block_encrypted(uint8_t block_num)
{
    const SystemConfig_t *config = Config_Get();
    
    if (!config->mifare.security.encryption_enabled) {
        return false;
    }
    
    // Header block is never encrypted - needed for card detection
    if (block_num == MIFARE_BLOCK_HEADER) {
        return false;
    }
    
    // User data blocks (primary and backup)
    if ((block_num == MIFARE_BLOCK_USER_PRIMARY || block_num == MIFARE_BLOCK_USER_BACKUP) && config->mifare.security.encrypt_user_data) {
        return true;
    }
    
    // Transaction log blocks
    if ((block_num == MIFARE_BLOCK_TRANSACTION_LOG || block_num == MIFARE_BLOCK_TRANSACTION_LOG + 1) && config->mifare.security.encrypt_transactions) {
        return true;
    }
    
    // Account data block
    if (block_num == MIFARE_BLOCK_ACCOUNT_DATA && config->mifare.security.encrypt_account_data) {
        return true;
    }
    
    return false;
}

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
 * @brief Get string representation of transaction state
 * @param state Transaction state
 * @return const char* State string
 */
const char* MIFARE_GetTransactionStateString(MIFARE_TransactionState_t state)
{
    switch (state) {
        case TRANSACTION_STATE_IDLE:               return "Idle";
        case TRANSACTION_STATE_CARD_DETECTED:      return "Card Detected";
        case TRANSACTION_STATE_AUTHENTICATING:     return "Authenticating";
        case TRANSACTION_STATE_READING_DATA:       return "Reading Data";
        case TRANSACTION_STATE_VALIDATING:         return "Validating";
        case TRANSACTION_STATE_READY:              return "Ready";
        case TRANSACTION_STATE_READY_AFTER_TOPUP:  return "Ready After Topup";
        case TRANSACTION_STATE_INITIALIZED:        return "Initialized";
        case TRANSACTION_STATE_WRITING_DATA:       return "Writing Data";
        case TRANSACTION_STATE_FINALIZING:         return "Finalizing";
        case TRANSACTION_STATE_ERROR:              return "Error";
        case TRANSACTION_STATE_ERROR_NO_FLOW:      return "Error - No Flow";
        case TRANSACTION_STATE_ERROR_CARD_CORRUPTED: return "Error - Card Corrupted";
        case TRANSACTION_STATE_ERROR_MODULE_FAILURE: return "Error - Module Failure";
        case TRANSACTION_STATE_ERROR_VALIDATION_FAILED: return "Error - Validation";
        case TRANSACTION_STATE_ERROR_WRITE_FAILED: return "Error - Write Failed";
        case TRANSACTION_STATE_CARD_REMOVED:       return "Card Removed";
        case TRANSACTION_STATE_WAITING_FOR_REMOVAL: return "Waiting for Removal";
        default:                                   return "Unknown";
    }
}

/**
 * @brief Get string representation of card state
 * @param state Card state
 * @return const char* State string
 */
const char* MIFARE_GetCardStateString(MIFARE_CardState_t state)
{
    switch (state) {
        case MIFARE_CARD_STATE_ABSENT:             return "Absent";
        case MIFARE_CARD_STATE_PRESENT:            return "Present";
        case MIFARE_CARD_STATE_INITIALIZING:       return "Initializing";
        case MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE: return "Needs Polling Cycle";
        case MIFARE_CARD_STATE_ERROR:              return "Error";
        default:                                   return "Unknown";
    }
}

/* Forward declarations for state transition helpers */
void MIFARE_SetErrorState_CardCorrupted(void);
void MIFARE_SetErrorState_ModuleFailure(void);
void MIFARE_SetErrorState_ValidationFailed(void);
void MIFARE_SetErrorState_WriteFailed(void);

/**
 * @brief Fully re-initializes the PN532 driver to recover from a bad state.
 * @details This function is a critical recovery mechanism. When communication with the
 *          PN532 is lost or corrupted (e.g., due to rapid card removal), this
 *          function resets the driver, preparing it for fresh operations.
 */
void mifare_recover_and_reinit_pn532(void) {
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: CRITICAL: PN532 seems to be in a bad state. Re-initializing driver.\r\n");

    // 1. Notify the system about the impending reset
    MIFARE_NotifyPN532Reset();

    // 2. Perform the re-initialization of the PN532 driver
    // This will reset its internal state machine and communication buffers.
    if (PN532_Init(g_transaction_manager.pn532_handle) == PN532_STATUS_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: PN532 driver re-initialized successfully.\r\n");
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ERROR: PN532 driver re-initialization failed.\r\n");
        // If re-initialization fails, we are in a deeper trouble.
        // A system reset might be the only way out, but for now, we log it.
    }

    // 3. Clear any cached authentication state, as it's now invalid.
    last_authenticated_sector = -1;
    
    // 4. CRITICAL: Reset write failure tracking - this was an I2C bus error, NOT card removal
    // If we don't reset this, the next write failure will incorrectly trigger card removal
    g_transaction_manager.write_failure_first_tick = 0;
    g_transaction_manager.consecutive_write_failures = 0;
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write failure tracking reset after PN532 recovery\r\n");
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

    // Get PN532 driver handle
    g_transaction_manager.pn532_handle = PN532_GetHandle(0);
    if (g_transaction_manager.pn532_handle == NULL) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ERROR: Failed to get PN532 handle\r\n");
        return MIFARE_RESULT_ERROR;
    }

    // Create mutex for transaction safety
    g_transaction_manager.transaction_mutex = xSemaphoreCreateMutex();
    if (g_transaction_manager.transaction_mutex == NULL) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ERROR: Failed to create transaction mutex\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    // Initialize state
    g_transaction_manager.transaction_state = TRANSACTION_STATE_IDLE;
    g_transaction_manager.card_state = MIFARE_CARD_STATE_ABSENT;
    g_transaction_manager.transaction_active = false;
    g_transaction_manager.consecutive_errors = 0;
    g_transaction_manager.pn532_recovery_until_tick = 0;
    
    // Initialize stability layer
    g_transaction_manager.last_successful_read_tick = 0;
    g_transaction_manager.last_successful_write_tick = 0;
    g_transaction_manager.card_first_detected_tick = 0;
    g_transaction_manager.card_confirmed_present_tick = 0;
    g_transaction_manager.card_first_lost_tick = 0;
    g_transaction_manager.card_presence_confirmed = false;
    
    mifare_clear_write_snapshot();
    last_card_write_changed = false;
    no_change_log_reported = false;
    
    // Initialize security layer with config from System_Config
    const SystemConfig_t *sys_config = Config_Get();
    MIFARE_SecurityConfig_t sec_config = {
        .encryption_enabled = sys_config->mifare.security.encryption_enabled,
        .hmac_enabled = sys_config->mifare.security.enable_hmac_auth,
        .challenge_response_enabled = sys_config->mifare.security.enable_challenge_response,
        .replay_protection_enabled = sys_config->mifare.security.enable_replay_protection,
        .pbkdf2_iterations = sys_config->mifare.security.pbkdf2_iterations,
        .max_timestamp_drift_sec = sys_config->mifare.security.max_timestamp_drift_sec,
        .failed_challenge_lockout = sys_config->mifare.security.failed_challenge_lockout
    };
    memcpy(sec_config.master_key, sys_config->mifare.security.master_key, 32);
    memcpy(sec_config.hmac_key, sys_config->mifare.security.hmac_key, 32);
    
    // Get platform unique ID
#ifdef PICO_BOARD
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    memcpy(sec_config.device_id, board_id.id, 8);
#else
    memset(sec_config.device_id, 0, 8);
#endif
    
    MIFARE_Security_Status_t sec_status = MIFARE_Security_Init(&sec_config);
    if (sec_status != MIFARE_SEC_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: WARNING: Security layer initialization failed: %s", 
                   MIFARE_Security_GetStatusString(sec_status));
        // Continue anyway - security might be disabled
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction manager initialized successfully\r\n");
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
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card detection deferred - in cooldown period (%lu ms remaining)", remaining_ms);
            last_cooldown_log = now;
        }
        return MIFARE_RESULT_BUSY;
    }
    
    // Take mutex for thread safety
    if (xSemaphoreTake(g_transaction_manager.transaction_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return MIFARE_RESULT_BUSY;
    }
    
    MIFARE_Result_t result = MIFARE_RESULT_OK;
    
    // Copy card info
    memcpy(&g_transaction_manager.card_info, card_info, sizeof(PN532_CardInfo_t));
    MIFARE_SetCardState(MIFARE_CARD_STATE_INITIALIZING);
    g_transaction_manager.consecutive_errors = 0;
    last_authenticated_sector = -1;  // Clear authentication cache for new card
    
    // Derive per-card security keys from UID
    const SystemConfig_t *config = Config_Get();
    MIFARE_SecurityConfig_t sec_config = {
        .encryption_enabled = config->mifare.security.encryption_enabled,
        .hmac_enabled = config->mifare.security.enable_hmac_auth,
        .pbkdf2_iterations = config->mifare.security.pbkdf2_iterations
    };
    memcpy(sec_config.master_key, config->mifare.security.master_key, 32);
    memcpy(sec_config.hmac_key, config->mifare.security.hmac_key, 32);
    
    // Derive per-card sector authentication key (for MIFARE Classic access)
    if (config->mifare.security.use_custom_sector_keys) {
        // Use SHA256(master_key || UID || "SECTOR") to generate unique 6-byte key
        SHA256_CTX sha_ctx;
        uint8_t hash[32];
        SHA256_Init(&sha_ctx);
        SHA256_Update(&sha_ctx, config->mifare.security.master_key, 32);
        SHA256_Update(&sha_ctx, card_info->uid, card_info->uid_length);
        SHA256_Update(&sha_ctx, (const uint8_t*)"SECTOR", 6);
        SHA256_Final(&sha_ctx, hash);
        memcpy(derived_sector_key, hash, 6);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Derived sector key: %02X%02X%02X%02X%02X%02X",
                   derived_sector_key[0], derived_sector_key[1], derived_sector_key[2],
                   derived_sector_key[3], derived_sector_key[4], derived_sector_key[5]);
    } else {
        memset(derived_sector_key, 0xFF, 6);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Using factory default sector keys (0xFF...)\r\n");
    }
    
    MIFARE_Security_Status_t sec_status = MIFARE_Security_DeriveCardKeys(
        &g_transaction_manager.security_context,
        &sec_config,
        card_info->uid,
        card_info->uid_length,
        NULL  // Report task status after this call instead
    );
    
    /* Report task status after key derivation (can take 300ms+) */
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    if (sec_status != MIFARE_SEC_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: WARNING: Failed to derive card security keys: %s",
                   MIFARE_Security_GetStatusString(sec_status));
        // Continue anyway - encryption might be disabled
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card security keys derived (UID: %02X%02X%02X%02X)",
                   card_info->uid[0], card_info->uid[1], card_info->uid[2], card_info->uid[3]);
    }
    
    // Transition to appropriate state
    if (g_transaction_manager.transaction_state == TRANSACTION_STATE_IDLE ||
        g_transaction_manager.transaction_state == TRANSACTION_STATE_ERROR) {
        
        // Clear error state when attempting new card initialization
        if (g_transaction_manager.transaction_state == TRANSACTION_STATE_ERROR) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Clearing error state to retry card initialization\r\n");
            MIFARE_SetCardState(MIFARE_CARD_STATE_INITIALIZING);
            g_transaction_manager.consecutive_errors = 0;
        }
        
        mifare_transition_state(TRANSACTION_STATE_CARD_DETECTED);
        
        // Start stability timer for card presence confirmation
        TickType_t now = xTaskGetTickCount();
        g_transaction_manager.card_first_detected_tick = now;
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card first detected - waiting %lu ms for stability confirmation", 
                   Config_Get()->mifare.stability_timeout_ms);
        
        // Release mutex before calling detect function (it takes its own mutex)
        xSemaphoreGive(g_transaction_manager.transaction_mutex);
        
        // Detect and read existing cards (will NOT initialize blank cards)
        result = MIFARE_DetectAndAutoInitializeCard(&g_transaction_manager.card_info, 1000000);
        
        /* Report task status after card detection (can take 200-500ms) */
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        if (result == MIFARE_RESULT_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card detected and validated - Ready for dispensing\r\n");
            mifare_transition_state(TRANSACTION_STATE_READY);
            
            // Set card state to PRESENT after successful initialization
            MIFARE_CardState_t current_state = MIFARE_GetCardState();
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Setting card state to PRESENT (was %d)", current_state);
            MIFARE_SetCardState(MIFARE_CARD_STATE_PRESENT);
            
            // Log successful card read to console
            LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✓] Card read OK: UID=%02X%02X%02X%02X, Balance=%lu ml\r\n",
                               g_transaction_manager.card_info.uid[0],
                               g_transaction_manager.card_info.uid[1],
                               g_transaction_manager.card_info.uid[2],
                               g_transaction_manager.card_info.uid[3],
                               g_transaction_manager.current_card.user_primary.balance_ml);
            
            // Log card detection to SD card
            SD_Logger_LogEvent("CARD_DETECTED: UID=%02X%02X%02X%02X, Balance=%lu ml",
                               g_transaction_manager.card_info.uid[0],
                               g_transaction_manager.card_info.uid[1],
                               g_transaction_manager.card_info.uid[2],
                               g_transaction_manager.card_info.uid[3],
                               g_transaction_manager.current_card.user_primary.balance_ml);
        } else if (result == MIFARE_RESULT_PN532_CORRUPTED) {
            MIFARE_SetErrorState_ModuleFailure();
            LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("    Context: PN532 communication error\r\n");
        } else if (result == MIFARE_RESULT_CARD_CORRUPTED) {
            MIFARE_SetErrorState_CardCorrupted();
        } else if (result == MIFARE_RESULT_BUSY) {
            // USB command is pending - card detected, waiting for manual command execution
            // Stay in CARD_DETECTED state to prevent re-detection loop
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: USB command pending - staying in CARD_DETECTED state\r\n");
            // No state transition - remain in CARD_DETECTED, USB command handler will execute
        } else {
            MIFARE_SetErrorState_ValidationFailed();
            LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("    Reason: %s\r\n", MIFARE_GetResultString(result));
        }
        return result;
    }
    
    xSemaphoreGive(g_transaction_manager.transaction_mutex);
    return result;
}

/**
 * @brief Force immediate card removal - resets ALL card/transaction state
 * @details Single function that handles all card removal cleanup.
 *          Call directly when card removal is confirmed (via write failures + PN532 poll).
 *          Called by MIFARE_ProcessCardRemoved() after stability check passes.
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_ForceCardRemoval(void)
{
    if (xSemaphoreTake(g_transaction_manager.transaction_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return MIFARE_RESULT_BUSY;
    }

    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✓] Card removed - cleaning up state\r\n");
    
    // Reset card state
    MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
    g_transaction_manager.transaction_state = TRANSACTION_STATE_IDLE;
    g_transaction_manager.transaction_active = false;
    
    // Clear card data
    memset(&g_transaction_manager.current_card, 0, sizeof(MIFARE_CardData_t));
    last_authenticated_sector = -1;
    mifare_clear_write_snapshot();
    last_card_write_changed = false;
    no_change_log_reported = false;
    
    // Reset all tracking
    g_transaction_manager.write_failure_first_tick = 0;
    g_transaction_manager.consecutive_write_failures = 0;
    g_transaction_manager.card_first_detected_tick = 0;
    g_transaction_manager.card_confirmed_present_tick = 0;
    g_transaction_manager.card_first_lost_tick = 0;
    g_transaction_manager.card_presence_confirmed = false;

    xSemaphoreGive(g_transaction_manager.transaction_mutex);
    return MIFARE_RESULT_OK;
}

/**
 * @brief Process card removal event (with stability timeout)
 * @details Called by polling task. Waits for stability period before confirming removal.
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_ProcessCardRemoved(void)
{
    if (xSemaphoreTake(g_transaction_manager.transaction_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ProcessCardRemoved - mutex busy\r\n");
        return MIFARE_RESULT_BUSY;
    }

    TickType_t now = xTaskGetTickCount();
    
    // Start stability timer on first call
    if (g_transaction_manager.card_first_lost_tick == 0) {
        g_transaction_manager.card_first_lost_tick = now;
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ProcessCardRemoved - stability timer started (%lu ms required)\r\n", 
                                              Config_Get()->mifare.removal_stability_ms);
        xSemaphoreGive(g_transaction_manager.transaction_mutex);
        return MIFARE_RESULT_OK;
    }
    
    // Check if card has been absent long enough
    uint32_t time_absent_ms = pdTICKS_TO_MS(now - g_transaction_manager.card_first_lost_tick);
    if (time_absent_ms < Config_Get()->mifare.removal_stability_ms) {
        // Still waiting - no logging to avoid spam
        xSemaphoreGive(g_transaction_manager.transaction_mutex);
        return MIFARE_RESULT_OK;
    }
    
    // Stability check passed - do full cleanup
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ProcessCardRemoved - stability passed (%lu ms), calling ForceCardRemoval\r\n", time_absent_ms);
    xSemaphoreGive(g_transaction_manager.transaction_mutex);
    return MIFARE_ForceCardRemoval();
}

/**
 * @brief Notify transaction manager that PN532 was reset
 * This sets the recovery cooldown to prevent premature write operations
 */
void MIFARE_NotifyPN532Reset(void)
{
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: PN532 recovery cooldown started for %u ms", PN532_POST_RESET_COOLDOWN_MS);
}

/*Card Data Operations ----------------------------------------------*/

/**
 * @brief Read and validate card data with error recovery
 * @param card_data Pointer to card data structure
 * @return MIFARE_Result_t Operation result
 * 
 * @note Uses single-exit pattern for maintainability - all paths converge at 'exit' label
 */
MIFARE_Result_t MIFARE_ReadCardData(MIFARE_CardData_t *card_data)
{
    // Single-exit pattern: declare result at top, use goto exit for all error paths
    MIFARE_Result_t result = MIFARE_RESULT_ERROR;
    uint8_t block_data[16];
    size_t copy_size;
    bool primary_valid = false;
    (void)primary_valid;  // May be used for validation later
    
    // === SECTION 1: Parameter validation ===
    if (card_data == NULL) {
        goto exit;
    }
    
    // === SECTION 2: Verify card presence ===
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Verifying card presence before read...\r\n");
    result = MIFARE_VerifyCardPresence();
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Card presence check FAILED: %s", MIFARE_GetResultString(result));
        goto exit;
    }
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Card presence verified OK\r\n");
    
    // CRITICAL: Presence check resets PN532 auth state - clear cache so driver re-authenticates
    last_authenticated_sector = -1;
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // === SECTION 3: Read and validate header ===
    result = mifare_read_block(MIFARE_BLOCK_HEADER, block_data);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Header read failed: %s", MIFARE_GetResultString(result));
        goto exit;
    }
    
    copy_size = sizeof(MIFARE_CardHeader_t);
    if (copy_size > 16) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ERROR: Header size exceeds block size\r\n");
        result = MIFARE_RESULT_ERROR;
        goto exit;
    }
    memcpy(&card_data->header, block_data, copy_size);
    
    if (card_data->header.magic_bytes != MIFARE_MAGIC_BYTES) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Invalid magic bytes: 0x%08lX", card_data->header.magic_bytes);
        result = MIFARE_RESULT_CARD_CORRUPTED;
        goto exit;
    }
    
    if (card_data->header.format_version != MIFARE_FORMAT_VERSION) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Unsupported format version: %d", card_data->header.format_version);
        result = MIFARE_RESULT_CARD_CORRUPTED;
        goto exit;
    }
    
    // === SECTION 4: Read and validate primary user data ===
    // HMAC validation happens inside mifare_read_block - if it passes, data is valid
    result = mifare_read_block(MIFARE_BLOCK_USER_PRIMARY, block_data);
    if (result == MIFARE_RESULT_OK) {
        copy_size = sizeof(MIFARE_UserData_t);
        if (copy_size > 16) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ERROR: User data size exceeds block size\r\n");
            result = MIFARE_RESULT_ERROR;
            goto exit;
        }
        memcpy(&card_data->user_primary, block_data, copy_size);
        primary_valid = true;
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Primary data read OK (HMAC validated)\r\n");
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Primary read failed: %s", MIFARE_GetResultString(result));
    }
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // === SECTION 5: Read backup if primary failed ===
    if (!primary_valid) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Primary invalid, checking backup...\r\n");
        result = mifare_read_block(MIFARE_BLOCK_USER_BACKUP, block_data);
        
        if (result == MIFARE_RESULT_OK) {
            copy_size = sizeof(MIFARE_UserData_t);
            memcpy(&card_data->user_backup, block_data, copy_size);
            // Copy backup to primary
            memcpy(&card_data->user_primary, &card_data->user_backup, sizeof(MIFARE_UserData_t));
            primary_valid = true;
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Backup data valid - using backup\r\n");
        } else {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Backup also invalid - card corrupted\r\n");
            result = MIFARE_RESULT_CARD_CORRUPTED;
            goto exit;
        }
    } else {
        // Primary is valid - sync backup in memory for consistency
        memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    }
    
    // === SECTION 6: Read usage data (non-critical) ===
    result = mifare_read_block(MIFARE_BLOCK_USAGE_DATA, block_data);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to read usage data block\r\n");
        memset(&card_data->usage_data, 0, sizeof(MIFARE_UsageData_t));
    } else {
        copy_size = sizeof(MIFARE_UsageData_t);
        if (copy_size > 16) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ERROR: Usage data size exceeds block size\r\n");
            result = MIFARE_RESULT_ERROR;
            goto exit;
        }
        memcpy(&card_data->usage_data, block_data, copy_size);
    }
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // === SECTION 7: Read account data (non-critical) ===
    result = mifare_read_block(MIFARE_BLOCK_ACCOUNT_DATA, block_data);
    if (result == MIFARE_RESULT_OK) {
        copy_size = sizeof(MIFARE_AccountData_t);
        if (copy_size <= 16) {
            memcpy(&card_data->account_data, block_data, copy_size);
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Account data read successfully\r\n");
        }
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Account data read failed - will initialize on validation\r\n");
        memset(&card_data->account_data, 0, sizeof(MIFARE_AccountData_t));
    }
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // === SECTION 10: Final validation and recovery ===
    result = MIFARE_ValidateCardData(card_data);
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Validation result: %s", MIFARE_GetResultString(result));
    
    if (result == MIFARE_RESULT_DATA_MISMATCH) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Data mismatch detected - attempting recovery...\r\n");
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        MIFARE_Result_t recovery_result = MIFARE_RecoverCardData(card_data);
        if (recovery_result == MIFARE_RESULT_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Recovery successful - card data restored\r\n");
            result = MIFARE_RESULT_OK;
        } else {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ CARD DATA] Recovery failed: %s", MIFARE_GetResultString(recovery_result));
            result = recovery_result;
            goto exit;
        }
    }
    
    if (result == MIFARE_RESULT_OK) {
        card_data->data_valid = true;
        card_data->last_read_time = (uint32_t)xTaskGetTickCount();
        mifare_update_write_snapshot(card_data);
        last_card_write_changed = false;
        no_change_log_reported = false;
        
        // Fix garbage balance_ml values (from old card format)
        // Invalid if: > 200000 (200 liters max - reasonable for dispenser)
        uint32_t current_balance = card_data->user_primary.balance_ml;

        
        // Fix garbage last_topup_ml values
        // Invalid if: > 200000 (200 liters max) OR < balance_ml (impossible - can't have more than topped up)
        uint32_t current_topup = card_data->user_primary.last_topup_ml;
        bool topup_too_high = (current_topup > 200000);  // > 200 liters
        bool topup_too_low = (current_topup < current_balance);  // Less than current balance
        
        if (topup_too_high || topup_too_low) {
            // Set topup to 50 liters - standard capacity
            uint32_t corrected_topup = 50000;  // 50 liters in ml
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Fixing invalid last_topup_ml: %lu -> %lu ml [too_%s]",
                      current_topup, corrected_topup, topup_too_high ? "high" : "low\r\n");
            card_data->user_primary.last_topup_ml = corrected_topup;
            
            // Update backup to match
            card_data->user_backup.last_topup_ml = corrected_topup;
            
            // Write corrected data back to card (HMAC computed during write)
            MIFARE_Result_t fix_result = MIFARE_WriteCardData(card_data);
            if (fix_result == MIFARE_RESULT_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card data corrected and written successfully\r\n");
            } else {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write corrected card data: %d", fix_result);
            }
        }
        
        // Check if phone number is empty (all zeros) and initialize with default
        bool phone_is_empty = mifare_is_account_data_empty(&card_data->account_data);
        
        if (phone_is_empty) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Phone number empty, initializing with default: 07970242024\r\n");
            
            // Encode default phone number "07970242024" with Normal validity into raw_data
            mifare_encode_account_data(&card_data->account_data, "07970242024", CARD_VALIDITY_NORMAL);
            
            // Debug log
            static char phone_debug[12];
            memcpy(phone_debug, &card_data->account_data.raw_data[ACCOUNT_DATA_PHONE_OFFSET], ACCOUNT_DATA_PHONE_SIZE);
            phone_debug[ACCOUNT_DATA_PHONE_SIZE] = '\0';
            uint16_t crc = card_data->account_data.raw_data[ACCOUNT_DATA_CRC_OFFSET] | 
                          (card_data->account_data.raw_data[ACCOUNT_DATA_CRC_OFFSET + 1] << 8);
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Writing account data: phone=%s, validity=%u, CRC=0x%04X",
                      phone_debug,
                      card_data->account_data.raw_data[ACCOUNT_DATA_VALIDITY_OFFSET],
                      crc);
            
            // Write account data to card
            result = mifare_write_block(MIFARE_BLOCK_ACCOUNT_DATA, 
                                             card_data->account_data.raw_data, false);
            if (result == MIFARE_RESULT_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Default phone number written to card successfully\r\n");
                
                // Verify by reading it back
                uint8_t verify_block[16];
                vTaskDelay(pdMS_TO_TICKS(20)); // Small delay for card to settle
                result = mifare_read_block(MIFARE_BLOCK_ACCOUNT_DATA, verify_block);
                if (result == MIFARE_RESULT_OK) {
                    char phone_verify[12];
                    memcpy(phone_verify, &verify_block[ACCOUNT_DATA_PHONE_OFFSET], ACCOUNT_DATA_PHONE_SIZE);
                    phone_verify[ACCOUNT_DATA_PHONE_SIZE] = '\0';
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Verification read: phone=%s, validity=%u",
                              phone_verify,
                              verify_block[ACCOUNT_DATA_VALIDITY_OFFSET]);
                } else {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to verify account data write\r\n");
                }
            } else {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write default phone number to card\r\n");
            }
        } else {
            // Log existing phone number
            static char phone_existing[12];
            memcpy(phone_existing, &card_data->account_data.raw_data[ACCOUNT_DATA_PHONE_OFFSET], ACCOUNT_DATA_PHONE_SIZE);
            phone_existing[ACCOUNT_DATA_PHONE_SIZE] = '\0';
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Phone number already set: '%s' (validity=%u)", 
                      phone_existing,
                      card_data->account_data.raw_data[ACCOUNT_DATA_VALIDITY_OFFSET]);
            // Debug: dump raw bytes
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Raw phone bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      card_data->account_data.raw_data[0], card_data->account_data.raw_data[1], 
                      card_data->account_data.raw_data[2], card_data->account_data.raw_data[3],
                      card_data->account_data.raw_data[4], card_data->account_data.raw_data[5],
                      card_data->account_data.raw_data[6], card_data->account_data.raw_data[7],
                      card_data->account_data.raw_data[8], card_data->account_data.raw_data[9],
                      card_data->account_data.raw_data[10]);
        }
        
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        // Log card scan to SD card with all MIFARE block data (skip if flag set - e.g. during verification)
        if (SD_Logger_IsReady() && !skip_sd_logging) {
            // Get card UID from transaction manager
            uint8_t uid_length = g_transaction_manager.card_info.uid_length;
            const uint8_t *card_uid = g_transaction_manager.card_info.uid;
            
            if (SD_Logger_LogMIFARECardScan(card_uid, uid_length, card_data, SD_LOG_TYPE_MIFARE_CARD_SCAN)) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card scan logged to SD card successfully\r\n");
            } else {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to log card scan to SD card\r\n");
            }
        } else if (!skip_sd_logging) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: SD Logger not ready - skipping card scan logging\r\n");
        }
        
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        result = MIFARE_RESULT_OK;  // Success path
    }
    
exit:
    // Single exit point - all paths converge here
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
    
    MIFARE_Result_t result = MIFARE_RESULT_OK;
    
    /* NOTE: This function is legacy from liquid dispensing system.
     * For dispenser system, volume deduction happens in Dispenser Integration layer.
     * This function just updates the transaction state on the card. */
    
    g_transaction_manager.current_card.user_primary.transaction_state = CARD_TRANSACTION_DISPENSING;
    
    /* Main Data Update - write primary+backup every MIFARE_MAIN_DATA_UPDATE_MS */
    uint32_t current_time = (uint32_t)xTaskGetTickCount();
    uint32_t time_since_main_update = current_time - g_transaction_manager.last_card_update_time;
    
    if (time_since_main_update >= MIFARE_MAIN_DATA_UPDATE_MS) {
        result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
        if (result == MIFARE_RESULT_OK) {
            g_transaction_manager.last_card_update_time = current_time;
            g_transaction_manager.consecutive_errors = 0;
            if (last_card_write_changed) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card updated: %lu ml remaining", 
                           g_transaction_manager.current_card.user_primary.balance_ml);
            } else if (!no_change_log_reported) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Data unchanged - skipping write\r\n");
                no_change_log_reported = true;
            }
        } else if (result == MIFARE_RESULT_CARD_REMOVED) {
            // Card removed during write - detected immediately
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write failed - card removed\r\n");
            return MIFARE_HandleCardRemovalDuringTransaction();
        } else {
            // Other errors (WRITE_FAILED, etc) - card still present but I2C issues
            g_transaction_manager.consecutive_errors++;
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write failed (error %u/%u): %s", 
                       g_transaction_manager.consecutive_errors,
                       Config_Get()->mifare.card_removal_fail_count,
                       MIFARE_GetResultString(result));
            
            // If multiple consecutive I2C errors, something is seriously wrong
            if (g_transaction_manager.consecutive_errors >= Config_Get()->mifare.card_removal_fail_count) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Excessive write failures - critical I2C error\r\n");
                return MIFARE_HandleCardRemovalDuringTransaction();
            }
        }
    }
    
    return result;
}

/**
 * @brief Commit the transaction (mark as completed)
 * @details Writes COMMIT_READY state, logs transaction, then writes COMMITTED state
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_CommitTransaction(void)
{
    if (!g_transaction_manager.transaction_active) {
        return MIFARE_RESULT_ERROR;
    }
    
    /* Step 1: Write Commit Ready State */
    g_transaction_manager.current_card.user_primary.transaction_state = CARD_TRANSACTION_COMMIT_READY;
    
    MIFARE_Result_t result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write commit ready state\r\n");
        g_transaction_manager.transaction_active = false;
        // Only transition to WAITING_FOR_REMOVAL if card wasn't removed (removal already cleaned up state)
        if (result != MIFARE_RESULT_CARD_REMOVED) {
            mifare_transition_state(TRANSACTION_STATE_WAITING_FOR_REMOVAL);
            LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✗] Transaction commit failed\r\n");
            LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[→] Remove card and re-insert to retry.\r\n");
        }
        return result;
    }
    
    /* Step 2: Log Transaction */
    MIFARE_LogTransaction(2, 0, 0);  // Type 2 = Dispense Completed, volume handled by business logic
    
    /* Step 3: Write Final Commit State */
    g_transaction_manager.current_card.user_primary.transaction_state = CARD_TRANSACTION_COMMITTED;
    result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
    
    if (result == MIFARE_RESULT_OK) {
        g_transaction_manager.transaction_active = false;
        // total_dispensed_this_session removed - handled by business logic
        mifare_transition_state(TRANSACTION_STATE_READY);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction committed successfully - dispensed amount logged\r\n");
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction flag cleared - polling resumed\r\n");
    } else {
        g_transaction_manager.transaction_active = false;
        // Only transition to error if card wasn't removed (removal already cleaned up state)
        if (result != MIFARE_RESULT_CARD_REMOVED) {
            MIFARE_SetErrorState_WriteFailed();
        }
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction flag cleared (after error) - polling resumed\r\n");
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
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: No active transaction to rollback\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    // Perform rollback
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Rolling back transaction - restoring previous balance\r\n");
    
    // NOTE: Balance restoration is handled by business logic (Dispenser Integration)
    // Transaction Manager just resets the card state
    // g_transaction_manager.current_card.user_primary.balance_ml managed by business logic
    
    // Reset transaction state
    g_transaction_manager.current_card.user_primary.transaction_state = CARD_TRANSACTION_IDLE;
    
    // Card is still present - attempt to write rollback to card
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card still present - attempting to write rollback to card\r\n");
    
    // CRITICAL: Wait for any in-progress I2C operations to complete
    // If safety timeout occurred during a write, the I2C semaphore may still be held
    // Give it time to complete and release the semaphore
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Wake PN532 if it entered sleep mode during long dispense operation
    PN532_Status_t pn532_status = PN532_Wakeup(g_transaction_manager.pn532_handle);
    if (pn532_status != PN532_STATUS_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to wake PN532 for rollback, continuing anyway...\r\n");
        // Give it another chance - maybe still recovering from interrupted operation
        vTaskDelay(pdMS_TO_TICKS(100));
        pn532_status = PN532_Wakeup(g_transaction_manager.pn532_handle);
        if (pn532_status != PN532_STATUS_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: PN532 wakeup failed again - I2C may be stuck\r\n");
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
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Rollback write failed (attempt %d/%d), retrying...", retry + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(100));  // Wait before retry
        }
    }
    
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write rollback to card after %d attempts - balance restored in memory only", max_retries);
    }
    
    // Log the rollback transaction
    MIFARE_LogTransaction(3, 0, 0); // Type 3 = Rollback, handled by business logic
    
    // Clear transaction state
    g_transaction_manager.transaction_active = false;
    // total_dispensed_this_session removed - handled by business logic
    
    // Transition to appropriate state
    if (result == MIFARE_RESULT_OK) {
        mifare_transition_state(TRANSACTION_STATE_READY);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction rollback completed successfully\r\n");
    } else {
        MIFARE_SetErrorState_WriteFailed();
    }

    
    return result;
}

/**
 * @brief Handle card removal during transaction - logs event and cleans up state
 * @return MIFARE_RESULT_CARD_REMOVED always
 */
MIFARE_Result_t MIFARE_HandleCardRemovalDuringTransaction(void)
{
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card removed during transaction\r\n");

    // Log to SD card for audit trail
    SD_Logger_LogEvent("CARD_REMOVED: UID=%02X%02X%02X%02X",
                       g_transaction_manager.card_info.uid[0],
                       g_transaction_manager.card_info.uid[1],
                       g_transaction_manager.card_info.uid[2],
                       g_transaction_manager.card_info.uid[3]);
    
    // Clean up all state (ForceCardRemoval takes mutex, we're not holding it here)
    MIFARE_ForceCardRemoval();
    
    return MIFARE_RESULT_CARD_REMOVED;
}

/*Private helper functions --------------------------------------*/

/**
 * @brief Write block with retry logic (handles timeouts and card removal)
 * @param block_number Block number to write
 * @param data Data buffer to write (16 bytes)
 * @param timeout_ms Maximum time to retry before giving up
 * @param block_name Description for logging
 * @return MIFARE_Result_t Operation result
 */
static MIFARE_Result_t mifare_write_block_with_retry(
    uint8_t block_number, 
    uint8_t *data, 
    uint32_t timeout_ms,
    const char *block_name)
{
    TickType_t start_tick = xTaskGetTickCount();
    MIFARE_Result_t result;
    
    do {
        /* Feed WDT at start of each retry iteration */
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        // Check for card removal
        if (g_transaction_manager.card_state == MIFARE_CARD_STATE_ABSENT) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write aborted - card removed during retry loop\r\n");
            return MIFARE_RESULT_CARD_REMOVED;
        }

        result = mifare_write_block(block_number, data, false);
        if (result == MIFARE_RESULT_OK) break;
        if (result == MIFARE_RESULT_CARD_REMOVED) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card removed during %s write", block_name);
            break;
        }
        
        // Check timeout
        uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start_tick);
        if (elapsed_ms > timeout_ms) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write timeout (%lu ms) for %s", elapsed_ms, block_name);
            return MIFARE_RESULT_TIMEOUT;
        }
        
        /* Feed WDT before delay */
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (result != MIFARE_RESULT_OK);
    
    return result;
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
    /* Feed WDT before potentially blocking I2C operation */
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    PN532_CardInfo_t current_card;
    PN532_Status_t status = PN532_DetectCard(g_transaction_manager.pn532_handle, &current_card);
    
    /* Feed WDT after I2C operation */
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    if (status != PN532_STATUS_CARD_DETECTED) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [PRESENCE CHECK] Card not detected (status=%d)", status);
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    // Verify it's the same card with bounds checking
    if (current_card.uid_length != g_transaction_manager.card_info.uid_length ||
        current_card.uid_length > sizeof(current_card.uid) ||
        g_transaction_manager.card_info.uid_length > sizeof(g_transaction_manager.card_info.uid) ||
        memcmp(current_card.uid, g_transaction_manager.card_info.uid, current_card.uid_length) != 0) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [PRESENCE CHECK] Different card detected (UID mismatch)\r\n");
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    // Only log success during critical operations (removed routine OK logging)
    return MIFARE_RESULT_OK;
}

/**
 * @brief Helper to convert driver status to transaction manager result
 */
static MIFARE_Result_t mifare_convert_driver_status(MIFARE_Classic_Status_t status)
{
    switch (status) {
        case MIFARE_CLASSIC_OK:
            return MIFARE_RESULT_OK;
        case MIFARE_CLASSIC_ERROR_AUTHENTICATION_FAILED:
            return MIFARE_RESULT_AUTHENTICATION_FAILED;
        case MIFARE_CLASSIC_ERROR_READ_FAILED:
            return MIFARE_RESULT_ERROR;
        case MIFARE_CLASSIC_ERROR_WRITE_FAILED:
            return MIFARE_RESULT_WRITE_FAILED;
        case MIFARE_CLASSIC_ERROR_SECTOR_TRAILER:
        case MIFARE_CLASSIC_ERROR_MANUFACTURER_BLOCK:
        case MIFARE_CLASSIC_ERROR_INVALID_BLOCK:
        case MIFARE_CLASSIC_ERROR_INVALID_PARAM:
        default:
            return MIFARE_RESULT_ERROR;
    }
}

/**
 * @brief Read MIFARE block and handle decryption/HMAC (business logic)
 * @param block_number Block to read
 * @param data Buffer to store data (16 bytes)
 * @return MIFARE_Result_t Operation result
 */
static MIFARE_Result_t mifare_read_block(uint8_t block_number, uint8_t *data)
{
    if (data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Setup authentication credentials with derived sector key
    MIFARE_Classic_Auth_t auth = {
        .uid_length = g_transaction_manager.card_info.uid_length
    };
    memcpy(auth.key, derived_sector_key, 6);  // Use derived key
    memcpy(auth.uid, g_transaction_manager.card_info.uid, auth.uid_length);
    
    // Debug: show auth cache state before read
    uint8_t target_sector = MIFARE_GET_SECTOR(block_number);
    if (last_authenticated_sector != target_sector) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ BLOCK %d] Will re-auth (cached sector=%d, target=%d)", 
                   block_number, last_authenticated_sector, target_sector);
    }
    
    // Call driver - it handles all validation, authentication, and retries
    // Feed WDT before and after potentially long I2C operations
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    MIFARE_Classic_Status_t driver_status = MIFARE_Classic_ReadBlock(
        g_transaction_manager.pn532_handle,
        block_number,
        &auth,
        &last_authenticated_sector,
        data
    );
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // If derived key fails, try factory key as fallback for all sectors
    // when custom keys are disabled (decryptcard/recovery mode)
    const SystemConfig_t *config = Config_Get();
    bool try_factory_fallback = !config->mifare.security.use_custom_sector_keys;
    
    if (driver_status != MIFARE_CLASSIC_OK && try_factory_fallback) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ BLOCK %d] Derived key failed (0x%02X) - trying factory key fallback", 
                   block_number, driver_status);
        
        // CRITICAL: After failed auth, PN532 requires re-selecting card before trying another key
        last_authenticated_sector = -1;
        vTaskDelay(pdMS_TO_TICKS(50));
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        PN532_CardInfo_t reselect_info;
        PN532_Status_t reselect_status = PN532_ReadPassiveTargetID(
            g_transaction_manager.pn532_handle,
            PN532_CARD_TYPE_106_TYPE_A,
            &reselect_info
        );
        
        if (reselect_status != PN532_STATUS_CARD_DETECTED) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ BLOCK %d] Card re-selection failed for factory key retry", block_number);
            return MIFARE_RESULT_CARD_REMOVED;  // Fast fail - card likely removed
        }
        
        // Now try with factory key
        uint8_t factory_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        memcpy(auth.key, factory_key, 6);
        
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        driver_status = MIFARE_Classic_ReadBlock(
            g_transaction_manager.pn532_handle,
            block_number,
            &auth,
            &last_authenticated_sector,
            data
        );
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        if (driver_status == MIFARE_CLASSIC_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ BLOCK %d] Factory key fallback succeeded", block_number);
            
            // CRITICAL: After factory key fallback succeeds, invalidate auth cache
            // This forces re-auth with derived keys on next operation to a different sector
            // Otherwise PN532 may be in an inconsistent state causing subsequent derived key auths to fail
            last_authenticated_sector = -1;
        } else {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ BLOCK %d] Factory key also failed: 0x%02X", block_number, driver_status);
        }
    } else if (driver_status != MIFARE_CLASSIC_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [READ BLOCK %d] Derived key failed (0x%02X) - no fallback for sector %d", 
                   block_number, driver_status, target_sector);
    }
    
    if (driver_status == MIFARE_CLASSIC_OK) {
        // Successful read - decrypt if needed
        if (mifare_is_block_encrypted(block_number)) {
            // Decrypt the block
            const SystemConfig_t *config = Config_Get();
            MIFARE_SecurityConfig_t sec_config = {
                .encryption_enabled = config->mifare.security.encryption_enabled
            };
            memcpy(sec_config.master_key, config->mifare.security.master_key, 32);
            
            MIFARE_Security_Status_t sec_status = MIFARE_Security_DecryptBlock(
                &g_transaction_manager.security_context,
                &sec_config,
                block_number,
                data
            );
            
            if (sec_status != MIFARE_SEC_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Decryption failed for block %d: %s", 
                           block_number, MIFARE_Security_GetStatusString(sec_status));
                return MIFARE_RESULT_ERROR;
            }
            
            // Verify HMAC if enabled (for encrypted blocks with authentication)
            const SystemConfig_t *config_hmac = Config_Get();
            if (config_hmac->mifare.security.enable_hmac_auth) {
                // Extract HMAC tag from last 4 bytes
                uint8_t hmac_tag[4];
                memcpy(hmac_tag, &data[12], 4);
                
                MIFARE_SecurityConfig_t sec_config_hmac = {
                    .hmac_enabled = config_hmac->mifare.security.enable_hmac_auth
                };
                memcpy(sec_config_hmac.hmac_key, config_hmac->mifare.security.hmac_key, 32);
                
                // Verify HMAC on first 12 bytes (encrypted payload)
                sec_status = MIFARE_Security_VerifyHMAC(
                    &g_transaction_manager.security_context,
                    &sec_config_hmac,
                    block_number,
                    data,
                    hmac_tag
                );
                
                if (sec_status == MIFARE_SEC_ERROR_HMAC_MISMATCH) {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: HMAC verification failed for block %d - data may be tampered", block_number);
                    
                    // For user data blocks (primary/backup), return error so caller can try backup
                    // This handles sudden card removal corruption - backup may still be valid
                    if (block_number == MIFARE_BLOCK_USER_PRIMARY || block_number == MIFARE_BLOCK_USER_BACKUP) {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: User data block HMAC failed - enabling backup fallback\r\n");
                        return MIFARE_RESULT_CARD_CORRUPTED;  // Signals "try backup" in ReadCardData
                    }
                    
                    // For other blocks (header, etc), check auto_reinit policy
                    if (!config_hmac->mifare.auto_reinit_on_corruption) {
                        return MIFARE_RESULT_CARD_CORRUPTED;
                    }
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Auto-reinit enabled - continuing despite HMAC mismatch\r\n");
                } else if (sec_status != MIFARE_SEC_OK) {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: HMAC verification error for block %d: %s",
                               block_number, MIFARE_Security_GetStatusString(sec_status));
                }
            }
        }
        
        return MIFARE_RESULT_OK;
    }
    
    // Driver returned error - convert to transaction manager result
    return mifare_convert_driver_status(driver_status);
}

/**
 * @brief Write MIFARE block with encryption/HMAC handling (business logic)
 * @param block_number Block to write
 * @param data Data to write (16 bytes)
 * @return MIFARE_Result_t Operation result
 */
static MIFARE_Result_t mifare_write_block(uint8_t block_number, uint8_t *data, bool allow_trailer_write)
{
    TickType_t block_write_start = xTaskGetTickCount();  // BENCHMARK: Total block write time
    uint32_t hmac_ms = 0, encrypt_ms = 0, reselect_ms = 0, driver_write_ms = 0;
    
    if (data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Validate block - protect sector trailers unless explicitly allowed
    if (MIFARE_IS_SECTOR_TRAILER(block_number) && !allow_trailer_write) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Blocked attempt to write sector trailer block %d without permission", block_number);
        return MIFARE_RESULT_ERROR;
    }
    
    // Log all write operations
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Writing to block %d (sector %d)", block_number, MIFARE_GET_SECTOR(block_number));
    
    // Encrypt data if needed (before writing)
    uint8_t encrypted_data[16];
    uint8_t *write_data = data;  // Default to original data
    
    if (mifare_is_block_encrypted(block_number)) {
        // Copy data to temp buffer for encryption
        memcpy(encrypted_data, data, 16);
        
        // Add HMAC authentication tag if enabled
        const SystemConfig_t *config = Config_Get();
        if (config->mifare.security.enable_hmac_auth) {
            TickType_t hmac_start = xTaskGetTickCount();  // BENCHMARK: HMAC time
            
            // Calculate HMAC on first 12 bytes (payload)
            uint8_t hmac_tag[4];
            MIFARE_SecurityConfig_t sec_config_hmac = {
                .hmac_enabled = config->mifare.security.enable_hmac_auth
            };
            memcpy(sec_config_hmac.hmac_key, config->mifare.security.hmac_key, 32);
            
            MIFARE_Security_Status_t sec_status = MIFARE_Security_CalculateHMAC(
                &g_transaction_manager.security_context,
                &sec_config_hmac,
                block_number,
                encrypted_data,
                hmac_tag
            );
            
            hmac_ms = pdTICKS_TO_MS(xTaskGetTickCount() - hmac_start);
            
            if (sec_status != MIFARE_SEC_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: HMAC calculation failed for block %d: %s",
                           block_number, MIFARE_Security_GetStatusString(sec_status));
                return MIFARE_RESULT_ERROR;
            }
            
            // Store HMAC in last 4 bytes
            memcpy(&encrypted_data[12], hmac_tag, 4);
        }
        
        // Encrypt the block
        TickType_t encrypt_start = xTaskGetTickCount();  // BENCHMARK: Encryption time
        
        const SystemConfig_t *config_enc = Config_Get();
        MIFARE_SecurityConfig_t sec_config_enc = {
            .encryption_enabled = config_enc->mifare.security.encryption_enabled
        };
        memcpy(sec_config_enc.master_key, config_enc->mifare.security.master_key, 32);
        
        MIFARE_Security_Status_t sec_status = MIFARE_Security_EncryptBlock(
            &g_transaction_manager.security_context,
            &sec_config_enc,
            block_number,
            encrypted_data
        );
        
        encrypt_ms = pdTICKS_TO_MS(xTaskGetTickCount() - encrypt_start);
        
        if (sec_status != MIFARE_SEC_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Encryption failed for block %d: %s",
                       block_number, MIFARE_Security_GetStatusString(sec_status));
            return MIFARE_RESULT_ERROR;
        }
        
        write_data = encrypted_data;  // Use encrypted data
    }
    
    // Setup authentication credentials for driver with derived sector key
    MIFARE_Classic_Auth_t auth = {
        .uid_length = g_transaction_manager.card_info.uid_length
    };
    
    // Use factory keys during initial blank card setup, derived keys otherwise
    if (use_factory_keys_for_writes) {
        uint8_t factory_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        memcpy(auth.key, factory_key, 6);
    } else {
        memcpy(auth.key, derived_sector_key, 6);  // Use derived key
    }
    
    memcpy(auth.uid, g_transaction_manager.card_info.uid, auth.uid_length);
    
    // CRITICAL: If auth cache is invalid (-1), re-select card before write
    // This ensures clean PN532 state after factory key fallback operations
    uint8_t target_sector = MIFARE_GET_SECTOR(block_number);
    if (last_authenticated_sector == -1 || last_authenticated_sector != (int8_t)target_sector) {
        TickType_t reselect_start = xTaskGetTickCount();  // BENCHMARK: Re-selection time
        
        // Re-select to ensure clean state before auth to new sector
        // Short delay to allow PN532 to stabilize (reduced from 20ms to 5ms)
        vTaskDelay(pdMS_TO_TICKS(5));
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        PN532_CardInfo_t reselect_info;
        PN532_Status_t reselect_status = PN532_ReadPassiveTargetID(
            g_transaction_manager.pn532_handle,
            PN532_CARD_TYPE_106_TYPE_A,
            &reselect_info
        );
        
        // Retry once if first attempt fails (PN532 may need recovery time)
        if (reselect_status != PN532_STATUS_CARD_DETECTED) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE] First re-selection failed, retrying after delay...\r\n");
            vTaskDelay(pdMS_TO_TICKS(50));  // Reduced from 100ms to 50ms
            System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
            
            reselect_status = PN532_ReadPassiveTargetID(
                g_transaction_manager.pn532_handle,
                PN532_CARD_TYPE_106_TYPE_A,
                &reselect_info
            );
        }
        
        reselect_ms = pdTICKS_TO_MS(xTaskGetTickCount() - reselect_start);
        
        if (reselect_status != PN532_STATUS_CARD_DETECTED) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE] Card re-selection failed before block %d write (reselect took %lums)", block_number, reselect_ms);
            return MIFARE_RESULT_CARD_REMOVED;
        }
        last_authenticated_sector = -1;  // Force re-auth after re-select
    }
    
    /* Write the block using driver - this can take up to several seconds with retries */
    /* Feed WDT before and after to prevent timeout during long I2C operations */
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    TickType_t driver_start = xTaskGetTickCount();  // BENCHMARK: Driver write time
    
    MIFARE_Classic_Status_t driver_status = MIFARE_Classic_WriteBlock(
        g_transaction_manager.pn532_handle,
        block_number,
        &auth,
        &last_authenticated_sector,
        write_data,
        allow_trailer_write
    );
    
    driver_write_ms = pdTICKS_TO_MS(xTaskGetTickCount() - driver_start);
    
    /* Feed WDT after potentially long write operation */
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // If derived key fails, try factory key as fallback for all sectors
    // when custom keys are disabled (decryptcard/recovery mode)
    const SystemConfig_t *config_write = Config_Get();
    bool try_factory_fallback = !config_write->mifare.security.use_custom_sector_keys;
    
    if (driver_status != MIFARE_CLASSIC_OK && try_factory_fallback) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE] Derived key failed for block %d (sector 4) - trying factory key fallback", block_number);
        
        // CRITICAL: After failed auth, PN532 requires re-selecting card before trying another key
        last_authenticated_sector = -1;
        vTaskDelay(pdMS_TO_TICKS(50));
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        PN532_CardInfo_t reselect_info;
        PN532_Status_t reselect_status = PN532_ReadPassiveTargetID(
            g_transaction_manager.pn532_handle,
            PN532_CARD_TYPE_106_TYPE_A,
            &reselect_info
        );
        
        if (reselect_status != PN532_STATUS_CARD_DETECTED) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE] Card re-selection failed for factory key retry on block %d", block_number);
            return MIFARE_RESULT_CARD_REMOVED;  // Fast fail - card likely removed
        }
        
        // Now try with factory key
        uint8_t factory_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        memcpy(auth.key, factory_key, 6);
        
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        driver_status = MIFARE_Classic_WriteBlock(
            g_transaction_manager.pn532_handle,
            block_number,
            &auth,
            &last_authenticated_sector,
            write_data,
            allow_trailer_write
        );
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        if (driver_status == MIFARE_CLASSIC_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE] Factory key fallback succeeded for block %d", block_number);
        }
    }
    
    if (driver_status != MIFARE_CLASSIC_OK) {
        /* Track consecutive write failures across ALL write attempts (not just this function call) */
        TickType_t now = xTaskGetTickCount();
        
        // Check if we're in an existing failure period or starting a new one
        if (g_transaction_manager.write_failure_first_tick == 0) {
            // First failure - check immediately if card removed
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE FAILURE #1] Checking if card present...\r\n");
            if (MIFARE_VerifyCardPresence() != MIFARE_RESULT_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE FAILURE #1] Card NOT present - returning CARD_REMOVED\r\n");
                return MIFARE_RESULT_CARD_REMOVED;
            }
            // Card present - start tracking
            g_transaction_manager.write_failure_first_tick = now;
            g_transaction_manager.consecutive_write_failures = 1;
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE FAILURE #1] Card still present - starting failure tracking\r\n");
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
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Previous failure period expired, starting new tracking\r\n");
            }
        }
        
        // Check card presence more aggressively - every failure after the first
        uint32_t failure_duration_ms = pdTICKS_TO_MS(now - g_transaction_manager.write_failure_first_tick);
        if (g_transaction_manager.consecutive_write_failures > 1) {
            // Multiple failures - check if card actually removed
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE FAILURE #%d] Checking card presence after %lu ms...", 
                       g_transaction_manager.consecutive_write_failures, failure_duration_ms);
            if (MIFARE_VerifyCardPresence() != MIFARE_RESULT_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE FAILURE #%d] Card REMOVED after %lu ms (count: %d)", 
                           g_transaction_manager.consecutive_write_failures, failure_duration_ms, 
                           g_transaction_manager.consecutive_write_failures);
                return MIFARE_RESULT_CARD_REMOVED;
            }
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [WRITE FAILURE #%d] Card still PRESENT at %lu ms - I2C issue", 
                       g_transaction_manager.consecutive_write_failures, failure_duration_ms);
        }
        
        // Driver already handles retries and authentication, so convert error and return
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write failed with driver status: %s", MIFARE_Classic_GetStatusString(driver_status));
        return mifare_convert_driver_status(driver_status);
    }
    
    // Log benchmark data
    uint32_t total_block_ms = pdTICKS_TO_MS(xTaskGetTickCount() - block_write_start);
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [BENCHMARK] Block %d write: total=%lums, hmac=%lums, encrypt=%lums, reselect=%lums, driver=%lums",
               block_number, total_block_ms, hmac_ms, encrypt_ms, reselect_ms, driver_write_ms);
    
    // Reset write failure tracking on success
    g_transaction_manager.write_failure_first_tick = 0;
    g_transaction_manager.consecutive_write_failures = 0;
    
    return MIFARE_RESULT_OK;
}

/**
 * @brief Transition to new dispensing state
 * @param new_state New state to transition to
 */
static void mifare_transition_state(MIFARE_TransactionState_t new_state)
{
    MIFARE_TransactionState_t old_state = g_transaction_manager.transaction_state;
    g_transaction_manager.transaction_state = new_state;
    
    if (old_state != new_state) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: State: %s -> %s", 
                   MIFARE_GetTransactionStateString(old_state), 
                   MIFARE_GetTransactionStateString(new_state));
        
        // Initialize stability timer when entering READY if not already set
        if (new_state == TRANSACTION_STATE_READY && 
            g_transaction_manager.card_first_detected_tick == 0) {
            TickType_t now = xTaskGetTickCount();
            g_transaction_manager.card_first_detected_tick = now;
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card ready - stability timer started (wait %lu ms for confirmation)", 
                       Config_Get()->mifare.stability_timeout_ms);
        }
    }
}

/*Write Snapshot Helpers ------------------------------------------*/

static void mifare_clear_write_snapshot(void)
{
    memset(&g_transaction_manager.last_written_user_data, 0, sizeof(MIFARE_UserData_t));
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

    // Only compare user data - HMAC validates integrity
    if (memcmp(&card_data->user_primary,
               &g_transaction_manager.last_written_user_data,
               sizeof(MIFARE_UserData_t)) != 0) {
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
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Account data invalid - all_zeros=%d, has_valid_ascii=%d", all_zeros, has_valid_ascii);
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
 * @brief Internal write function with backup control
 * @param card_data Pointer to card data structure
 * @param skip_backup If true, only write primary block (faster, use during active dispense)
 * @return MIFARE_Result_t Operation result
 * 
 * @note BACKUP WRITE TRADEOFF:
 *       - Full write (primary+backup): ~430ms total
 *       - Fast write (primary only):   ~320ms total (saves ~110ms)
 *       
 *       During active dispenser, we use fast writes because:
 *       1. Card can be removed at ANY time - there is no "final commit" moment
 *       2. Primary block has HMAC for integrity validation
 *       3. Backup block may become stale, but primary is always current
 *       4. On next card read, if primary is corrupted, system falls back to backup
 *          (backup may be slightly behind, but better than total data loss)
 *       5. 110ms savings per write reduces chance of write-in-progress during removal
 */
static MIFARE_Result_t mifare_write_card_data_internal(MIFARE_CardData_t *card_data, bool skip_backup)
{
    TickType_t write_start_tick = xTaskGetTickCount();  // BENCHMARK: Total write time
    
    if (card_data == NULL || !card_data->data_valid) {
        return MIFARE_RESULT_ERROR;
    }
    
    // Check if PN532 is still recovering from previous write failure
    TickType_t now = xTaskGetTickCount();
    if (now < g_transaction_manager.pn532_recovery_until_tick) {
        uint32_t remaining_ms = pdTICKS_TO_MS(g_transaction_manager.pn532_recovery_until_tick - now);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write deferred - PN532 recovering for %lu ms more", remaining_ms);
        return MIFARE_RESULT_BUSY;
    }
    
    MIFARE_Result_t result;
    uint8_t block_data[16];
    
    TickType_t change_check_start = xTaskGetTickCount();
    if (!mifare_card_data_changed(card_data)) {
        last_card_write_changed = false;
        return MIFARE_RESULT_OK;
    }
    uint32_t change_check_ms = pdTICKS_TO_MS(xTaskGetTickCount() - change_check_start);
    
    // Update transaction counter
    card_data->user_primary.transaction_counter++;
    
    // Update the backup copy in RAM to match primary
    memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    
    /* Write User Primary Block (block 5, sector 1) */
    memcpy(block_data, (uint8_t*)&card_data->user_primary, sizeof(MIFARE_UserData_t));
    
    TickType_t primary_write_start = xTaskGetTickCount();
    result = mifare_write_block_with_retry(MIFARE_BLOCK_USER_PRIMARY, block_data, 500, "primary");
    uint32_t primary_write_ms = pdTICKS_TO_MS(xTaskGetTickCount() - primary_write_start);
    
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write primary data (block %u)", MIFARE_BLOCK_USER_PRIMARY);
        last_card_write_changed = false;
        return result;
    }
    
    uint32_t backup_write_ms = 0;
    
    if (!skip_backup) {
        // Small delay between writes
        vTaskDelay(pdMS_TO_TICKS(2));
        
        /* Write User Backup Block (block 6, sector 1) - SAME SECTOR, NO SWITCH! */
        /* HMAC in the data provides integrity validation - no separate CRC block needed */
        memcpy(block_data, (uint8_t*)&card_data->user_backup, sizeof(MIFARE_UserData_t));
        
        TickType_t backup_write_start = xTaskGetTickCount();
        result = mifare_write_block_with_retry(MIFARE_BLOCK_USER_BACKUP, block_data, 500, "backup");
        backup_write_ms = pdTICKS_TO_MS(xTaskGetTickCount() - backup_write_start);
        
        if (result != MIFARE_RESULT_OK) {
            // Backup write failure is not critical - primary is already written
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Backup write failed (non-critical)\r\n");
        }
    }
    
    mifare_update_write_snapshot(card_data);
    last_card_write_changed = true;
    no_change_log_reported = false;
    
    // Update stability tracking
    g_transaction_manager.last_successful_write_tick = xTaskGetTickCount();
    g_transaction_manager.card_first_lost_tick = 0;
    
    uint32_t total_write_ms = pdTICKS_TO_MS(xTaskGetTickCount() - write_start_tick);
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [BENCHMARK] Write: total=%lums, change=%lums, primary=%lums, backup=%lums%s",
               total_write_ms, change_check_ms, primary_write_ms, backup_write_ms,
               skip_backup ? " (fast)" : "");
    
    return MIFARE_RESULT_OK;
}

/**
 * @brief Write card data with atomic transaction support (writes primary + backup)
 * @param card_data Pointer to card data structure
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_WriteCardData(MIFARE_CardData_t *card_data)
{
    return mifare_write_card_data_internal(card_data, false);
}

/**
 * @brief Fast write - only primary block, skip backup (use during active transactions)
 * @param card_data Pointer to card data structure  
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_WriteCardDataFast(MIFARE_CardData_t *card_data)
{
    return mifare_write_card_data_internal(card_data, true);
}

/**
 * @brief Read single MIFARE block (public wrapper for decryptcard)
 * @param block_number Block number to read (0-63)
 * @param data Buffer to store read data (must be 16 bytes)
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_ReadBlock(uint8_t block_number, uint8_t *data)
{
    if (data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    return mifare_read_block(block_number, data);
}

/**
 * @brief Write single MIFARE block (public wrapper for decryptcard)
 * @param block_number Block number to write (0-63)
 * @param data Data to write (must be 16 bytes)
 * @param allow_trailer If true, allow writing sector trailers
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_WriteBlock(uint8_t block_number, const uint8_t *data, bool allow_trailer)
{
    if (data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    return mifare_write_block(block_number, (uint8_t*)data, allow_trailer);
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
        if (duration >= Config_Get()->mifare.stability_timeout_ms) {
            g_transaction_manager.card_presence_confirmed = true;
            g_transaction_manager.card_confirmed_present_tick = now;
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card presence CONFIRMED (stable for %lu ms)", duration);
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
        mifare_transition_state(TRANSACTION_STATE_READY);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card state transitioned to PRESENT/READY after polling cycle\r\n");
    }
}

/*Atomic Transaction Operations ----------------------------------*/

MIFARE_Result_t MIFARE_BeginTransaction(void)
{
    if (g_transaction_manager.transaction_active) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction already active\r\n");
        return MIFARE_RESULT_BUSY;
    }

    // Pre-auth: Reselect card to ensure PN532 is ready for writes
    // This eliminates the first-write reselect failure (~200ms penalty)
    PN532_CardInfo_t preauth_info;
    PN532_Status_t preauth_status = PN532_ReadPassiveTargetID(
        g_transaction_manager.pn532_handle,
        PN532_CARD_TYPE_106_TYPE_A,
        &preauth_info
    );
    if (preauth_status != PN532_STATUS_CARD_DETECTED) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Pre-auth reselect failed - card may be removed\r\n");
        return MIFARE_RESULT_CARD_REMOVED;
    }
    last_authenticated_sector = -1;  // Force fresh auth to sector 1

    // Balance checking now done by business logic layer (Dispenser Integration)
    
    // Save previous state to revert on failure
    uint8_t prev_state = g_transaction_manager.current_card.user_primary.transaction_state;
    
    g_transaction_manager.current_card.user_primary.transaction_state = CARD_TRANSACTION_STARTED;
    g_transaction_manager.transaction_active = true;
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction flag set to ACTIVE - polling suspended\r\n");
    
    MIFARE_Result_t result = MIFARE_WriteCardData(&g_transaction_manager.current_card);
    if (result == MIFARE_RESULT_OK) {
        mifare_transition_state(TRANSACTION_STATE_READY);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction started\r\n");
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to start transaction: %s", MIFARE_GetResultString(result));
        g_transaction_manager.transaction_active = false;
        
        // Revert in-memory state
        g_transaction_manager.current_card.user_primary.transaction_state = prev_state;
        
        // Only transition to error if card wasn't removed (removal already cleaned up state)
        if (result != MIFARE_RESULT_CARD_REMOVED) {
            MIFARE_SetErrorState_WriteFailed();
            LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("    Reason: %s\r\n", MIFARE_GetResultString(result));
        }
    }
    
    return result;
}



/**
 * @brief Initialize a MIFARE card for a new customer
 * @param initial_balance_ml Initial balance in milliliters of water
 * @param customer_id Unique customer identifier (can be derived from card serial)
 * @param force_factory_keys If true, force factory keys even if card has derived keys (for decryptcard)
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_InitializeNewCustomerCard(uint32_t initial_balance_ml, uint64_t customer_id, bool force_factory_keys)
{
    MIFARE_Result_t result = MIFARE_RESULT_ERROR;
    PN532_CardInfo_t card_info;
    
    // Report to watchdog at start of initialization (long operation)
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // Suspend polling task to prevent interference
    bool prev_transaction_active = g_transaction_manager.transaction_active;
    g_transaction_manager.transaction_active = true;
    
    // Wait for polling task to finish any active cycle (period is 50ms)
    vTaskDelay(pdMS_TO_TICKS(60));
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Initializing new customer card with balance: %u ml, Customer ID: %llu", 
               initial_balance_ml, customer_id);
    
    // First, detect the card
    // If a card is already active/authenticated, InListPassiveTarget (called by DetectCard) might fail.
    // Release any selected target first to ensure clean detection state.
    PN532_ReleaseTarget(g_transaction_manager.pn532_handle);
    
    // Wait a bit longer after release to ensure card state is reset
    vTaskDelay(pdMS_TO_TICKS(20));
    
    PN532_Status_t status = PN532_DetectCard(g_transaction_manager.pn532_handle, &card_info);
    if (status != PN532_STATUS_CARD_DETECTED) {
        // Retry once if detection fails (common if card was just released)
        vTaskDelay(pdMS_TO_TICKS(20));
        status = PN532_DetectCard(g_transaction_manager.pn532_handle, &card_info);
        
        if (status != PN532_STATUS_CARD_DETECTED) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: No card detected for initialization\r\n");
            result = MIFARE_RESULT_ERROR;
            goto cleanup;
        }
    }
    
    // Verify it's a MIFARE Classic card
    if (card_info.card_type != PN532_CARD_MIFARE_CLASSIC_1K &&
        card_info.card_type != PN532_CARD_MIFARE_CLASSIC_4K) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Unsupported card type for initialization: %d", card_info.card_type);
        result = MIFARE_RESULT_ERROR;
        goto cleanup;
    }
    
    // Store card info
    memcpy(&g_transaction_manager.card_info, &card_info, sizeof(PN532_CardInfo_t));
    
    // Initialize card data structure
    MIFARE_CardData_t *card_data = &g_transaction_manager.current_card;
    memset(card_data, 0, sizeof(MIFARE_CardData_t));
    
    uint32_t current_time = (uint32_t)xTaskGetTickCount();
    
    // 1. Initialize card header
    card_data->header.magic_bytes = MIFARE_MAGIC_BYTES;
    card_data->header.format_version = MIFARE_FORMAT_VERSION;
    card_data->header.card_type = card_info.card_type;
    card_data->header.card_serial = customer_id;
    card_data->header.header_crc = 0; // Ensure 0 for CRC calculation
    card_data->header.header_crc = MIFARE_CALCULATE_CRC16((uint8_t*)&card_data->header, 
                                                          sizeof(MIFARE_CardHeader_t));
    
    // 2. Initialize primary user data (balance_ml, status, state)
    card_data->user_primary.balance_ml = initial_balance_ml;
    card_data->user_primary.last_topup_ml = initial_balance_ml;  // Set last topup to initial amount
    card_data->user_primary.transaction_counter = 0;
    card_data->user_primary.status_flags = CARD_STATUS_ACTIVE;
    card_data->user_primary.transaction_state = CARD_TRANSACTION_IDLE;
    memset(card_data->user_primary.hmac, 0, sizeof(card_data->user_primary.hmac));
    
    // 3. Initialize usage data (lifetime statistics)
    card_data->usage_data.total_volume_purchased_ml = initial_balance_ml;
    card_data->usage_data.total_dispenses_completed = 0;
    card_data->usage_data.total_volume_dispensed_ml = 0;
    card_data->usage_data.reserved = 0;
    
    // 4. Initialize backup user data (identical to primary)
    memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    
    // 5. Initialize transaction log
    memset(&card_data->transaction_log, 0, sizeof(MIFARE_TransactionLog_t));
    card_data->transaction_log.head_index = 0;
    card_data->transaction_log.count = 0;
    card_data->transaction_log.log_crc = MIFARE_CALCULATE_CRC16(
        (uint8_t*)card_data->transaction_log.records,
        sizeof(MIFARE_TransactionRecord_t) * MIFARE_MAX_TRANSACTIONS);
    
    // 6. Mark data as valid
    card_data->data_valid = true;
    card_data->last_read_time = current_time;
    
    // 7. Write the initialized data to the card
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Writing initial data structure to card...\r\n");
    
    // Determine which keys to use for writes:
    // - If card already has derived keys (auto-reinit scenario), use derived keys
    // - If card has factory keys (blank card), use factory keys then program derived keys
    // Test by attempting auth with derived keys on ALL sectors we'll write to
    // Sectors: 12 (block 48), 13 (block 52), 14 (block 56), 15 (block 60)
    last_authenticated_sector = -1;  // Clear cache to force fresh auth test
    
    uint8_t test_blocks[] = {MIFARE_BLOCK_ACCOUNT_DATA,  // 48 - Sector 12
                             52,                          // 52 - Sector 13 (recovery info, no named constant)
                             MIFARE_BLOCK_USAGE_DATA,     // 56 - Sector 14
                             MIFARE_BLOCK_HEADER};        // 60 - Sector 15
    bool all_sectors_have_derived_keys = true;
    
    // Always detect current keys on card (needed for write authentication)
    for (uint8_t i = 0; i < 4; i++) {
        PN532_Status_t auth_test = PN532_MifareAuthenticate(
            g_transaction_manager.pn532_handle,
            test_blocks[i],
            (uint8_t*)g_transaction_manager.card_info.uid,
            g_transaction_manager.card_info.uid_length,
            derived_sector_key
        );
        
        if (auth_test != PN532_STATUS_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Sector %d (block %d) has factory keys\r\n", 
                       MIFARE_GET_SECTOR(test_blocks[i]), test_blocks[i]);
            all_sectors_have_derived_keys = false;
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(5));  // Brief delay between auth tests
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    }
    
    if (all_sectors_have_derived_keys) {
        // Card already has derived keys on all sectors - use them for writes (auto-reinit scenario)
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card has derived keys on all sectors - using them for re-initialization writes\r\n");
        use_factory_keys_for_writes = false;
        last_authenticated_sector = -1;  // Clear cache, will re-auth as needed
    } else {
        // Card has factory keys on one or more sectors - use them for writes, then program derived keys
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card has factory keys - using them for initialization writes\r\n");
        use_factory_keys_for_writes = true;
        last_authenticated_sector = -1;
        
        // Re-select card after failed auth attempt
        vTaskDelay(pdMS_TO_TICKS(50));
        PN532_CardInfo_t reselect_info;
        PN532_Status_t reselect_status = PN532_ReadPassiveTargetID(
            g_transaction_manager.pn532_handle, PN532_CARD_TYPE_106_TYPE_A, &reselect_info);
        
        if (reselect_status != PN532_STATUS_CARD_DETECTED) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card re-selection failed after auth test - retrying once\r\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            reselect_status = PN532_ReadPassiveTargetID(
                g_transaction_manager.pn532_handle, PN532_CARD_TYPE_106_TYPE_A, &reselect_info);
            
            if (reselect_status != PN532_STATUS_CARD_DETECTED) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card re-selection failed - card may have been removed\r\n");
                result = MIFARE_RESULT_CARD_REMOVED;
                goto cleanup;
            }
        }
    }
    
    // Write header block (authentication handled internally)
    result = mifare_write_block(MIFARE_BLOCK_HEADER, (uint8_t*)&card_data->header, false);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write header block\r\n");
        use_factory_keys_for_writes = false;  // Restore normal operation
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
    
    // Write primary user data block
    result = mifare_write_block(MIFARE_BLOCK_USER_PRIMARY, (uint8_t*)&card_data->user_primary, false);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write primary data block\r\n");
        use_factory_keys_for_writes = false;  // Restore normal operation
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
    
    // Write backup user data block
    result = mifare_write_block(MIFARE_BLOCK_USER_BACKUP, (uint8_t*)&card_data->user_backup, false);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write backup data block\r\n");
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
    
    // Write usage data block
    result = mifare_write_block(MIFARE_BLOCK_USAGE_DATA, (uint8_t*)&card_data->usage_data, false);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write usage data block\r\n");
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
    
    // Write transaction log (may span multiple blocks)
    uint8_t *log_data = (uint8_t*)&card_data->transaction_log;
    size_t log_total_size = sizeof(MIFARE_TransactionLog_t);
    uint8_t blocks_needed = (log_total_size + 15) / 16;
    
    uint8_t block_num = MIFARE_BLOCK_TRANSACTION_LOG;
    
    for (uint8_t i = 0; i < blocks_needed; i++) {
        if ((block_num % 4) == 3) {
            block_num++;
        }
        
        result = mifare_write_block(block_num, &log_data[i * 16], false);
        if (result != MIFARE_RESULT_OK) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write transaction log block %d", i);
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
        
        block_num++;
    }
    
    // Report status after transaction log writes (~1800ms elapsed)
    // Force WDT update for both tasks
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);

    // 8. Initialize and write account data (block 16 - sector 4)
    // This is done BEFORE sector trailer updates so if sector 4 still has factory keys,
    // the write uses factory key fallback, and then we update the trailer
    mifare_encode_account_data(&card_data->account_data, "07970242024", CARD_VALIDITY_NORMAL);
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Writing account data to block %d...\r\n", MIFARE_BLOCK_ACCOUNT_DATA);
    result = mifare_write_block(MIFARE_BLOCK_ACCOUNT_DATA, card_data->account_data.raw_data, false);
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write account data block (status=%d)\r\n", result);
        // Non-fatal - continue with initialization, account data can be written later
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Account data written successfully\r\n");
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);

    // Write sector keys to all sector trailers:
    // - If config has custom keys AND card has factory keys: Write derived keys
    // - If force_factory_keys is true: Write factory keys (for decryptcard command)
    // - If card already has derived keys and NOT force_factory_keys: Skip (already correct)
    const SystemConfig_t *config = Config_Get();
    if (force_factory_keys) {
        // Force factory keys for decryptcard command
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Force factory keys requested - writing factory keys to all sector trailers...\r\n");
        
        // Sector trailer format: 6 bytes Key A | 4 bytes Access | 6 bytes Key B
        uint8_t sector_trailer[16];
        uint8_t factory_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        memcpy(sector_trailer, factory_key, 6);              // Key A (factory default)
        sector_trailer[6] = 0xFF;  // Access bits
        sector_trailer[7] = 0x07;
        sector_trailer[8] = 0x80;
        sector_trailer[9] = 0x69;
        memcpy(&sector_trailer[10], factory_key, 6);         // Key B (factory default)
        
        // Write factory keys to sector trailers (allow_trailer_write=true)
        uint8_t trailer_blocks[] = {51, 55, 59, 63};  // Sectors 12, 13, 14, 15
        for (uint8_t i = 0; i < 4; i++) {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Writing factory keys to sector %d trailer (block %d)...\r\n",
                       MIFARE_GET_SECTOR(trailer_blocks[i]), trailer_blocks[i]);
            
            // mifare_write_block will handle authentication and use correct keys (derived or factory)
            result = mifare_write_block(trailer_blocks[i], sector_trailer, true);
            
            if (result != MIFARE_RESULT_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write sector %d trailer: %s\r\n", 
                           MIFARE_GET_SECTOR(trailer_blocks[i]), MIFARE_GetResultString(result));
                goto cleanup;
            }
            
            vTaskDelay(pdMS_TO_TICKS(20));
            System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        }
        
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card sector keys converted to factory defaults successfully\r\n");
        
        // CRITICAL: Skip verification after force_factory_keys because mifare_read_block()
        // uses derived keys but card now has factory keys (only sector 4 has factory fallback)
        // Verification would fail on sector 15 (header block) with authentication error
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Skipping verification (card now has factory keys)\r\n");
        
        // Set proper state transitions for successful completion
        MIFARE_LogTransaction(1, initial_balance_ml / 1000, 0xFF);  // Log in liters
        last_authenticated_sector = -1;
        mifare_transition_state(TRANSACTION_STATE_CARD_DETECTED);
        g_transaction_manager.last_card_update_time = current_time;
        MIFARE_SetCardState(MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card initialization completed successfully\r\n");
        
        result = MIFARE_RESULT_OK;
        goto cleanup;
        
    } else if (config->mifare.security.use_custom_sector_keys && use_factory_keys_for_writes) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Writing derived sector keys to all sector trailers...\r\n");
        
        // Sector trailer format: 6 bytes Key A | 4 bytes Access | 6 bytes Key B
        uint8_t sector_trailer[16];
        memcpy(sector_trailer, derived_sector_key, 6);       // Key A (derived)
        sector_trailer[6] = 0xFF;  // Access bits
        sector_trailer[7] = 0x07;
        sector_trailer[8] = 0x80;
        sector_trailer[9] = 0x69;
        memcpy(&sector_trailer[10], derived_sector_key, 6);  // Key B (same as Key A)
        
        // Write to sector trailers: blocks 3, 7, 11, 15
        // Must use default key for initial authentication (card still has factory keys)
        uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        MIFARE_Classic_Auth_t auth = {
            .uid_length = g_transaction_manager.card_info.uid_length
        };
        memcpy(auth.key, default_key, 6);  // Authenticate with default key first
        memcpy(auth.uid, g_transaction_manager.card_info.uid, auth.uid_length);
        
        // Application uses sectors 12-15 only (blocks 48-63)
        // Sector 0-2 may be locked/read-only on genuine cards
        uint8_t trailer_blocks[] = {51, 55, 59, 63};  // Sectors 12-15
        for (uint8_t i = 0; i < 4; i++) {
            uint8_t block = trailer_blocks[i];
            
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Writing derived keys to sector %d trailer (block %d)...", block/4, block);
            
            // Use mifare_write_block with allow_trailer_write=true
            result = mifare_write_block(block, sector_trailer, true);
            
            if (result == MIFARE_RESULT_OK) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Sector %d trailer updated with derived keys", block/4);
            } else {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write sector %d trailer: %s", block/4, MIFARE_GetResultString(result));
                goto cleanup;
            }
            
            vTaskDelay(pdMS_TO_TICKS(15));  // Allow card to commit sector trailer
            System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
            System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
        }
        
        // Clear auth cache - new keys in effect now
        last_authenticated_sector = -1;
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: All sector trailers updated with UID-derived keys\r\n");
        
        // Use derived keys for remaining writes
        use_factory_keys_for_writes = false;
    } else if (!use_factory_keys_for_writes) {
        // Card already has derived keys on sectors 1-3 (auto-reinit scenario)
        // BUT check if sector 4 still has factory keys (common on older cards)
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card has derived keys - checking if sector 4 needs update...\r\n");
        
        // Feed WDT before sector 4 check operations
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
        
        // Test auth on sector 4 with derived key
        last_authenticated_sector = -1;  // Force fresh auth
        PN532_Status_t sector4_test = PN532_MifareAuthenticate(
            g_transaction_manager.pn532_handle,
            MIFARE_BLOCK_ACCOUNT_DATA,  // Block 16 (sector 4)
            (uint8_t*)g_transaction_manager.card_info.uid,
            g_transaction_manager.card_info.uid_length,
            derived_sector_key
        );
        
        // Feed WDT after auth test
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
        
        if (sector4_test != PN532_STATUS_OK) {
            // Sector 4 still has factory keys - update it
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Sector 4 has factory keys - updating trailer to derived keys\r\n");
            
            // Re-select card after failed auth
            vTaskDelay(pdMS_TO_TICKS(50));
            PN532_CardInfo_t reselect_info;
            PN532_ReadPassiveTargetID(g_transaction_manager.pn532_handle, PN532_CARD_TYPE_106_TYPE_A, &reselect_info);
            
            // Feed WDT after reselect
            System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
            System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
            
            // Prepare sector trailer with derived keys
            uint8_t sector_trailer[16];
            memcpy(sector_trailer, derived_sector_key, 6);       // Key A (derived)
            sector_trailer[6] = 0xFF;  // Access bits
            sector_trailer[7] = 0x07;
            sector_trailer[8] = 0x80;
            sector_trailer[9] = 0x69;
            memcpy(&sector_trailer[10], derived_sector_key, 6);  // Key B (same as Key A)
            
            // Auth with factory key (sector 4 still has them)
            uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            MIFARE_Classic_Auth_t auth = {
                .uid_length = g_transaction_manager.card_info.uid_length
            };
            memcpy(auth.key, default_key, 6);
            memcpy(auth.uid, g_transaction_manager.card_info.uid, auth.uid_length);
            
            MIFARE_Classic_Status_t auth_status = MIFARE_Classic_Authenticate(
                g_transaction_manager.pn532_handle, 19, &auth, &last_authenticated_sector);  // Block 19 = sector 4 trailer
            
            // Feed WDT after factory key auth
            System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
            System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
            
            if (auth_status == MIFARE_CLASSIC_OK) {
                PN532_Status_t write_status = PN532_MifareWriteBlock(
                    g_transaction_manager.pn532_handle, 19, sector_trailer);
                
                // Feed WDT after trailer write
                System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
                System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
                
                if (write_status == PN532_STATUS_OK) {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Sector 4 trailer updated with derived keys\r\n");
                    last_authenticated_sector = -1;  // Clear cache - new keys in effect
                    
                    // Re-write block 16 (account data) now that sector 4 has derived keys
                    // This ensures HMAC is computed in the correct key context
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Re-writing account data with new sector keys...\r\n");
                    vTaskDelay(pdMS_TO_TICKS(50));  // Allow trailer to settle
                    result = mifare_write_block(MIFARE_BLOCK_ACCOUNT_DATA, card_data->account_data.raw_data, false);
                    
                    // Feed WDT after account data write
                    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
                    System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
                    
                    if (result == MIFARE_RESULT_OK) {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Account data re-written with derived key HMAC\r\n");
                    } else {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to re-write account data (status=%d)\r\n", result);
                    }
                } else {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write sector 4 trailer (status=0x%02X)\r\n", write_status);
                    // Continue anyway - sector 4 will use factory key fallback
                }
            } else {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to auth sector 4 trailer for update\r\n");
                // Continue anyway - sector 4 will use factory key fallback
            }
            
            vTaskDelay(pdMS_TO_TICKS(15));
            System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
            System_ReportTaskStatus(SYSTEM_TASK_ID_DISPENSER, true);
        } else {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Sector 4 already has derived keys - skipping\r\n");
            last_authenticated_sector = 4;  // Cache the successful auth
        }
    } else {
        // Use factory default keys (backward compatible mode)
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Using factory default sector keys (0xFF...) - no trailer updates needed\r\n");
        // Keep using factory keys for remaining writes
    }

    // Report status after all block writes complete
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // Verify - skip SD logging for verification read to avoid USB buffer overflow
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Verifying written data...\r\n");
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow card to stabilize after heavy writing
    skip_sd_logging = true;  // Don't log verification read to SD
    MIFARE_CardData_t verify_data;
    result = MIFARE_ReadCardData(&verify_data);
    skip_sd_logging = false;
    if (result != MIFARE_RESULT_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to read back initialized data\r\n");
        goto cleanup;
    }
    
    if (verify_data.header.magic_bytes != MIFARE_MAGIC_BYTES ||
        verify_data.header.card_serial != customer_id ||
        verify_data.user_primary.balance_ml != initial_balance_ml) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Data verification failed after initialization\r\n");
        result = MIFARE_RESULT_DATA_MISMATCH;
        goto cleanup;
    }

    MIFARE_LogTransaction(1, initial_balance_ml / 1000, 0xFF);  // Log in liters
    
    last_authenticated_sector = -1;
    mifare_transition_state(TRANSACTION_STATE_INITIALIZED);
    g_transaction_manager.last_card_update_time = current_time;
    MIFARE_SetCardState(MIFARE_CARD_STATE_PRESENT);
    
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✓] Card initialization successful\r\n");
    result = MIFARE_RESULT_OK;

cleanup:
    // Restore transaction active state
    g_transaction_manager.transaction_active = prev_transaction_active;
    
    // If we used factory keys override, ensure it's cleared
    use_factory_keys_for_writes = false;
    
    // On error, reset card state to prevent stuck-in-reprocessing loop
    if (result != MIFARE_RESULT_OK) {
        MIFARE_SetErrorState_WriteFailed();
        LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("    Context: Card initialization - %s\r\n", MIFARE_GetResultString(result));
        MIFARE_SetCardState(MIFARE_CARD_STATE_PRESENT);
        last_authenticated_sector = -1;
    }
    
    return result;
}

MIFARE_Result_t MIFARE_ValidateCardData(MIFARE_CardData_t *card_data)
{
    if (card_data == NULL) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Card data is NULL\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Checking magic bytes: 0x%08lX (expected: 0x%08lX)", 
               card_data->header.magic_bytes, MIFARE_MAGIC_BYTES);
    if (card_data->header.magic_bytes != MIFARE_MAGIC_BYTES) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Magic bytes MISMATCH\r\n");
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Checking format version: %d (expected: %d)", 
               card_data->header.format_version, MIFARE_FORMAT_VERSION);
    if (card_data->header.format_version != MIFARE_FORMAT_VERSION) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Format version MISMATCH\r\n");
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
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Header CRC: read=0x%04X, calculated=0x%04X", 
               read_crc, calculated_header_crc);
    if (read_crc != calculated_header_crc) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Header CRC MISMATCH\r\n");
        return MIFARE_RESULT_CARD_CORRUPTED;
    }
    
    // User data integrity is validated by HMAC during mifare_read_block()
    // If we get here, the HMAC was already verified successfully
    // Compare primary and backup to detect mismatches
    if (memcmp(&card_data->user_primary, &card_data->user_backup, 12) != 0) {
        // First 12 bytes differ (balance, topup, counter) - try to recover
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Primary/backup mismatch detected\r\n");
        return MIFARE_RESULT_DATA_MISMATCH;
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: [VALIDATE] Card data valid - OK\r\n");
    card_data->data_valid = true;
    return MIFARE_RESULT_OK;
}

MIFARE_Result_t MIFARE_RecoverCardData(MIFARE_CardData_t *card_data)
{
    if (card_data == NULL) {
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: RecoverCardData: Attempting to recover card data\r\n");
    
    // With HMAC validation in mifare_read_block, we know which blocks passed HMAC
    // For now, prefer primary data and sync to backup
    // In a more complete implementation, we'd track HMAC validation status per block
    
    memcpy(&card_data->user_backup, &card_data->user_primary, sizeof(MIFARE_UserData_t));
    card_data->data_valid = true;
    
    MIFARE_Result_t result;
    
    // Write recovered backup block (HMAC computed during write)
    result = mifare_write_block(MIFARE_BLOCK_USER_BACKUP, (uint8_t*)&card_data->user_backup, false);
    if (result != MIFARE_RESULT_OK) return result;
    
    bool phone_is_empty = mifare_is_account_data_empty(&card_data->account_data);
    
    if (phone_is_empty) {
        mifare_encode_account_data(&card_data->account_data, "07970242024", CARD_VALIDITY_NORMAL);
        result = mifare_write_block(MIFARE_BLOCK_ACCOUNT_DATA, card_data->account_data.raw_data, false);
    }
    
    return MIFARE_RESULT_OK;
}

/* Data Integrity Functions -------------------------------------------------*/
/* CRC16 calculation moved to MIFARE_Classic_Driver */

MIFARE_Result_t MIFARE_DetectAndAutoInitializeCard(const PN532_CardInfo_t *card_info, uint32_t default_balance_ml)
{
    PN532_Status_t status;
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: DetectAndAutoInitializeCard: Entry\r\n");
    
    // Check if USB command handler has a pending CARDINIT command
    // Only skip auto-init for cardinit (manual initialization) - other commands need the card ready
    USB_PendingCommandState_t *pending = USB_Command_GetPendingCommand();
    if (pending != NULL && pending->active && pending->command == USB_PENDING_CMD_CARD_INIT) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: cardinit pending - skipping auto-init to allow manual control\r\n");
        return MIFARE_RESULT_BUSY;  // Card detected but waiting for manual cardinit
    }
    
    // For topup/recover commands, proceed with auto-init so card becomes READY
    if (pending != NULL && pending->active) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: USB command pending (%d) - proceeding with auto-init\r\n", 
                   pending->command);
    }
    
    // Report WDT status but don't yield - we want fast card read
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    // Try derived custom keys FIRST (fast path for already-initialized cards)
    // This avoids 2400ms timeout when card has custom keys
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Attempting derived key authentication on block %d...\r\n", MIFARE_BLOCK_HEADER);
    status = PN532_MifareAuthenticate(
        g_transaction_manager.pn532_handle,
        MIFARE_BLOCK_HEADER,
        (uint8_t*)card_info->uid,
        card_info->uid_length,
        derived_sector_key  // Try custom derived key first
    );
    
    // Report WDT status after potentially long I2C operation
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    if (status == PN532_STATUS_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Derived key auth succeeded - verifying with test read\r\n");
        
        // Authentication succeeded - proceed directly to test read
        // Do NOT re-select card here as it would break the authenticated session
        uint8_t header_data[16];
        status = PN532_MifareReadBlock(g_transaction_manager.pn532_handle, MIFARE_BLOCK_HEADER, header_data);
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Test read status: 0x%02X", status);
        
        // Yield after test read I2C operation
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        if (status == PN532_STATUS_OK) {
            // Successfully read with derived keys - this is an existing initialized card
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Existing initialized card with custom keys detected.\r\n");
            last_authenticated_sector = MIFARE_GET_SECTOR(MIFARE_BLOCK_HEADER);
            
            // Load card data
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Loading card data...\r\n");
            MIFARE_Result_t read_result = MIFARE_ReadCardData(&g_transaction_manager.current_card);
            
            // Check if card is corrupted (CRC or HMAC failure) - trigger auto-reinit if enabled
            if (read_result == MIFARE_RESULT_CARD_CORRUPTED) {
                const SystemConfig_t *config = Config_Get();
                if (config->mifare.auto_reinit_on_corruption) {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card data corrupted (HMAC/CRC mismatch). Auto-reinit enabled.\r\n");
                    
                    // Derive customer ID from UID
                    uint64_t customer_id = 0;
                    for (int i = 0; i < card_info->uid_length; i++) {
                        customer_id = (customer_id << 8) | card_info->uid[i];
                    }
                    
                    // STEP 1: Try to recover balance from SD card log first
                    uint32_t recovered_balance_ml = 0;
                    bool sd_recovery_success = SD_Logger_RecoverCardBalance(
                        card_info->uid, card_info->uid_length, &recovered_balance_ml);
                    
                    uint32_t init_balance_ml;
                    if (sd_recovery_success && recovered_balance_ml > 0) {
                        // Use recovered balance from SD card
                        init_balance_ml = recovered_balance_ml;
                        USB_Log_Printf("MIFARE: Auto-recovery from SD log - balance: %lu ml\r\n\r\n", init_balance_ml);
                    } else {
                        // Fall back to default balance from config
                        init_balance_ml = config->mifare.card_init_default_balance_ml;
                        USB_Log_Printf("MIFARE: Auto-recovery using default balance: %lu ml\r\n\r\n", init_balance_ml);
                    }
                    
                    return MIFARE_InitializeNewCustomerCard(init_balance_ml, customer_id, false);
                } else {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card corrupted but auto-reinit disabled - manual intervention required\r\n");
                }
            }
            
            return read_result;
        } else {
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Derived key auth succeeded but read failed - card may be corrupted\r\n");
            return MIFARE_RESULT_ERROR;
        }
    }
    
    // Derived keys failed - card may be blank or have factory keys
    // CRITICAL: After failed auth, PN532 requires re-selecting the card before trying another key
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Derived key auth failed - re-selecting card before trying factory keys...\r\n");
    
    // Minimal delay for PN532 to recover from failed auth
    vTaskDelay(pdMS_TO_TICKS(10));
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    PN532_CardInfo_t reselect_info;
    PN532_Status_t reselect_status = PN532_ReadPassiveTargetID(
        g_transaction_manager.pn532_handle,
        PN532_CARD_TYPE_106_TYPE_A,
        &reselect_info
    );
    
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    if (reselect_status != PN532_STATUS_CARD_DETECTED) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card re-selection failed after derived key failure\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card re-selected, trying factory default keys...\r\n");
    uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    // Try to authenticate block MIFARE_BLOCK_HEADER (header)
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Attempting factory key authentication on block %d...", MIFARE_BLOCK_HEADER);
    
    // Report status before potentially long I2C operation
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    status = PN532_MifareAuthenticate( 
                                      g_transaction_manager.pn532_handle,
                                      MIFARE_BLOCK_HEADER, 
                                      (uint8_t*)card_info->uid, 
                                      card_info->uid_length, 
                                      default_key);
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Factory key auth status: 0x%02X", status);
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
                                          
    if (status == PN532_STATUS_OK) {
        // Authenticated successfully - proceed directly to read
        // Do NOT re-select card here as it would break the authenticated session
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Factory key auth succeeded - attempting read...\r\n");
        
        uint8_t header_data[16];
        status = PN532_MifareReadBlock(g_transaction_manager.pn532_handle, MIFARE_BLOCK_HEADER, header_data);
        
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Read block status after factory auth: 0x%02X", status);
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        if (status == PN532_STATUS_OK) {
            // Check for Magic Bytes (Little Endian)
            uint32_t magic = (header_data[0]) | (header_data[1] << 8) | (header_data[2] << 16) | (header_data[3] << 24);
            
            if (magic == MIFARE_MAGIC_BYTES) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Existing initialized card detected.\r\n");
                
                // DISABLED: Auto-upgrade from factory to custom keys
                // After decryptcard, we want cards to stay with factory keys
                // Users must run 'cardinit' explicitly to set custom keys
                #if 0  // DISABLED - no auto-upgrade
                // Check if we need to upgrade sector keys BEFORE reading card data
                // (Reading requires authentication with the current keys)
                const SystemConfig_t *config = Config_Get();
                if (config->mifare.security.use_custom_sector_keys) {
                    // Card is using factory default keys (we just authenticated with 0xFF...)
                    // Config says to use custom keys - upgrade BEFORE reading
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card using factory keys, config requires custom keys - upgrading...\r\n");
                    
                    // Report to watchdog before starting upgrade (slow operation)
                    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
                    
                    // Create sector trailer with derived keys
                    uint8_t sector_trailer[16];
                    memset(sector_trailer, 0, 16);
                    
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Derived key to write: %02X%02X%02X%02X%02X%02X",
                              derived_sector_key[0], derived_sector_key[1], derived_sector_key[2],
                              derived_sector_key[3], derived_sector_key[4], derived_sector_key[5]);
                    
                    // Copy derived Key A (6 bytes)
                    memcpy(&sector_trailer[0], derived_sector_key, 6);
                    
                    // Access bits (4 bytes) - standard configuration
                    sector_trailer[6] = 0xFF;
                    sector_trailer[7] = 0x07;
                    sector_trailer[8] = 0x80;
                    sector_trailer[9] = 0x69;
                    
                    // Copy derived Key B (6 bytes)
                    memcpy(&sector_trailer[10], derived_sector_key, 6);
                    
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Sector trailer: KeyA=%02X%02X... Access=%02X%02X%02X%02X KeyB=%02X%02X...",
                              sector_trailer[0], sector_trailer[1],
                              sector_trailer[6], sector_trailer[7], sector_trailer[8], sector_trailer[9],
                              sector_trailer[10], sector_trailer[11]);
                    
                    // Write new keys to sector trailers for sectors 12, 13, 14, 15
                    uint8_t sector_trailer_blocks[] = {51, 55, 59, 63};
                    bool upgrade_success = true;
                    
                    for (uint8_t i = 0; i < 4; i++) {
                        // Report to watchdog during each sector upgrade
                        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
                        
                        uint8_t block = sector_trailer_blocks[i];
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Upgrading sector %d trailer (block %d)...", i, block);
                        
                        // Authenticate with factory default key (still using it)
                        status = PN532_MifareAuthenticate(
                            g_transaction_manager.pn532_handle,
                            block,
                            (uint8_t*)card_info->uid,
                            card_info->uid_length,
                            default_key
                        );
                        
                        if (status != PN532_STATUS_OK) {
                            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to auth sector %d for upgrade", i);
                            upgrade_success = false;
                            break;
                        }
                        
                        // Write new derived keys
                        status = PN532_MifareWriteBlock(g_transaction_manager.pn532_handle, block, sector_trailer);
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Write sector %d trailer status: 0x%02X", i, status);
                        if (status != PN532_STATUS_OK) {
                            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write sector %d trailer", i);
                            upgrade_success = false;
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));  // Allow card to commit
                    }
                    
                    if (upgrade_success) {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card sector keys upgraded successfully - now using custom derived keys\r\n");
                        // Clear authentication cache since keys changed
                        last_authenticated_sector = -1;
                        
                        // Re-authenticate with new derived keys to establish valid session
                        // This is critical because PN532's auth state is now invalid after key change
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Re-authenticating with new derived keys...\r\n");
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Using key: %02X%02X%02X%02X%02X%02X",
                                  derived_sector_key[0], derived_sector_key[1], derived_sector_key[2],
                                  derived_sector_key[3], derived_sector_key[4], derived_sector_key[5]);
                        status = PN532_MifareAuthenticate(
                            g_transaction_manager.pn532_handle,
                            MIFARE_BLOCK_HEADER,  // Block 60
                            (uint8_t*)card_info->uid,
                            card_info->uid_length,
                            derived_sector_key  // Use NEW derived key
                        );
                        
                        if (status != PN532_STATUS_OK) {
                            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to re-authenticate with new derived keys!\r\n");
                            return MIFARE_RESULT_ERROR;
                        }
                        
                        // Update cache to reflect successful auth
                        last_authenticated_sector = MIFARE_GET_SECTOR(MIFARE_BLOCK_HEADER);
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Re-authentication successful - ready for read/write\r\n");
                    } else {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Sector key upgrade failed - card may have mixed keys\r\n");
                        return MIFARE_RESULT_ERROR;
                    }
                    
                    // Final watchdog report after upgrade
                    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
                }
                #endif  // End of disabled auto-upgrade block
                
                // Now read card data (will use factory keys since no upgrade)
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Loading card data...\r\n");
                MIFARE_Result_t read_result = MIFARE_ReadCardData(&g_transaction_manager.current_card);
                
                // Check if card is corrupted (CRC or HMAC failure)
                if (read_result == MIFARE_RESULT_CARD_CORRUPTED) {
                    const SystemConfig_t *config = Config_Get();
                    if (config->mifare.auto_reinit_on_corruption) {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card data corrupted (HMAC/CRC mismatch). Auto-reinit enabled.\r\n");
                        
                        // Derive customer ID from UID
                        uint64_t customer_id = 0;
                        for (int i = 0; i < card_info->uid_length; i++) {
                            customer_id = (customer_id << 8) | card_info->uid[i];
                        }
                        
                        // STEP 1: Try to recover balance from SD card log first
                        uint32_t recovered_balance_ml = 0;
                        bool sd_recovery_success = SD_Logger_RecoverCardBalance(
                            card_info->uid, card_info->uid_length, &recovered_balance_ml);
                        
                        uint32_t init_balance_ml;
                        if (sd_recovery_success && recovered_balance_ml > 0) {
                            // Use recovered balance from SD card
                            init_balance_ml = recovered_balance_ml;
                            USB_Log_Printf("MIFARE: Auto-recovery from SD log - balance: %lu ml\r\n\r\n", init_balance_ml);
                        } else {
                            // Fall back to default balance from config
                            init_balance_ml = config->mifare.card_init_default_balance_ml;
                            USB_Log_Printf("MIFARE: Auto-recovery using default balance: %lu ml\r\n\r\n", init_balance_ml);
                        }
                        
                        return MIFARE_InitializeNewCustomerCard(init_balance_ml, customer_id, false);
                    } else {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card corrupted but auto-reinit disabled - manual intervention required\r\n");
                        return MIFARE_RESULT_CARD_CORRUPTED;
                    }
                }
                
                return read_result;
            }
        } else {
            // Read failed - do NOT assume it's blank. It might be a communication error.
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to read block %d (status 0x%02X). Card error or blank card.", MIFARE_BLOCK_HEADER, status);
            return MIFARE_RESULT_ERROR;
        }

        // Card accepts default key AND read succeeded AND magic bytes didn't match.
        // This is a blank/unformatted card - do NOT auto-initialize, wait for manual command
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Blank/Unformatted card detected - manual 'cardinit' required\r\n");
        USB_Log_Printf("MIFARE: Blank card detected. Use 'cardinit' command to initialize.\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    // Authentication with factory default keys failed
    // Card likely has custom keys already - try derived keys directly
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Factory key auth failed - card likely has custom keys\r\n");
    
    // Clear PN532 state by re-selecting the card after failed auth
    // The failed auth corrupts PN532's internal state - add delay for recovery
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Re-selecting card to clear PN532 state...\r\n");
    
    // Give PN532 time to recover from failed auth before re-selection
    vTaskDelay(pdMS_TO_TICKS(50));
    System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
    
    PN532_CardInfo_t reselect_info2;
    PN532_Status_t reselect_status2 = PN532_ReadPassiveTargetID(
        g_transaction_manager.pn532_handle,
        PN532_CARD_TYPE_106_TYPE_A,  // MIFARE Classic uses Type A
        &reselect_info2
    );
    
    if (reselect_status2 != PN532_STATUS_CARD_DETECTED) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card re-selection failed after failed factory auth\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    // Verify same card
    if (memcmp(reselect_info2.uid, card_info->uid, card_info->uid_length) != 0) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Different card detected after re-selection!\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card re-selected successfully, trying derived keys...\r\n");
    status = PN532_MifareAuthenticate(
        g_transaction_manager.pn532_handle,
        MIFARE_BLOCK_HEADER,
        (uint8_t*)card_info->uid,
        card_info->uid_length,
        derived_sector_key  // Try custom derived key
    );
    
    if (status == PN532_STATUS_OK) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Auth with derived keys succeeded - verifying with test read\r\n");
        
        // Don't update cache yet - let's verify read works first
        uint8_t header_data[16];
        status = PN532_MifareReadBlock(g_transaction_manager.pn532_handle, MIFARE_BLOCK_HEADER, header_data);
        
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Test read status: 0x%02X", status);
        
        if (status == PN532_STATUS_OK) {
            // Read succeeded - update auth cache
            last_authenticated_sector = MIFARE_GET_SECTOR(MIFARE_BLOCK_HEADER);
            
            // Check for Magic Bytes (Little Endian)
            uint32_t magic = (header_data[0]) | (header_data[1] << 8) | (header_data[2] << 16) | (header_data[3] << 24);
            
            if (magic == MIFARE_MAGIC_BYTES) {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Existing initialized card with custom keys detected.\r\n");
                
                // Update auth cache
                last_authenticated_sector = MIFARE_GET_SECTOR(MIFARE_BLOCK_HEADER);
                
                // Read card data directly (already authenticated with correct keys)
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Loading card data...\r\n");
                MIFARE_Result_t read_result = MIFARE_ReadCardData(&g_transaction_manager.current_card);
                
                // Check if card is corrupted (CRC or HMAC failure)
                if (read_result == MIFARE_RESULT_CARD_CORRUPTED) {
                    const SystemConfig_t *config = Config_Get();
                    if (config->mifare.auto_reinit_on_corruption) {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card data corrupted (HMAC/CRC mismatch). Auto-reinit enabled.\r\n");
                        
                        // Derive customer ID from UID
                        uint64_t customer_id = 0;
                        for (int i = 0; i < card_info->uid_length; i++) {
                            customer_id = (customer_id << 8) | card_info->uid[i];
                        }
                        
                        // STEP 1: Try to recover balance from SD card log first
                        uint32_t recovered_balance_ml = 0;
                        bool sd_recovery_success = SD_Logger_RecoverCardBalance(
                            card_info->uid, card_info->uid_length, &recovered_balance_ml);
                        
                        uint32_t init_balance_ml;
                        if (sd_recovery_success && recovered_balance_ml > 0) {
                            // Use recovered balance from SD card
                            init_balance_ml = recovered_balance_ml;
                            USB_Log_Printf("MIFARE: Auto-recovery from SD log - balance: %lu ml\r\n\r\n", init_balance_ml);
                        } else {
                            // Fall back to default balance from config
                            init_balance_ml = config->mifare.card_init_default_balance_ml;
                            USB_Log_Printf("MIFARE: Auto-recovery using default balance: %lu ml\r\n\r\n", init_balance_ml);
                        }
                        
                        return MIFARE_InitializeNewCustomerCard(init_balance_ml, customer_id, false);
                    } else {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card corrupted but auto-reinit disabled - manual intervention required\r\n");
                        return MIFARE_RESULT_CARD_CORRUPTED;
                    }
                }
                
                return read_result;
            } else {
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card has custom keys but invalid magic bytes - cannot auto-init (would destroy data)\r\n");
                return MIFARE_RESULT_ERROR;
            }
        } else {
            // Read failed but authentication succeeded - card has custom keys but corrupted data
            // This happens after a partial upgrade (keys changed but data not rewritten)
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to read header with custom keys (status 0x%02X) - forcing reinit", status);
            
            // Update auth cache since we successfully authenticated
            last_authenticated_sector = MIFARE_GET_SECTOR(MIFARE_BLOCK_HEADER);
            
            // Derive customer ID from UID
            uint64_t customer_id = 0;
            for (int i = 0; i < card_info->uid_length; i++) {
                customer_id = (customer_id << 8) | card_info->uid[i];
            }
            
            // Force re-initialization with default balance
            const SystemConfig_t *config = Config_Get();
            uint32_t init_balance_ml = config->mifare.card_init_default_balance_ml;
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Re-initializing card with balance: %lu ml", (unsigned long)init_balance_ml);
            return MIFARE_InitializeNewCustomerCard(init_balance_ml, customer_id, false);
        }
    }
    
    // Neither factory nor derived keys worked
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Authentication failed with both factory and derived keys\r\n");
    return MIFARE_RESULT_ERROR;
}

/**
 * @brief Get current dispense state
 * @return MIFARE_TransactionState_t Current transaction state
 */
MIFARE_TransactionState_t MIFARE_GetDispenseState(void)
{
    return g_transaction_manager.transaction_state;
}

/**
 * @brief Get current balance in milliliters from card
 * @return Current balance in ml, or 0 if card not ready
 * @deprecated Use MIFARE_GetUserData()->balance_ml instead for direct access
 */
uint32_t MIFARE_GetBalanceMl(void)
{
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    return user_data ? user_data->balance_ml : 0;
}

/**
 * @brief Add volume to card (topup)
 * @param topup_ml Number of milliliters of water to add
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_TopupCardBalance(uint32_t topup_ml)
{
    if (!MIFARE_IsCardReady()) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card not ready for topup\r\n");
        return MIFARE_RESULT_ERROR;
    }
    
    if (topup_ml == 0 || topup_ml > 200000) {  // Max 200 liters
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Invalid topup amount: %lu ml (must be 1-200000)", topup_ml);
        return MIFARE_RESULT_ERROR;
    }
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Adding %lu ml to card", topup_ml);
    
    /* Get current card data */
    MIFARE_CardData_t *card_data = &g_transaction_manager.current_card;
    
    uint32_t old_balance = card_data->user_primary.balance_ml;
    uint32_t new_balance = old_balance + topup_ml;
    
    /* Check for overflow */
    if (new_balance < old_balance) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Balance overflow: %lu + %lu", old_balance, topup_ml);
        return MIFARE_RESULT_ERROR;
    }
    
    /* Update balance in card data */
    card_data->user_primary.balance_ml = new_balance;
    card_data->user_backup.balance_ml = new_balance;
    card_data->user_primary.last_topup_ml = topup_ml;
    card_data->user_backup.last_topup_ml = topup_ml;
    
    /* Set transaction_active to suspend polling during write */
    g_transaction_manager.transaction_active = true;
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Topup: Polling suspended for write\r\n");
    
    /* Write to card */
    MIFARE_Result_t result = MIFARE_WriteCardData(card_data);
    
    /* Clear transaction_active to resume polling */
    g_transaction_manager.transaction_active = false;
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Topup: Polling resumed\r\n");
    
    if (result == MIFARE_RESULT_OK) {
        /* Clear any spurious removal timer that started during write */
        g_transaction_manager.card_first_lost_tick = 0;
        
        /* Transition to READY_AFTER_TOPUP state - triggers 8-beep pattern and keeps card balance on screen */
        mifare_transition_state(TRANSACTION_STATE_READY_AFTER_TOPUP);
        
        LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[\u2713] Topup successful: %lu + %lu = %lu ml\r\n", 
                       old_balance, topup_ml, new_balance);
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Failed to write topup to card: %s", MIFARE_GetResultString(result));
        /* Restore old balance on failure */
        card_data->user_primary.balance_ml = old_balance;
        card_data->user_backup.balance_ml = old_balance;
    }
    
    return result;
}

/**
 * @brief Get pointer to user data (balance_ml, status, etc.)
 * @return Pointer to user data structure (read-only for caller), or NULL if not ready
 */
MIFARE_UserData_t* MIFARE_GetUserData(void)
{
    /* Allow access when card is ready, OR when in post-init/topup states (data is valid but not "ready") */
    bool card_ready = MIFARE_IsCardReady();
    bool in_post_init_state = (g_transaction_manager.transaction_state == TRANSACTION_STATE_INITIALIZED ||
                                g_transaction_manager.transaction_state == TRANSACTION_STATE_READY_AFTER_TOPUP);
    bool data_valid = g_transaction_manager.current_card.data_valid;
    
    if (!card_ready && !in_post_init_state) {
        return NULL;
    }
    
    /* For post-init states, only return data if it's actually valid */
    if (in_post_init_state && !data_valid) {
        return NULL;
    }
    
    return &g_transaction_manager.current_card.user_primary;
}

/**
 * @brief Get pointer to usage data (lifetime statistics)
 * @return Pointer to usage data structure (read-only for caller), or NULL if not ready
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
 * @return Pointer to account data structure (read-only for caller), or NULL if not ready
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
 * @return true if card present, data valid, and in READY state
 */
bool MIFARE_IsCardReady(void)
{
    bool card_present = (g_transaction_manager.card_state == MIFARE_CARD_STATE_PRESENT);
    bool data_valid = g_transaction_manager.current_card.data_valid;
    bool state_ready = (g_transaction_manager.transaction_state == TRANSACTION_STATE_READY ||
                        g_transaction_manager.transaction_state == TRANSACTION_STATE_READY_AFTER_TOPUP);
    bool result = card_present && data_valid && state_ready;
    
    // Debug log when any condition fails (helps diagnose why card not ready)
    if (!result) {
        static uint32_t debug_counter = 0;
        if (++debug_counter % 500 == 1) {  // Log every 50th call to avoid spam
            LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: IsCardReady=FALSE (present=%d, valid=%d, ready=%d)\r\n",
                           card_present, data_valid, state_ready);
        }
    }
    
    return result;
}

/**
 * @brief Get current card info (UID, type, etc)
 * @param info Pointer to structure to fill
 * @return true if card info is valid, false otherwise
 */
bool MIFARE_GetCurrentCardInfo(PN532_CardInfo_t *info)
{
    if (!info) return false;
    
    // Only return info if card is present
    if (g_transaction_manager.card_state == MIFARE_CARD_STATE_ABSENT) {
        return false;
    }
    
    memcpy(info, &g_transaction_manager.card_info, sizeof(PN532_CardInfo_t));
    return true;
}

/**
 * @brief Set user data (business logic modifies via this)
 * @param user_data Pointer to new user data
 */
void MIFARE_SetUserData(const MIFARE_UserData_t *user_data)
{
    if (!user_data) return;
    
    // Copy to in-memory structure (primary and backup in sector 1)
    memcpy(&g_transaction_manager.current_card.user_primary, user_data, sizeof(MIFARE_UserData_t));
    memcpy(&g_transaction_manager.current_card.user_backup, user_data, sizeof(MIFARE_UserData_t));
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
 * @brief Set error state: card corrupted/blank (needs cardinit)
 */
void MIFARE_SetErrorState_CardCorrupted(void)
{
    mifare_transition_state(TRANSACTION_STATE_ERROR_CARD_CORRUPTED);
    MIFARE_SetCardState(MIFARE_CARD_STATE_ERROR);
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✗] Card corrupted/blank. Use 'cardinit' command to initialize.\r\n");
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[→] Remove card and re-insert after initialization.\r\n");
}

/**
 * @brief Set error state: hardware module failure (PN532, etc)
 */
void MIFARE_SetErrorState_ModuleFailure(void)
{
    mifare_transition_state(TRANSACTION_STATE_ERROR_MODULE_FAILURE);
    MIFARE_SetCardState(MIFARE_CARD_STATE_ERROR);
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✗] Hardware module error detected\r\n");
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[→] Remove card and re-insert to retry.\r\n");
}

/**
 * @brief Set error state: card validation failed
 */
void MIFARE_SetErrorState_ValidationFailed(void)
{
    mifare_transition_state(TRANSACTION_STATE_ERROR_VALIDATION_FAILED);
    MIFARE_SetCardState(MIFARE_CARD_STATE_ERROR);
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✗] Card validation failed\r\n");
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[→] Remove card and re-insert to retry.\r\n");
}

/**
 * @brief Set error state: transaction write failed
 */
void MIFARE_SetErrorState_WriteFailed(void)
{
    mifare_transition_state(TRANSACTION_STATE_ERROR_WRITE_FAILED);
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[✗] Transaction write failed\r\n");
    LOG_CRITICAL_MIFARE_TRANSACTION_MANAGER("[→] Remove card and re-insert to retry.\r\n");
}

/**
 * @brief Set transaction state (public API)
 * @param new_state New transaction state
 */
void MIFARE_SetTransactionState(MIFARE_TransactionState_t new_state)
{
    mifare_transition_state(new_state);
}

/**
 * @brief Update card data - write in-memory data to physical card
 * @param fast If true, skip backup write (faster, use during active transactions)
 * @return MIFARE_Result_t Operation result
 */
MIFARE_Result_t MIFARE_UpdateCardData(bool fast)
{
    // This function writes the in-memory card data structures to the physical card
    // Business logic calls this after modifying data via setters
    
    if (!MIFARE_IsCardPresent()) {
        return MIFARE_RESULT_CARD_REMOVED;
    }
    
    if (fast) {
        return MIFARE_WriteCardDataFast(&g_transaction_manager.current_card);
    }
    return MIFARE_WriteCardData(&g_transaction_manager.current_card);
}

/**
 * @brief Get last top-up amount in milliliters
 * @return uint32_t Last top-up amount in ml
 */
uint32_t MIFARE_GetLastTopupMl(void)
{
    MIFARE_UserData_t *user_data = MIFARE_GetUserData();
    return user_data ? user_data->last_topup_ml : 0;
}

/**
 * @brief Get total dispensed amount in the current session (legacy - not used in dispenser)
 * @return uint32_t Always returns 0 (not applicable for dispenser)
 * @deprecated Not used in dispenser system
 */
uint32_t MIFARE_GetTotalDispensedThisSession(void)
{
    return 0;  // Not applicable for dispenser system
}

/**
 * @brief Get card status for UI (returns true if card present and ready)
 * @return bool True if card is present and ready
 */
bool MIFARE_GetCardStatus(void)
{
    return MIFARE_IsCardReady();
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
 * @brief Get total purchased volume (lifetime)
 * @return uint32_t Total volume purchased in ml
 */
uint32_t MIFARE_GetTotalTokensPurchased(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_volume_purchased_ml : 0;
}

/**
 * @brief Get total dispenses completed (lifetime)
 * @return uint32_t Total dispense sessions completed
 */
uint32_t MIFARE_GetTotalWashesCompleted(void)
{
    MIFARE_UsageData_t *usage_data = MIFARE_GetUsageData();
    return usage_data ? usage_data->total_dispenses_completed : 0;
}

/**
 * @brief Get customer phone number from card
 * @param phone_buffer Buffer to store phone number (must be at least 12 bytes)
 * @param buffer_size Size of the buffer
 * @return bool True if phone number retrieved, false if no card or buffer too small
 */
bool MIFARE_GetCustomerPhoneNumber(char *phone_buffer, size_t buffer_size)
{
    if (phone_buffer == NULL || buffer_size < 12) {
        return false;
    }
    
    // Check if card is present and data is valid
    if (g_transaction_manager.card_state != MIFARE_CARD_STATE_PRESENT || 
        !g_transaction_manager.current_card.data_valid) {
        return false;
    }
    
    // Extract phone number from account data (bytes 0-10)
    const uint8_t *raw_data = g_transaction_manager.current_card.account_data.raw_data;
    
    // Copy phone number bytes and null-terminate
    memcpy(phone_buffer, raw_data, 11);
    phone_buffer[11] = '\0';
    
    // Validate it contains only digits
    for (int i = 0; i < 11; i++) {
        if (phone_buffer[i] < '0' || phone_buffer[i] > '9') {
            return false;  // Invalid phone number
        }
    }
    
    return true;
}

void MIFARE_LogTransaction(uint8_t type, uint16_t amount_ml, uint8_t dispenser_id)
{
    MIFARE_CardData_t *card_data = &g_transaction_manager.current_card;
    MIFARE_TransactionLog_t *log = &card_data->transaction_log;
    
    // Update log
    uint8_t index = log->head_index;
    log->records[index].timestamp = (uint32_t)xTaskGetTickCount();
    log->records[index].volume_ml = amount_ml;
    log->records[index].transaction_type = type;
    log->records[index].dispenser_id = dispenser_id;
    
    log->head_index = (index + 1) % MIFARE_MAX_TRANSACTIONS;
    if (log->count < MIFARE_MAX_TRANSACTIONS) {
        log->count++;
    }
    
    // Update CRC
    log->log_crc = MIFARE_CALCULATE_CRC16((uint8_t*)log->records, 
                                          sizeof(MIFARE_TransactionRecord_t) * MIFARE_MAX_TRANSACTIONS);
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Transaction logged: Type=%d, Amount=%u mL", type, amount_ml);
}


/*Card Polling Task ----------------------------------------------*/
static TaskHandle_t mifare_polling_task_handle = NULL;

/**
 * @brief Get MIFARE polling task handle
 * @return Task handle or NULL if not created
 */
TaskHandle_t task_get_handle_MIFARE_Polling_Task(void)
{
    return mifare_polling_task_handle;
}

/**
 * @brief Execute pending USB command in MIFARE task context
 * @details Called by MIFARE polling task when card is stable in CARD_DETECTED state
 * @param pending Pointer to pending command state
 */
static void MIFARE_ExecutePendingUSBCommand(USB_PendingCommandState_t* pending)
{
    if (pending == NULL || !pending->active) {
        return;
    }
    
    // Execute the command via USB handler's execution function
    // This ensures all MIFARE I/O happens in MIFARE task context
    USB_Command_ExecutePendingCommand(pending);
}

/**
 * @brief MIFARE card polling task
 * @details Continuously polls for card presence/removal
 */
static void MIFARE_Polling_Task(void* argument)
{
    (void)argument;
    
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t periodTicks = pdMS_TO_TICKS(50);  // Poll every 50ms for faster card detection
    
    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: MIFARE polling task started\r\n");
    
    for (;;)
    {
        vTaskDelayUntil(&lastWake, periodTicks);
        System_ReportTaskStatus(SYSTEM_TASK_ID_MIFARE_POLLING, true);
        
        // Get current states
        MIFARE_CardState_t current_card_state = MIFARE_GetCardState();
        MIFARE_TransactionState_t current_dispense_state = MIFARE_GetTransactionState();
        
        // Skip polling during active transaction OR active dispense
        // This prevents I2C bus contention between polling and card writes
        if (g_transaction_manager.transaction_active || MIFARE_Dispenser_IsDispenseActive()) {
            continue;
        }
        
        // Poll for card
        PN532_CardInfo_t card_info;
        PN532_Status_t status = PN532_DetectCard(g_transaction_manager.pn532_handle, &card_info);
        
        if (status == PN532_STATUS_CARD_DETECTED) {
            // Card detected
            if (current_card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
                MIFARE_ConfirmReadyAfterPolling();
            } else if (current_card_state == MIFARE_CARD_STATE_PRESENT && 
                       current_dispense_state == TRANSACTION_STATE_READY) {
                // Card is ready - check if USB command is pending and execute it
                USB_PendingCommandState_t* pending = USB_Command_GetPendingCommand();
                if (pending != NULL && pending->active) {
                    // USB command pending - execute it now that card is ready
                    MIFARE_ExecutePendingUSBCommand(pending);
                }
                // No logging for normal ready state to avoid spam
            } else if (current_dispense_state == TRANSACTION_STATE_ERROR) {
#if MIFARE_ERROR_STATE_AUTO_RECOVERY_EN
                // Card present but in ERROR state - attempt recovery
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card present in ERROR state - attempting recovery\r\n");
                // Reset card state to allow re-initialization
                MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
                mifare_transition_state(TRANSACTION_STATE_IDLE);
                // Re-process as new card
                MIFARE_ProcessCardDetected(&card_info);
#else
                // Check if USB command is pending (manual cardinit/recover)
                USB_PendingCommandState_t* pending = USB_Command_GetPendingCommand();
                if (pending != NULL && pending->active) {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: USB command pending in ERROR state - clearing ERROR to allow command\r\n");
                    MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
                    mifare_transition_state(TRANSACTION_STATE_IDLE);
                    // Re-process as new card so the pending command check in DetectAndAutoInitializeCard can catch it
                    MIFARE_ProcessCardDetected(&card_info);
                } else {
                    // ERROR state recovery disabled - stay in ERROR until card removed or manual command (log once)
                    static bool error_state_logged = false;
                    if (!error_state_logged) {
                        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card present in ERROR state - staying in ERROR (recovery disabled)\r\n");
                        error_state_logged = true;
                    }
                }
#endif
            } else if (current_card_state == MIFARE_CARD_STATE_ABSENT) {
                // Truly new card detected
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: New card detected\r\n");
                MIFARE_ProcessCardDetected(&card_info);
            } else if (current_dispense_state == TRANSACTION_STATE_CARD_DETECTED) {
                // Card in CARD_DETECTED state - check if cardinit pending
                USB_PendingCommandState_t* pending = USB_Command_GetPendingCommand();
                if (pending != NULL && pending->active && pending->command == USB_PENDING_CMD_CARD_INIT) {
                    // cardinit pending - execute it now (card doesn't need to be READY)
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Executing cardinit in CARD_DETECTED state\r\n");
                    MIFARE_ExecutePendingUSBCommand(pending);
                } else {
                    // Not cardinit - shouldn't be in this state, try re-processing
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card in CARD_DETECTED but no cardinit - re-processing\r\n");
                    MIFARE_ProcessCardDetected(&card_info);
                }
            } else if (current_dispense_state == TRANSACTION_STATE_WAITING_FOR_REMOVAL ||
                       current_dispense_state == TRANSACTION_STATE_INITIALIZED ||
                       current_dispense_state == TRANSACTION_STATE_READY_AFTER_TOPUP ||
                       current_dispense_state == TRANSACTION_STATE_ERROR_NO_FLOW ||
                       current_dispense_state == TRANSACTION_STATE_ERROR_CARD_CORRUPTED ||
                       current_dispense_state == TRANSACTION_STATE_ERROR_MODULE_FAILURE ||
                       current_dispense_state == TRANSACTION_STATE_ERROR_VALIDATION_FAILED ||
                       current_dispense_state == TRANSACTION_STATE_ERROR_WRITE_FAILED) {
                // Card still present after USB command or error - do nothing, wait for removal
                // (no logging to avoid spam)
            } else {
                // Unexpected state combination - log and skip to avoid loop
                static uint32_t state_spam_counter = 0;
                if (++state_spam_counter % 100 == 1) {  // Log every 100th time to avoid spam
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card detected but state mismatch (card=%d, txn=%d) - skipping\r\n",
                               current_card_state, current_dispense_state);
                }
            }
        } else {
            // No card detected
            if (current_dispense_state == TRANSACTION_STATE_WAITING_FOR_REMOVAL ||
                current_dispense_state == TRANSACTION_STATE_INITIALIZED ||
                current_dispense_state == TRANSACTION_STATE_READY_AFTER_TOPUP ||
                current_dispense_state == TRANSACTION_STATE_ERROR_NO_FLOW ||
                current_dispense_state == TRANSACTION_STATE_ERROR_CARD_CORRUPTED ||
                current_dispense_state == TRANSACTION_STATE_ERROR_MODULE_FAILURE ||
                current_dispense_state == TRANSACTION_STATE_ERROR_VALIDATION_FAILED ||
                current_dispense_state == TRANSACTION_STATE_ERROR_WRITE_FAILED) {
                // USB command completed or error cleared, waiting for physical removal - transition to IDLE
                LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card removed after USB command/error - transitioning to IDLE\r\n");
                MIFARE_ForceCardRemoval();  // Clean up state
            } else if (current_card_state == MIFARE_CARD_STATE_PRESENT) {
                // Only log on first detection to avoid spam during stability timeout
                static bool removal_logged = false;
                if (!removal_logged) {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Poll - no card detected, starting removal process\r\n");
                    removal_logged = true;
                }
                
                MIFARE_Result_t removal_result = MIFARE_ProcessCardRemoved();
                
                // If removal completed (stability timeout passed), reset log flag
                if (removal_result == MIFARE_RESULT_OK && MIFARE_GetCardState() != MIFARE_CARD_STATE_PRESENT) {
                    removal_logged = false;
                }
            } else if (current_card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
                MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
            } else if ((current_card_state == MIFARE_CARD_STATE_ERROR || current_dispense_state == TRANSACTION_STATE_ERROR) 
                       && status != PN532_STATUS_CARD_DETECTED) {
                // Error state but no card - reset after delay
                static uint32_t error_no_card_counter = 0;
                error_no_card_counter++;
                if (error_no_card_counter >= 10) {
                    LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Card removed from ERROR state - clearing to IDLE\r\n");
                    MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
                    mifare_transition_state(TRANSACTION_STATE_IDLE);
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
        MIFARE_POLLING_TASK_PRIORITY,  // Highest app priority - card writes must complete quickly
        &mifare_polling_task_handle
    );
    
    if (result == pdPASS) {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: MIFARE polling task created successfully\r\n");
    } else {
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: ERROR: Failed to create MIFARE polling task\r\n");
    }
}

/**
 * @brief Stop the MIFARE card polling task
 */
void MIFARE_StopPollingTask(void)
{
    if (mifare_polling_task_handle != NULL) {
        vTaskDelete(mifare_polling_task_handle);
        mifare_polling_task_handle = NULL;
        LOG_DEBUG_MIFARE_TRANSACTION_MANAGER("MIFARE: Polling task stopped\r\n");
    }
}

