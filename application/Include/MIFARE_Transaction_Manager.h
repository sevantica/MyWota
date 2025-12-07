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
#include <stdint.h>
#include <stdbool.h>

/*Defines ------------------------------------------------------------*/
#define MIFARE_MAGIC_BYTES              0x6D575441UL    // "mWTA" in little endian
#define MIFARE_FORMAT_VERSION           0x01
#define MIFARE_MAX_TRANSACTIONS         3               // Store last 3 transactions (Fits in 2 blocks: 9 & 10)
#define MIFARE_CARD_TIMEOUT_MS          2000           // Card must respond within 2s
#define MIFARE_MAX_RETRIES              3              // Maximum retry attempts
#define MIFARE_CARD_REMOVAL_FAIL_COUNT  3              // Consecutive failures to infer card removal
#define MIFARE_FAST_BALANCE_UPDATE_MS   500             // Fast balance cache update interval (500ms) - reduced I2C load
#define MIFARE_MAIN_DATA_UPDATE_MS      2000            // Main user data update interval (2000ms) - reduced I2C load
#define MIFARE_STABILITY_TIMEOUT_MS     50              // Card must be stable for 50ms before confirmed present
#define MIFARE_REMOVAL_STABILITY_MS     1000            // Card must be absent for 1000ms before confirmed removed (prevents false removals)

/* MIFARE Classic 1K Block Layout 
 * WARNING: Blocks 3, 7, 11, 15, 19, 23, 27, 31, etc. are SECTOR TRAILERS
 *          containing authentication keys and MUST NOT be written to!
 * 
 * Sector 1 (blocks 4-7):
 *   Block 4: Header
 *   Block 5: User Primary
 *   Block 6: User Backup  
 *   Block 7: SECTOR TRAILER (do not use)
 * 
 * Sector 2 (blocks 8-11):
 *   Block 8: Usage Data
 *   Block 9: Transaction Log (start)
 *   Block 10: Transaction Log
 *   Block 11: SECTOR TRAILER (do not use)
 * 
 * Sector 3 (blocks 12-15):
 *   Block 12: Recovery Info
 *   Block 13: Fast Balance Primary
 *   Block 14: Fast Balance Backup
 *   Block 15: SECTOR TRAILER (do not use)
 * 
 * Sector 4 (blocks 16-19):
 *   Block 16: Account Data (phone number, validity)
 *   Block 17: Reserved
 *   Block 18: Reserved
 *   Block 19: SECTOR TRAILER (do not use)
 */
#define MIFARE_BLOCK_HEADER             4              // Card header and system info
#define MIFARE_BLOCK_USER_PRIMARY       5              // Primary user data: balance, status, state (updated every 500ms)
#define MIFARE_BLOCK_USER_BACKUP        6              // Backup user data
#define MIFARE_BLOCK_USAGE_DATA         8              // Usage statistics: lifetime totals (MOVED from 7 to avoid sector trailer)
#define MIFARE_BLOCK_TRANSACTION_LOG    9              // Transaction log start (MOVED from 8)
#define MIFARE_BLOCK_RECOVERY_INFO      12             // Recovery and integrity data
#define MIFARE_BLOCK_FAST_BALANCE_PRIMARY   13         // Fast balance cache (updated every 250ms during dispense)
#define MIFARE_BLOCK_FAST_BALANCE_BACKUP    14         // Fast balance cache backup
#define MIFARE_BLOCK_ACCOUNT_DATA       16             // Account data: phone number, card validity

/* Transaction States */
#define TRANSACTION_STATE_IDLE          0x00
#define TRANSACTION_STATE_STARTED       0x01
#define TRANSACTION_STATE_IN_PROGRESS   0x02
#define TRANSACTION_STATE_COMMIT_READY  0x03
#define TRANSACTION_STATE_COMMITTED     0x04
#define TRANSACTION_STATE_ROLLBACK      0xFF

/* Card Status Flags */
#define CARD_STATUS_ACTIVE              0x01
#define CARD_STATUS_SUSPENDED           0x02
#define CARD_STATUS_MAINTENANCE         0x04
#define CARD_STATUS_LOW_BALANCE         0x08
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
 * Note: Size must be <= 16 bytes to fit in a single MIFARE Classic block
 * Stored in Block 5 (primary) and Block 6 (backup)
 * Updated: Supports 1000L capacity with uint32_t balance_ml
 */
typedef struct __attribute__((packed)) {
    uint32_t balance_ml;           // Current balance in milliliters (max ~4 billion mL = 4 million L)
    uint16_t last_topup_amount_ml; // Last topup amount (max 65,535 mL = 65.5L per topup)
    uint16_t transaction_counter;  // Incremented on each transaction
    uint8_t status_flags;          // Card status bits
    uint8_t transaction_state;     // Current transaction state
    uint8_t reserved[6];           // Reserved for future use
    // Total size: 4+2+2+1+1+6 = 16 bytes (perfect fit!)
} MIFARE_UserData_t;

/**
 * @brief Usage statistics - Lifetime totals and counters
 * Note: Size must be <= 16 bytes to fit in a single MIFARE Classic block
 * Stored in Block 7 (usage tracking)
 */
typedef struct __attribute__((packed)) {
    uint32_t total_purchased_ml;   // Total water purchased (lifetime, max ~4 billion mL)
    uint32_t total_dispensed_ml;   // Total water dispensed (lifetime, max ~4 billion mL)
    uint32_t reserved1;            // Reserved for future statistics
    uint32_t reserved2;            // Reserved for future statistics
    // Total size: 4+4+4+4 = 16 bytes (perfect fit!)
} MIFARE_UsageData_t;

/**
 * @brief Individual transaction record
 */
typedef struct __attribute__((packed)) {
    uint32_t timestamp;            // Unix timestamp
    uint16_t amount_ml;           // Amount in milliliters
    uint8_t transaction_type;     // 1=Purchase, 2=Dispense, 3=Refund
    uint8_t dispenser_id;         // Which dispenser was used
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
 * @brief Fast balance cache - Updated every 250ms during dispensing
 * Reduces write traffic to large user data blocks (which are updated every 500ms)
 * Size: 16 bytes (fits in one MIFARE Classic block)
 */
typedef struct __attribute__((packed)) {
    uint32_t balance_ml;           // Current balance (4 bytes) - supports up to ~4 billion mL = 4 million L
    uint16_t sequence_number;      // Incremental counter (2 bytes) - used to detect newer data
    uint16_t reserved1;            // Reserved for alignment (2 bytes)
    uint32_t timestamp;            // Last update time in ms (4 bytes)
    uint32_t crc32;                // CRC32 for integrity check (4 bytes)
    // Total: 16 bytes (full MIFARE block)
} MIFARE_FastBalance_t;

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
    MIFARE_UserData_t user_primary;        // Block 5: Current balance, status, state
    MIFARE_UserData_t user_backup;         // Block 6: Backup of user data
    MIFARE_UsageData_t usage_data;         // Block 8: Lifetime usage statistics
    MIFARE_TransactionLog_t transaction_log;
    MIFARE_RecoveryInfo_t recovery_info;
    MIFARE_FastBalance_t fast_balance_primary;
    MIFARE_FastBalance_t fast_balance_backup;
    MIFARE_AccountData_t account_data;     // Block 16: Account info (phone, validity)
    bool data_valid;
    uint32_t last_read_time;
} MIFARE_CardData_t;

/**
 * @brief Dispensing session state
 */
typedef enum {
    DISPENSE_STATE_IDLE,
    DISPENSE_STATE_CARD_DETECTED,
    DISPENSE_STATE_AUTHENTICATING,
    DISPENSE_STATE_READING_DATA,
    DISPENSE_STATE_VALIDATING,
    DISPENSE_STATE_READY_TO_DISPENSE,
    DISPENSE_STATE_DISPENSING,
    DISPENSE_STATE_UPDATING_CARD,
    DISPENSE_STATE_FINALIZING,
    DISPENSE_STATE_ERROR,
    DISPENSE_STATE_CARD_REMOVED,
    DISPENSE_STATE_CARD_REMOVED_DURING_DISPENSING
} MIFARE_DispenseState_t;

/**
 * @brief Transaction manager handle
 */
typedef struct {
    MIFARE_CardData_t current_card;
    MIFARE_DispenseState_t dispense_state;
    MIFARE_CardState_t card_state;
    PN532_CardInfo_t card_info;
    // PN532_Handle_t *pn532_handle; // Removed: Driver now manages handle internally
    SemaphoreHandle_t transaction_mutex;
    uint32_t dispense_start_time;
    uint32_t total_dispensed_this_session;
    uint32_t last_card_update_time;
    uint32_t last_fast_balance_update_time;  // Track fast balance cache updates (20ms interval)
    bool transaction_active;
    uint8_t consecutive_errors;
    MIFARE_UserData_t last_written_user_data;
    MIFARE_RecoveryInfo_t last_written_recovery_info;
    bool has_last_written_snapshot;
    volatile bool card_removal_abort;  // Flag to immediately abort all operations when card removed mid-operation
    TickType_t pn532_recovery_until_tick;  // Tick count until PN532 is considered recovered after failed write
    
    // Stability layer - shield user from transient RF failures
    TickType_t last_successful_read_tick;      // Last time card was successfully read
    TickType_t last_successful_write_tick;     // Last time card was successfully written
    TickType_t card_first_detected_tick;       // When card was first detected (not yet confirmed)
    TickType_t card_confirmed_present_tick;    // When card presence was confirmed (stable for 1s)
    TickType_t card_first_lost_tick;           // When card first failed to read (not yet confirmed removed)
    bool card_presence_confirmed;              // True if card has been stable for 1s
    bool ui_state_card_present;                // What we've told the UI (only changes after confirmation)
    
    // Write failure tracking - only treat as card removal if writes fail continuously for >500ms
    TickType_t write_failure_first_tick;       // When write failures started (0 = no active failure period)
    uint8_t consecutive_write_failures;        // Count of consecutive write failures
    
    // Alternating write pattern - reduces I2C traffic and wear by 40%
    uint8_t write_cycle_counter;               // Alternates between 0 (write primary) and 1 (write backup)
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
#define MIFARE_CALCULATE_CRC16(data, len) mifare_calculate_crc16((uint8_t*)(data), len)

/*Extern Variables ---------------------------------------------------*/
extern MIFARE_TransactionManager_t g_transaction_manager;

/*Function Prototypes ------------------------------------------------*/

/* Core Transaction Manager Functions */
MIFARE_Result_t MIFARE_TransactionManager_Init(void);
MIFARE_Result_t MIFARE_TransactionManager_DeInit(void);
MIFARE_Result_t MIFARE_ProcessCardDetected(PN532_CardInfo_t *card_info);
MIFARE_Result_t MIFARE_ProcessCardRemoved(void);
void MIFARE_NotifyPN532Reset(void);  /* Called after PN532 reset to set recovery cooldown */

/* Card Data Operations */
MIFARE_Result_t MIFARE_ReadCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_WriteCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_ValidateCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_RecoverCardData(MIFARE_CardData_t *card_data);

/* Card Initialization */
MIFARE_Result_t MIFARE_InitializeNewCustomerCard(uint32_t initial_balance_ml, uint64_t customer_id);
MIFARE_Result_t MIFARE_DetectAndAutoInitializeCard(const PN532_CardInfo_t *card_info, uint32_t default_balance_ml);

/* Balance Management */
MIFARE_Result_t MIFARE_TopupCardBalance(uint32_t topup_amount_ml);

/* Atomic Transaction Operations */
MIFARE_Result_t MIFARE_BeginTransaction(uint32_t amount_ml);
MIFARE_Result_t MIFARE_UpdateTransactionProgress(uint32_t dispensed_ml, float flow_rate_lpm);
MIFARE_Result_t MIFARE_CommitTransaction(void);
MIFARE_Result_t MIFARE_RollbackTransaction(void);
void mifare_recover_and_reinit_pn532(void);

/* Dispensing State Management */
MIFARE_Result_t MIFARE_StartDispensing(uint16_t requested_amount_ml);
MIFARE_Result_t MIFARE_UpdateDispensing(uint16_t dispensed_ml);
MIFARE_Result_t MIFARE_StopDispensing(void);
MIFARE_DispenseState_t MIFARE_GetDispenseState(void);
bool MIFARE_IsCardPresent(void);
bool MIFARE_IsCardPresenceConfirmed(void);  // Returns true only if card stable for 1s
void MIFARE_UpdateStabilityCheck(void);      // Update stability timer (call periodically)
MIFARE_CardState_t MIFARE_GetCardState(void);
void MIFARE_SetCardState(MIFARE_CardState_t new_state);
void MIFARE_ConfirmReadyAfterPolling(void);


/* Card Monitoring and Safety */
MIFARE_Result_t MIFARE_MonitorCardPresence(void);
MIFARE_Result_t MIFARE_PerformPeriodicUpdate(void);
MIFARE_Result_t MIFARE_HandleCardRemovalDuringDispense(void);

/* Data Integrity Functions */
uint16_t mifare_calculate_crc16(uint8_t *data, uint16_t length);
bool mifare_verify_data_integrity(MIFARE_CardData_t *card_data);
MIFARE_Result_t mifare_create_backup(MIFARE_CardData_t *card_data);
MIFARE_Result_t mifare_restore_from_backup(MIFARE_CardData_t *card_data);

/* Utility Functions */
const char* MIFARE_GetResultString(MIFARE_Result_t result);
const char* MIFARE_GetStateString(MIFARE_DispenseState_t state);
uint32_t MIFARE_GetBalanceML(void);
uint32_t MIFARE_GetLastTopupAmountML(void);
uint32_t MIFARE_GetTotalDispensedThisSession(void);

/* Debug and Logging Functions */
void MIFARE_PrintCardData(MIFARE_CardData_t *card_data);
void MIFARE_PrintTransactionLog(MIFARE_TransactionLog_t *log);
void MIFARE_LogTransaction(uint8_t type, uint16_t amount_ml, uint8_t dispenser_id);

#endif /* APPLICATION_INCLUDE_MIFARE_TRANSACTION_MANAGER_H_ */