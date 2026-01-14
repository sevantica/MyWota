/**
 ******************************************************************************
 * @file    SD_Logger_Task.h
 * @brief   SD Card Logger Task Header - State machine based initialization and logging
 * 
 * @attention
 * Copyright (c) Sevantica 2025
 * 
 ******************************************************************************
 */

#ifndef SD_LOGGER_TASK_H
#define SD_LOGGER_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "ff.h"

/* Configuration -------------------------------------------------------------*/
#define SD_LOG_QUEUE_LENGTH     8       // Number of pending log messages
#define SD_LOG_MSG_MAX_LEN      120     // Maximum log message length

/* Exported types ------------------------------------------------------------*/

/**
 * @brief SD Log Entry Types
 */
typedef enum {
    SD_LOG_TYPE_SYSTEM_STARTUP,         // System startup event
    SD_LOG_TYPE_MIFARE_CARD_SCAN,       // MIFARE card detected and scanned
    SD_LOG_TYPE_TRANSACTION,            // Transaction event
    SD_LOG_TYPE_ERROR,                  // Error event
    SD_LOG_TYPE_EVENT                   // General event (SD_Logger_LogEvent)
} SDLogType_t;

/**
 * @brief SD Logger Task States
 */
typedef enum {
    SD_LOGGER_STATE_STARTUP,        // Initial power-on state
    SD_LOGGER_STATE_INIT_SD,        // Initializing SD card hardware
    SD_LOGGER_STATE_MOUNT_FS,       // Mounting FAT filesystem
    SD_LOGGER_STATE_READY,          // Ready for logging operations
    SD_LOGGER_STATE_ERROR,          // Error state
    SD_LOGGER_STATE_RETRY           // Retry initialization after error
} SDLoggerState_t;

/**
 * @brief Log message structure for async queue
 */
typedef struct {
    uint32_t timestamp;                     // Tick count when queued
    char message[SD_LOG_MSG_MAX_LEN];       // Pre-formatted message
} SDLogQueueMsg_t;

/**
 * @brief Transaction Log Data (Passed to Adapter)
 */
typedef struct {
    char timestamp_str[32];
    uint8_t uid[7];
    uint32_t initial_balance;
    uint32_t final_balance;
    uint32_t consumed_amount;
    uint32_t duration_seconds;
} SD_Log_Transaction_Data_t;

/**
 * @brief Card Scan Log Data (Passed to Adapter)
 */
typedef struct {
    char timestamp_str[32];
    uint8_t uid[7];
    int result_code;
    uint32_t balance;
} SD_Log_CardScan_Data_t;

/**
 * @brief System Event Log Data (Passed to Adapter)
 */
typedef struct {
    char timestamp_str[32];
    uint32_t event_id;
    uint32_t data1;
    uint32_t data2;
} SD_Log_System_Data_t;

/* Exported functions --------------------------------------------------------*/

/**
 * @brief Start SD Logger Task
 * @note Creates FreeRTOS task for SD card initialization and logging
 */
bool Task_Start_SD_Logger_Task(void);

/**
 * @brief Log MIFARE card scan with all block data to SD card
 * @param card_uid Card unique identifier
 * @param uid_length Length of card UID (4 or 7 bytes)
 * @param card_data Pointer to MIFARE card data structure
 * @param log_type Type of log entry
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogMIFARECardScan(const uint8_t *card_uid, uint8_t uid_length, 
                                 const void *card_data, SDLogType_t log_type);

/**
 * @brief Log a system event to the system log file
 * @param format Printf-style format string
 * @param ... Variable arguments
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogEvent(const char *format, ...);

/**
 * @brief Log a transaction event (start, progress, complete, abort)
 * @param card_uid Card unique identifier
 * @param uid_length Length of card UID
 * @param event_type Event description string
 * @param balance_before Balance before event (in mL)
 * @param balance_after Balance after event (in mL)
 * @param amount Amount involved in transaction (in mL)
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogTransaction(const uint8_t *card_uid, uint8_t uid_length,
                              const char *event_type,
                              uint32_t balance_before, uint32_t balance_after,
                              uint32_t amount);

/**
 * @brief Log an error event
 * @param module Module name where error occurred
 * @param error_code Error code
 * @param description Error description
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogError(const char *module, int error_code, const char *description);

/**
 * @brief Check if SD logger is ready for logging operations
 * @return true if ready, false otherwise
 */
bool SD_Logger_IsReady(void);

/**
 * @brief Print card transaction log to USB terminal
 * @param card_uid Card unique identifier (hex string or bytes)
 * @param uid_length Length of card UID (4 or 7 bytes)
 * @return true if log was printed successfully, false otherwise
 */
bool SD_Logger_PrintCardLog(const uint8_t *card_uid, uint8_t uid_length);

/**
 * @brief Get last known balance from SD card transaction logs
 * @param card_uid Card unique identifier (4 or 7 bytes)
 * @param uid_length Length of card UID
 * @param balance_out Pointer to store recovered balance (in milliliters)
 * @return true if balance found, false otherwise
 * @note Parses the card log file to find the latest balance entry
 */
bool SD_Logger_GetLastBalance(const uint8_t *card_uid, uint8_t uid_length, uint32_t *balance_out);

/**
 * @brief Suspend SD logging for USB MSC mode
 * @note Stops the logging task from writing to SD card
 *       Must be called before unmounting FatFs for MSC
 */
void SD_Logger_SuspendLogging(void);

/**
 * @brief Resume SD logging after USB MSC mode
 * @note Resumes the logging task after FatFs is remounted
 */
void SD_Logger_ResumeLogging(void);

/**
 * @brief Get the FatFs object used by SD Logger
 * @return Pointer to FATFS object, or NULL if not mounted
 */
FATFS* SD_Logger_GetFatFs(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_LOGGER_TASK_H */
