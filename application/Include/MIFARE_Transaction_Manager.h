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

#ifndef APPLICATION_INCLUDE_MIFARE_TRANSACTION_MANAGER_H_
#define APPLICATION_INCLUDE_MIFARE_TRANSACTION_MANAGER_H_

/*Includes ----------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "PN532_Driver.h"
#include "MIFARE_Security.h"
#include <stdint.h>
#include <stdbool.h>

/*Defines ------------------------------------------------------------*/
#define MIFARE_MAGIC_BYTES              0x6D575441UL    // "mWTA" in little endian (keep same magic bytes)
#define MIFARE_FORMAT_VERSION           0x02            // Version 2 for water dispenser system
#define MIFARE_MAX_TRANSACTIONS         3               // Store last 3 transactions (Fits in 2 blocks: 9 & 10)
#define MIFARE_CARD_TIMEOUT_MS          2000           // Card must respond within 2s
#define MIFARE_MAX_RETRIES              3              // Maximum retry attempts
#define MIFARE_CARD_REMOVAL_FAIL_COUNT  3              // Consecutive failures to infer card removal
#define MIFARE_FAST_BALANCE_UPDATE_MS   100             // Fast balance cache update interval (100ms) - reduced I2C load
#define MIFARE_MAIN_DATA_UPDATE_MS      500             // Main user data update interval (500ms) - reduced I2C load
#define MIFARE_STABILITY_TIMEOUT_MS     0               // No stability delay - authenticate immediately for fastest response
#define MIFARE_REMOVAL_STABILITY_MS     150             // Card must be absent for 150ms before confirmed removed (fast response)

/* MIFARE Classic 1K Block Layout - OPTIMIZED FOR SPEED
 * WARNING: Blocks 3, 7, 11, 15, 19, 23, 27, 31, etc. are SECTOR TRAILERS
 *          containing authentication keys and MUST NOT be written to!
 * 
 * DESIGN: All frequent writes stay in one sector to avoid sector switching (~300ms penalty)
 *         HMAC (last 4 bytes of user data) provides integrity - no separate CRC needed
 *         Using HIGH sectors (15, 14, 13) to avoid corruption from previous testing
 * 
 * Sector 15 (blocks 60-63) - FREQUENT ACCESS:
 *   Block 60: Header (read once at init)
 *   Block 61: User Primary (balance_ml + HMAC) - written during dispense based on flow
 *   Block 62: User Backup (identical copy) - written during dispense based on flow
 *   Block 63: SECTOR TRAILER (do not use)
 * 
 * Sector 14 (blocks 56-59) - STATISTICS (end of session):
 *   Block 56: Usage Data (lifetime statistics)
 *   Block 57: Transaction Log (start)
 *   Block 58: Transaction Log
 *   Block 59: SECTOR TRAILER (do not use)
 * 
 * Sector 13 (blocks 52-55) - ACCOUNT DATA (read once):
 *   Block 52: Account Data (phone number, validity)
 *   Block 53-54: Reserved
 *   Block 55: SECTOR TRAILER (do not use)
 */
#define MIFARE_BLOCK_HEADER             60             // Card header and system info (Sector 15)
#define MIFARE_BLOCK_USER_PRIMARY       61             // Primary user data: balance_ml, status, HMAC (Sector 15)
#define MIFARE_BLOCK_USER_BACKUP        62             // Backup user data (SAME SECTOR - no switch!) (Sector 15)
#define MIFARE_BLOCK_USAGE_DATA         56             // Usage statistics: lifetime counts (Sector 14)
#define MIFARE_BLOCK_TRANSACTION_LOG    57             // Transaction log start (Sector 14)
#define MIFARE_BLOCK_ACCOUNT_DATA       52             // Account data: phone number, validity (Sector 13)

/* Transaction States (stored on card in user_primary.transaction_state) */
#define CARD_TRANSACTION_IDLE          0x00
#define CARD_TRANSACTION_STARTED       0x01
#define CARD_TRANSACTION_DISPENSING    0x02
#define CARD_TRANSACTION_COMMIT_READY  0x03
#define CARD_TRANSACTION_COMMITTED     0x04
#define CARD_TRANSACTION_ROLLBACK      0xFF

/* Card Status Flags */
#define CARD_STATUS_ACTIVE              0x01
#define CARD_STATUS_SUSPENDED           0x02
#define CARD_STATUS_MAINTENANCE         0x04
#define CARD_STATUS_LOW_BALANCE         0x08
#define CARD_STATUS_PENDING_TRANSACTION 0x10  // Volume deduction in progress - used for crash recovery
#define CARD_STATUS_CORRUPTED           0x80

/*Typedefs -----------------------------------------------------------*/

/**
 * @brief Represents the state of the MIFARE card in the system.
 */
typedef enum {
    MIFARE_CARD_STATE_ABSENT,
    MIFARE_CARD_STATE_PRESENT,
    MIFARE_CARD_STATE_INITIALIZING,
    MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE, // Card is initialized, needs a clean polling cycle to become ready
    MIFARE_CARD_STATE_ERROR
} MIFARE_CardState_t;

/**
 * @brief Card header stored in block 4
 */
typedef struct __attribute__((packed)) {
    uint32_t magic_bytes;           // System identifier "mWTA"
    uint8_t format_version;         // Data format version
    uint8_t card_type;             // Card type identifier
    uint16_t header_crc;           // Header checksum
    uint64_t card_serial;          // Unique card serial number
} MIFARE_CardHeader_t;

/**
 * @brief User account data - Current balance and card state
 * Note: Size must be exactly 16 bytes to fit in a single MIFARE Classic block
 * Stored in Block 5 (primary) and Block 6 (backup) - BOTH IN SECTOR 1!
 * Car Wash System: balance_ml = milliliters of water remaining
 * 
 * INTEGRITY: First 12 bytes are data, last 4 bytes become HMAC after encryption.
 *            HMAC validates data integrity - no separate CRC block needed!
 */
typedef struct __attribute__((packed)) {
    uint32_t balance_ml;           // Current water balance in milliliters (bytes 0-3)
    uint32_t last_topup_ml;        // Last topup amount in milliliters (bytes 4-7)
    uint16_t transaction_counter;  // Incremented on each transaction (bytes 8-9)
    uint8_t status_flags;          // Card status bits (byte 10)
    uint8_t transaction_state;     // Current transaction state (byte 11)
    uint8_t hmac[4];               // HMAC-SHA256 truncated (bytes 12-15) - set by encryption
    // Total size: 4+4+2+1+1+4 = 16 bytes (perfect fit!)
} MIFARE_UserData_t;

/**
 * @brief Usage statistics - Lifetime wash totals and counters
 * Note: Size must be <= 16 bytes to fit in a single MIFARE Classic block
 * Stored in Block 8 (usage tracking)
 */
typedef struct __attribute__((packed)) {
    uint32_t total_volume_purchased_ml; // Total volume purchased (lifetime in ml)
    uint32_t total_dispenses_completed; // Total dispense sessions completed (lifetime)
    uint32_t total_volume_dispensed_ml; // Total volume dispensed (lifetime in ml)
    uint32_t reserved;                  // Reserved for future statistics
    // Total size: 4+4+4+4 = 16 bytes (perfect fit!)
} MIFARE_UsageData_t;

/**
 * @brief Individual transaction record
 */
typedef struct __attribute__((packed)) {
    uint32_t timestamp;            // Unix timestamp
    uint16_t volume_ml;            // Volume dispensed in ml (max 65535 ml = 65.5 liters)
    uint8_t transaction_type;      // 1=Topup, 2=Dispense Started, 3=Dispense Completed, 4=Refund
    uint8_t dispenser_id;          // Which dispenser unit was used
} MIFARE_TransactionRecord_t;

/**
 * @brief Transaction log structure
 */
typedef struct __attribute__((packed)) {
    MIFARE_TransactionRecord_t records[MIFARE_MAX_TRANSACTIONS];
    uint8_t head_index;           // Next write position
    uint8_t count;                // Number of valid records
    uint16_t log_crc;             // Log integrity checksum
} MIFARE_TransactionLog_t;

/**
 * @brief Recovery and integrity information
 */
typedef struct __attribute__((packed)) {
    uint32_t last_update_time;     // Last successful update timestamp
    uint16_t primary_data_crc;     // CRC of primary user data
    uint16_t backup_data_crc;      // CRC of backup user data
    uint8_t sequence_number;       // Alternating sequence (0/1)
    uint8_t recovery_attempts;     // Number of recovery attempts
    uint16_t integrity_flags;      // Data integrity status
} MIFARE_RecoveryInfo_t;

/**
 * @brief Balance cache - Quick access to current balance
 * Size: 16 bytes (fits in one MIFARE Classic block)
 */
typedef struct __attribute__((packed)) {
    uint32_t balance_ml;           // Current balance in milliliters (4 bytes)
    uint16_t sequence_number;      // Incremental counter (2 bytes) - used to detect newer data
    uint16_t reserved1;            // Reserved for alignment (2 bytes)
    uint32_t timestamp;            // Last update time in ms (4 bytes)
    uint32_t crc32;                // CRC32 for integrity check (4 bytes)
    // Total: 16 bytes (full MIFARE block)
} MIFARE_TokenCache_t;

/**
 * @brief Card validity status enumeration
 */
typedef enum {
    CARD_VALIDITY_BLOCKED = 0,     // Card is blocked
    CARD_VALIDITY_TEST = 1,        // Card is in test mode
    CARD_VALIDITY_NORMAL = 2,      // Card is active and normal
    CARD_VALIDITY_CONFIGURE = 3    // Card is in configuration mode
} MIFARE_CardValidity_t;

/**
 * @brief Account data for MIFARE card
 * Contains user identification and card validity information
 * Size: 16 bytes (fits in one MIFARE Classic block)
 * 
 * Layout in raw_data array:
 * Bytes 0-10:  Phone number "07970242024" (11 ASCII digits)
 * Byte 11:     Card validity enum (MIFARE_CardValidity_t)
 * Bytes 12-13: CRC16 checksum
 * Bytes 14-15: Reserved
 */
typedef struct __attribute__((packed)) {
    uint8_t raw_data[16];          // Raw 16-byte block data
} MIFARE_AccountData_t;

/* Helper macros to access account data fields */
#define ACCOUNT_DATA_PHONE_OFFSET       0
#define ACCOUNT_DATA_PHONE_SIZE         11
#define ACCOUNT_DATA_VALIDITY_OFFSET    11
#define ACCOUNT_DATA_CRC_OFFSET         12
#define ACCOUNT_DATA_RESERVED_OFFSET    14

/**
 * @brief Complete card data structure
 */
typedef struct {
    MIFARE_CardHeader_t header;
    MIFARE_UserData_t user_primary;        // Block 5: Current token count, status, state
    MIFARE_UserData_t user_backup;         // Block 6: Backup of user data
    MIFARE_UsageData_t usage_data;         // Block 8: Lifetime wash statistics
    MIFARE_TransactionLog_t transaction_log;
    MIFARE_RecoveryInfo_t recovery_info;
    MIFARE_TokenCache_t token_cache_primary;
    MIFARE_TokenCache_t token_cache_backup;
    MIFARE_AccountData_t account_data;     // Block 16: Account info (phone, validity)
    bool data_valid;
    uint32_t last_read_time;
} MIFARE_CardData_t;

/**
 * @brief Generic transaction/card I/O state (application agnostic)
 */
typedef enum {
    TRANSACTION_STATE_IDLE,
    TRANSACTION_STATE_CARD_DETECTED,
    TRANSACTION_STATE_AUTHENTICATING,
    TRANSACTION_STATE_READING_DATA,
    TRANSACTION_STATE_VALIDATING,
    TRANSACTION_STATE_READY,                    // Normal ready - auto-dispense allowed
    TRANSACTION_STATE_READY_AFTER_TOPUP,       // After USB topup - show balance, trigger 8-beep pattern, no auto-dispense
    TRANSACTION_STATE_INITIALIZED,             // After cardinit - show balance, trigger 8-beep pattern, no auto-dispense
    TRANSACTION_STATE_WRITING_DATA,
    TRANSACTION_STATE_FINALIZING,
    TRANSACTION_STATE_ERROR,
    TRANSACTION_STATE_ERROR_NO_FLOW,           // Dispense error: no flow detected
    TRANSACTION_STATE_ERROR_CARD_CORRUPTED,    // Card corrupted/blank - needs init
    TRANSACTION_STATE_ERROR_MODULE_FAILURE,    // Hardware module failure (PN532, etc)
    TRANSACTION_STATE_ERROR_VALIDATION_FAILED, // Card validation failed
    TRANSACTION_STATE_ERROR_WRITE_FAILED,      // Transaction write failed
    TRANSACTION_STATE_CARD_REMOVED,
    TRANSACTION_STATE_WAITING_FOR_REMOVAL      // Legacy/generic removal state (avoid using)
} MIFARE_TransactionState_t;

/**
 * @brief Transaction manager handle
 */
typedef struct {
    MIFARE_CardData_t current_card;
    MIFARE_TransactionState_t transaction_state;
    MIFARE_CardState_t card_state;
    PN532_CardInfo_t card_info;
    PN532_Handle_t *pn532_handle;  // PN532 driver handle
    SemaphoreHandle_t transaction_mutex;
    uint32_t last_card_update_time;
    bool transaction_active;
    uint8_t consecutive_errors;
    MIFARE_UserData_t last_written_user_data;
    bool has_last_written_snapshot;
    TickType_t pn532_recovery_until_tick;  // Tick count until PN532 is considered recovered after failed write
    
    // Stability layer - shield user from transient RF failures
    TickType_t last_successful_read_tick;      // Last time card was successfully read
    TickType_t last_successful_write_tick;     // Last time card was successfully written
    TickType_t card_first_detected_tick;       // When card was first detected (not yet confirmed)
    TickType_t card_confirmed_present_tick;    // When card presence was confirmed (stable for 1s)
    TickType_t card_first_lost_tick;           // When card first failed to read (not yet confirmed removed)
    bool card_presence_confirmed;              // True if card has been stable for 1s
    
    // Write failure tracking - only treat as card removal if writes fail continuously for >500ms
    TickType_t write_failure_first_tick;       // When write failures started (0 = no active failure period)
    uint8_t consecutive_write_failures;        // Count of consecutive write failures
    
    // Alternating write pattern - reduces I2C traffic and wear by 40%
    uint8_t write_cycle_counter;               // Alternates between 0 (write primary) and 1 (write backup)
    
    // Security context - encryption/authentication
    MIFARE_SecurityContext_t security_context;  // Per-card security context
} MIFARE_TransactionManager_t;

/**
 * @brief Transaction operation result
 */
typedef enum {
    MIFARE_RESULT_OK = 0,
    MIFARE_RESULT_ERROR,
    MIFARE_RESULT_CARD_REMOVED,
    MIFARE_RESULT_INSUFFICIENT_BALANCE,
    MIFARE_RESULT_CARD_CORRUPTED,
    MIFARE_RESULT_AUTHENTICATION_FAILED,
    MIFARE_RESULT_WRITE_FAILED,
    MIFARE_RESULT_DATA_MISMATCH,
    MIFARE_RESULT_TIMEOUT,
    MIFARE_RESULT_BUSY,
    MIFARE_RESULT_PN532_CORRUPTED,
} MIFARE_Result_t;



/*Macros -------------------------------------------------------------*/
#define MIFARE_CRC16_POLY               0x1021
#define MIFARE_CALCULATE_CRC16(data, len) MIFARE_Classic_CalculateCRC16((const uint8_t*)(data), len)

/*Function Prototypes ------------------------------------------------*/

/* Core Transaction Manager Functions */
MIFARE_Result_t MIFARE_TransactionManager_Init(void);
MIFARE_Result_t MIFARE_ProcessCardDetected(PN532_CardInfo_t *card_info);
MIFARE_Result_t MIFARE_ProcessCardRemoved(void);
MIFARE_Result_t MIFARE_ForceCardRemoval(void);  /* Force immediate removal (skip stability check) */
void MIFARE_NotifyPN532Reset(void);  /* Called after PN532 reset to set recovery cooldown */

/* Card Data Operations */
MIFARE_Result_t MIFARE_ReadCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_WriteCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_WriteCardDataFast(MIFARE_CardData_t *card_data);  /* Primary only, skip backup (faster) */
MIFARE_Result_t MIFARE_ValidateCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_RecoverCardData(MIFARE_CardData_t *card_data);

/* Low-Level Block Operations (for decryptcard and advanced operations) */
MIFARE_Result_t MIFARE_ReadBlock(uint8_t block_number, uint8_t *data);   /* Read single block (16 bytes) with decryption */
MIFARE_Result_t MIFARE_WriteBlock(uint8_t block_number, const uint8_t *data, bool allow_trailer);  /* Write single block (16 bytes) */

/* Card Initialization */
MIFARE_Result_t MIFARE_InitializeNewCustomerCard(uint32_t initial_balance_ml, uint64_t customer_id, bool force_factory_keys);
MIFARE_Result_t MIFARE_DetectAndAutoInitializeCard(const PN532_CardInfo_t *card_info, uint32_t default_balance_ml);

/* Balance Management (milliliters of water) */
MIFARE_Result_t MIFARE_TopupCardBalance(uint32_t topup_ml);

/* Atomic Transaction Operations - Generic card write operations */
MIFARE_Result_t MIFARE_BeginTransaction(void);      // Start atomic transaction
MIFARE_Result_t MIFARE_UpdateCardData(bool fast);   // Write current card data (fast=true skips backup)
MIFARE_Result_t MIFARE_CommitTransaction(void);
MIFARE_Result_t MIFARE_RollbackTransaction(void);
void mifare_recover_and_reinit_pn532(void);

/* Transaction State Management */
MIFARE_TransactionState_t MIFARE_GetTransactionState(void);
void MIFARE_SetTransactionState(MIFARE_TransactionState_t new_state);

/* State Transition Helpers - Use these instead of direct SetTransactionState */
void MIFARE_SetErrorState_CardCorrupted(void);     // Card blank/corrupted, needs cardinit
void MIFARE_SetErrorState_ModuleFailure(void);     // Hardware module failure (PN532, etc)
void MIFARE_SetErrorState_ValidationFailed(void);  // Card validation failed
void MIFARE_SetErrorState_WriteFailed(void);       // Transaction write failed

uint32_t MIFARE_GetBalanceMl(void);  // Get current card balance in milliliters
uint32_t MIFARE_GetLastTopupMl(void);  // Get last topup amount in milliliters
bool MIFARE_IsCardPresent(void);            /* STATE-BASED CHECK ONLY - use MIFARE_VerifyCardPresence() for hardware check */
bool MIFARE_IsCardPresenceConfirmed(void);  /* Returns true only if card stable for 1s */
bool MIFARE_IsWriteInProgress(void);        /* Returns true if write operation in progress (for polling skip) */
void MIFARE_UpdateStabilityCheck(void);      // Update stability timer (call periodically)
MIFARE_CardState_t MIFARE_GetCardState(void);
void MIFARE_SetCardState(MIFARE_CardState_t new_state);
void MIFARE_ConfirmReadyAfterPolling(void);
MIFARE_Result_t MIFARE_UpdateTransactionProgress(uint32_t additional_ml, float flow_rate_lpm);  // Update ongoing transaction


/* Card Monitoring and Safety */
MIFARE_Result_t MIFARE_VerifyCardPresence(void);  /* UNIFIED FUNCTION - Use this for ALL card presence checks */

/* Data Integrity Functions */
uint16_t mifare_calculate_crc16(uint8_t *data, uint16_t length);

/* Utility Functions */
const char* MIFARE_GetResultString(MIFARE_Result_t result);
const char* MIFARE_GetTransactionStateString(MIFARE_TransactionState_t state);
const char* MIFARE_GetCardStateString(MIFARE_CardState_t state);

/* Card Data Getter Functions - Simple accessors, no business logic */
MIFARE_UserData_t* MIFARE_GetUserData(void);           /* Get pointer to user data (balance_ml, etc) */
MIFARE_UsageData_t* MIFARE_GetUsageData(void);         /* Get pointer to usage data */
MIFARE_AccountData_t* MIFARE_GetAccountData(void);     /* Get pointer to account data */
bool MIFARE_IsCardReady(void);                         /* Returns true if card is present and ready */
uint8_t MIFARE_GetCardStatusFlags(void);               /* Returns card status flags byte */
bool MIFARE_GetCustomerPhoneNumber(char *phone_buffer, size_t buffer_size);  /* Get phone number from card */
bool MIFARE_GetCurrentCardInfo(PN532_CardInfo_t *info); /* Get current card info (UID, type, etc) */

/* Card Data Setter Functions - Business logic layer uses these to update card data */
void MIFARE_SetUserData(const MIFARE_UserData_t *user_data);
void MIFARE_SetUsageData(const MIFARE_UsageData_t *usage_data);

/* Debug and Logging Functions */
void MIFARE_LogTransaction(uint8_t type, uint16_t wash_duration_min, uint8_t wash_bay_id);

/* Card Polling Task */
void MIFARE_StartPollingTask(void);
void MIFARE_StopPollingTask(void);

#endif /* APPLICATION_INCLUDE_MIFARE_TRANSACTION_MANAGER_H_ */